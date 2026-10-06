#include "check.hpp"
#include <saga/search.hpp>
#include <atomic>
#include <thread>
using namespace saga;
namespace {
WebResponse response(std::string url,std::string body,long status=200) {
  WebResponse r;r.url=std::move(url);r.body=std::move(body);r.status=status;return r;
}
std::string directory_html(const std::vector<std::string> &origins) {
  std::string body="<a href='https://unrelated.test'>Repo</a><noscript><table><tr><th>Server</th><th>API</th></tr>";
  for(auto &origin:origins)body+="<tr><td class='expand'><a href='"+origin+"'>Server</a></td><td>unknown</td></tr>";
  return body+"</table></noscript>";
}
std::string probe_json(bool api=true,int bot=0,int version=9) {
  return Json{{"status","ok"},{"service","4get"},{"server",{{"api_enabled",api},{"bot_protection",bot},{"version",version},{"name",nullptr}}},{"instances",Json::array()}}.dump();
}
std::string hits_json(std::string npt="") {
  return Json{{"status","ok"},{"web",Json::array({{{"url","https://docs.example.com/rotation"},{"title","Rotation"},{"description",nullptr}},{{"url","https://docs.example.com/rotation"},{"title","duplicate"}},{{"url","http://127.0.0.1/private"},{"title","blocked"}}})},{"npt",npt.empty() ? Json() : Json(npt)}}.dump();
}
struct FakeClock {
  std::atomic<long> ms{0};
  SearchClock clock() {return {[&]{return SearchTime{}+SearchDuration(ms.load());},[&](SearchDuration d){ms+=d.count();std::this_thread::yield();}};}
  SearchContext context() {SearchContext c;c.human_initiated=true;c.clock=clock();return c;}
};
void limiter() {
  FakeClock clock;auto c=clock.context();SearchRateLimiter gate;gate.wait(c);CHECK(clock.ms==0);gate.wait(c);CHECK(clock.ms>=1000);
  SearchRateLimiter concurrent;std::vector<long> starts;std::mutex mutex;
  std::vector<std::thread> threads;
  for(int i=0;i<3;++i)threads.emplace_back([&]{concurrent.wait(c);std::lock_guard guard(mutex);starts.push_back(clock.ms.load());});
  for(auto &t:threads)t.join();
  CHECK(starts.size()==3);CHECK(clock.ms>=3000); // Gate admission, not callback scheduling, defines start spacing.
  gate.degrade(c);bool blocked=false;try{gate.wait(c);}catch(const SearchError &e){blocked=e.code==SearchErrorCode::RateLimited;}CHECK(blocked);
  c.service=[] {throw TurnCancelled();};bool cancelled=false;try{gate.wait(c);}catch(const TurnCancelled &){cancelled=true;}CHECK(cancelled);
  FakeClock ddg_time;auto context=ddg_time.context();std::vector<long> requests;
  DuckDuckGoEngine ddg([&](auto &url,auto&,auto&){requests.push_back(ddg_time.ms);return response(url,"<p class='no-results'>No results found</p>");});
  ddg.run({"first","",5,{},{}},context);ddg.run({"second","",5,{},{}},context);CHECK(requests[1]-requests[0]>=1000);
  DuckDuckGoEngine challenge([&](auto &url,auto&,auto&){return response(url,"<form id='challenge-form'>",202);});
  bool bot=false;try{challenge.run({"query","",5,{},{}},context);}catch(const SearchError &e){bot=e.code==SearchErrorCode::BotChallenge;}CHECK(bot);
  rejects([&]{SearchContext not_human;ddg.run({"query","",5,{},{}},not_human);});
}
void parsing() {
  auto seeds=FourGetSearchEngine::directory(response("https://4get.ca/instances",directory_html({"https://a.example/","HTTPS://A.example:443/","https://b.example","http://127.0.0.1/","http://localhost/","http://10.2.3.4/","https://user:pass@bad.example/","https://b.example/path","javascript:evil"})));
  CHECK(seeds==std::vector<std::string>({"https://a.example","https://b.example"}));
  auto healthy=FourGetSearchEngine::probe(response("https://a.example/ami4get",probe_json()),"https://a.example");
  SearchContext c;c.human_initiated=true;CHECK(healthy.eligible(c));
  auto disabled=FourGetSearchEngine::probe(response("https://a.example/ami4get",probe_json(false)),"https://a.example");CHECK(!disabled.eligible(c));
  auto protected_instance=FourGetSearchEngine::probe(response("https://a.example/ami4get",probe_json(true,1)),"https://a.example");CHECK(!protected_instance.eligible(c));
  for(auto body:{std::string("not json"),std::string("{\"status\":\"no\"}"),std::string("{\"status\":\"ok\",\"service\":\"other\"}"),std::string("{\"status\":\"ok\",\"service\":\"4get\",\"server\":{}}"),probe_json(true,0,0)})rejects([&]{FourGetSearchEngine::probe(response("https://a.example/ami4get",body),"https://a.example");});
  SearchRequest request{"SRS","",5,{"example.com"},{}};
  auto hits=FourGetSearchEngine::parse(response("https://a.example/api/v1/web",hits_json("OPAQUE+TOKEN")),request,"https://a.example");CHECK(hits["results"].size()==1);CHECK(hits["results"][0]["snippet"]=="");CHECK(Json::parse(hits["next_cursor"].get<std::string>())["instance"]=="https://a.example");
  // HTTP 200 is not API success.
  rejects([&]{FourGetSearchEngine::parse(response("https://a.example/api/v1/web","{\"status\":\"The server administrator disabled the API!\"}"),request,"https://a.example");});
  auto slow=healthy;healthy.latency_ms=80;slow.latency_ms=200;CHECK(healthy.score(false)>slow.score(false));
  slow.retry_after=c.clock.time()+SearchDuration(1000);CHECK(!slow.eligible(c));
}
void failover_and_pagination() {
  Config config;config.fourget_max_instance_attempts=6;FakeClock clock;auto context=clock.context();
  std::vector<std::string> requested;int directories=0;
  FourGetSearchEngine engine(config,[&](auto &url,auto&,auto& control){
    if(control)control();
    requested.push_back(url);
    if(url=="https://4get.ca/instances") {++directories;return response(url,directory_html({"https://a.example","https://b.example","https://c.example","https://protected.example"}));}
    if(url.find("/ami4get")!=std::string::npos)return response(url,probe_json(true,url.find("protected")!=std::string::npos ? 1 : 0));
    if(url.starts_with("https://a.example"))throw SearchError(SearchErrorCode::Timeout,"fixture timeout");
    if(url.starts_with("https://b.example"))return response(url,"{\"status\":\"upstream error\"}");
    return response(url,hits_json("opaque token"));
  });
  SearchRequest request{"rotation","",5,{},{}};auto result=engine.run(request,context);CHECK(result["results"].size()==1);CHECK(directories==1);
  auto first_count=requested.size();request.cursor=result["next_cursor"];engine.run(request,context);
  CHECK(requested.size()==first_count+1);CHECK(requested.back()=="https://c.example/api/v1/web?npt=opaque%20token");
  request.cursor.clear();engine.run(request,context);CHECK(directories==1);
  int a_queries=0,b_queries=0;for(auto &url:requested){if(url.starts_with("https://a.example/api"))++a_queries;if(url.starts_with("https://b.example/api"))++b_queries;}CHECK(a_queries==1 && b_queries==1);
  auto health=engine.health();for(auto &instance:health)if(instance["origin"]=="https://a.example")CHECK(instance["failures"]==1);
  // Expiring a circuit permits a single lazy health probe, without visiting all instances.
  clock.ms+=120000;config.fourget_manual_instances={"https://single.example"};
  int calls=0;bool fail=true;
  FourGetSearchEngine circuit(config,[&](auto &url,auto&,auto&){++calls;if(url.ends_with("/instances"))return response(url,directory_html({"https://single.example"}));if(url.ends_with("/ami4get"))return response(url,probe_json());if(fail)throw SearchError(SearchErrorCode::Timeout,"timeout");return response(url,hits_json());});
  rejects([&]{circuit.run(request,context);});auto before=calls;rejects([&]{circuit.run(request,context);});CHECK(calls==before);clock.ms+=61000;fail=false;CHECK(circuit.run(request,context)["results"].size()==1);CHECK(circuit.health()[0]["failures"]==0);
  auto saved=circuit.health();context.service=[] {throw TurnCancelled();};bool cancelled=false;try{circuit.run(request,context);}catch(const TurnCancelled &){cancelled=true;}CHECK(cancelled);CHECK(circuit.health()==saved);
  int api=0;config.fourget_manual_instances.clear();FourGetSearchEngine protected_engine(config,[&](auto &url,auto&,auto&){if(url.ends_with("/instances"))return response(url,directory_html({"https://protected.example"}));if(url.ends_with("/ami4get"))return response(url,probe_json(true,2));++api;return response(url,hits_json());});context.service={};rejects([&]{protected_engine.run(request,context);});CHECK(api==0);
}
void concurrent_single_flight() {
  Config config;FakeClock clock;auto context=clock.context();int directories=0,probes=0,queries=0;
  FourGetSearchEngine engine(config,[&](auto &url,auto&,auto&){
    if(url.ends_with("/instances")) {++directories;return response(url,directory_html({"https://a.example"}));}
    if(url.ends_with("/ami4get")) {++probes;return response(url,probe_json());}
    ++queries;return response(url,hits_json());
  });
  std::vector<std::thread> threads;
  for(int i=0;i<4;++i)threads.emplace_back([&]{engine.run({"query","",5,{},{}},context);});
  for(auto &thread:threads)thread.join();
  CHECK(directories==1 && probes==1 && queries==4);
  auto limiter=std::make_shared<SearchRateLimiter>();std::vector<long> starts;
  auto transport=[&](auto &url,auto&,auto&){starts.push_back(clock.ms);return response(url,"<p class='no-results'>No results found</p>");};
  DuckDuckGoEngine first(transport,limiter),second(transport,limiter);
  std::thread a([&]{first.run({"A","",5,{},{}},context);});
  std::thread b([&]{second.run({"B","",5,{},{}},context);});a.join();b.join();
  CHECK(starts.size()==2 && starts[1]-starts[0]>=1000);
  // A one-use page token is never sent to a replacement instance.
  bool failed=false;config.fourget_manual_instances={"https://a.example","https://b.example"};
  std::vector<std::string> urls;
  FourGetSearchEngine paged(config,[&](auto &url,auto&,auto&){urls.push_back(url);
    if(url.ends_with("/instances"))return response(url,directory_html({"https://a.example","https://b.example"}));
    if(url.ends_with("/ami4get"))return response(url,probe_json());
    if(failed)throw SearchError(SearchErrorCode::Timeout,"fixture");
    return response(url,hits_json("once"));
  });
  SearchRequest request{"query","",5,{},{}};auto page=paged.run(request,context);request.cursor=page["next_cursor"];failed=true;
  bool unavailable=false;try{paged.run(request,context);}catch(const SearchError &e){unavailable=e.code==SearchErrorCode::PaginationUnavailable;}CHECK(unavailable);
  CHECK(std::none_of(urls.begin(),urls.end(),[](auto &url){return url.starts_with("https://b.example/api");}));
}
class Stub final : public SearchEngine {
public:
  std::string id;bool fail;int calls=0;
  Stub(std::string value,bool failure):id(std::move(value)),fail(failure){}
  Json capabilities() const override{return {{"id",id}};}
  std::string guidance() const override{return "fixture";}
  Json run(const SearchRequest &request,const SearchContext &c) override {c.check();++calls;if(fail)throw SearchError(SearchErrorCode::BotChallenge,"fixture challenge");return {{"engine",id},{"query",request.query},{"submitted_query",request.query},{"results",Json::array()},{"empty",true}};}
};
void orchestration() {
  auto root=fs::temp_directory_path()/("saga-search-"+uuid());private_dir(root);
  struct Cleanup{fs::path root;~Cleanup(){fs::remove_all(root);}} cleanup{root};
  Database db(root/"test.sqlite");db.migrate();Config config;config.search_engines={"duckduckgo","fourget"};SearchOrchestrator orchestrator(db,config);
  auto ddg=std::make_unique<Stub>("duckduckgo",true);auto fourget=std::make_unique<Stub>("fourget",false);auto *d=ddg.get();auto *f=fourget.get();orchestrator.registry().add(std::move(ddg));orchestrator.registry().add(std::move(fourget));
  FakeClock clock;auto context=clock.context();int fallback=0;context.emit=[&](auto &type,auto&){if(type=="search.fallback")++fallback;};
  SearchRequest request{"test","",5,{},{}};CHECK(orchestrator.search(request,context)["engine"]=="fourget");CHECK(d->calls==1 && f->calls==1 && fallback==1);
  CHECK(orchestrator.search(request,context)["engine"]=="fourget");CHECK(d->calls==1 && f->calls==1); // Cached result bypasses degraded engine/network.
  context.service=[] {throw TurnCancelled();};bool cancelled=false;try{orchestrator.search(request,context);}catch(const TurnCancelled &){cancelled=true;}CHECK(cancelled);
  context.service={};f->fail=true;request.query="different";rejects([&]{orchestrator.search(request,context);});
}
}
int main(){try{limiter();parsing();failover_and_pagination();concurrent_single_flight();orchestration();return 0;}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
