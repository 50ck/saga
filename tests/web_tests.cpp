#include "check.hpp"
#include <saga/web.hpp>
#include <saga/persona.hpp>
#include <saga/runtime.hpp>
using namespace saga;
namespace {
WebResponse page(std::string body,long status=200) {WebResponse r;r.url="https://html.duckduckgo.com/html/";r.body=std::move(body);r.content_type="text/html";r.status=status;return r;}
const char* search_html=R"HTML(<html><div class="result"><a class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fdocs.example.com%2Frules%3Fx%3D1%26y%3D2">Rules &amp; examples</a><a class="result__snippet">Version 2 rotation rules.</a></div><div class="result"><a class="result__a" href="https://spam.test/rules">Spam</a><p class="result__snippet">Unrelated neighboring snippet.</p></div><div class="result"><a class="result__a" href="https://docs.example.com/rules?x=1&amp;y=2">Duplicate</a></div><form><input type="hidden" name="q" value="rules"><input type="hidden" name="s" value="30"><input type="hidden" name="vqd" value="synthetic-token"></form></html>)HTML";
void parsing() {
  SearchRequest query{"site:example.com \"rotation rules\" +modern -old","",5,{"example.com"},{}};
  auto result=DuckDuckGoEngine::parse(page(search_html),query);
  CHECK(result["results"].size()==1);CHECK(result["results"][0]["snippet"]=="Version 2 rotation rules.");CHECK(result["results"][0]["url"]=="https://docs.example.com/rules?x=1&y=2");CHECK(result["results"][0]["title"]=="Rules & examples");CHECK(result.contains("next_cursor"));
  query.exclude_domains={"docs.example.com"};CHECK(DuckDuckGoEngine::parse(page(search_html),query)["empty"]==true);
  CHECK(DuckDuckGoEngine::parse(page("<p class='no-results'>No results found</p>"),query)["empty"]==true);
  rejects([&]{DuckDuckGoEngine::parse(page("<p>Unexpected markup</p>"),query);});
  rejects([&]{DuckDuckGoEngine::parse(page("<form id='challenge-form'>Challenge</form>",202),query);});
  rejects([&]{DuckDuckGoEngine::parse(page("",429),query);});
  auto lite=DuckDuckGoEngine::parse(page("<table><tr><td><a class='result-link' href='https://docs.example.com/a'>A</a></td></tr><tr><td class='result-snippet'>Snippet A</td></tr><tr><td><a class='result-link' href='https://docs.example.com/b'>B</a></td></tr><tr><td class='result-snippet'>Snippet B</td></tr></table>"),SearchRequest{"rules","",5,{},{}});
  CHECK(lite["results"][0]["snippet"]=="Snippet A");CHECK(lite["results"][1]["snippet"]=="Snippet B");
  CHECK(web_encode("\"東京\" site:example.com +x -y").find("%22%E6%9D%B1%E4%BA%AC%22")!=std::string::npos);
  auto doc=extract_web_document(page("<html><head><title>Docs</title></head><nav>unwanted</nav><main><h1>Rules</h1><p>Read <b>bold</b> and <code>code</code>.</p><pre>if (x) {\n  y();\n}</pre><table><tr><th>A</th><td>B</td></tr></table><script>evil</script><p hidden>secret</p><a href='/guide'>Guide</a></main></html>"));
  CHECK(doc.title=="Rules");CHECK(doc.text.find("Read bold and `code`.")!=std::string::npos);CHECK(doc.text.find("\n  y();\n")!=std::string::npos);CHECK(doc.text.find("A | B |")!=std::string::npos);CHECK(doc.text.find("unwanted")==std::string::npos);CHECK(doc.text.find("evil")==std::string::npos);CHECK(doc.text.find("secret")==std::string::npos);CHECK(doc.text.find("https://html.duckduckgo.com/guide")!=std::string::npos);
  CHECK(extract_web_document(page("<aside>noise</aside><div role='main'><p>Actual documentation.</p></div>")).text.find("Actual documentation.")!=std::string::npos);
  rejects([]{extract_web_document(page("<main>Enable JavaScript to continue</main>"));});
  rejects([]{extract_web_document(page("<script>window._cf_chl_opt={}</script><p>Challenge</p>"));});
  auto plain=page(std::string(300000,'x'));plain.content_type="text/plain";rejects([&]{extract_web_document(plain);});
  plain.content_type="application/pdf";rejects([&]{extract_web_document(plain);});plain.body=std::string(3*1024*1024,'x');rejects([&]{extract_web_document(plain);});
}
void boundaries() {
  for(auto& tool:Tools::definitions())CHECK(tool["function"]["parameters"]["required"].is_array());
  for(auto* ip:{"127.0.0.1","10.1.2.3","169.254.169.254","100.100.100.200","172.16.0.1","192.168.1.1","198.18.0.1","224.0.0.1","::1","fe80::1","fc00::1","::ffff:127.0.0.1","2001:db8::1","2002:7f00:1::"})CHECK(!public_web_address(ip));
  CHECK(public_web_address("8.8.8.8"));CHECK(public_web_address("2606:4700:4700::1111"));
  for(auto* url:{"file:///etc/passwd","http://localhost/","http://127.1/","http://2130706433/","http://[::1]/","https://user:password@example.com/","http://169.254.169.254/latest","http://x.local/","https://example.com/\nfoo"})rejects([&]{web_url(url);});
  CHECK(web_url("HTTPS://DOCS.Example.COM:443/page#anchor")=="https://docs.example.com/page");
  rejects([]{web_url("http://localhost./");});
  rejects([]{fetch_public_web("https://example.com/","",[]{throw TurnCancelled();});});
  CHECK(web_url("/doc#section","https://example.com/base")=="https://example.com/doc");
  rejects([]{fetch_public_web("http://127.0.0.1/","",{});});
  rejects([]{make_search_engine("unavailable");});
}
void adapter() {
  int calls=0;std::string observed_form;
  DuckDuckGoEngine engine([&](const std::string& url,const std::string& form,const std::function<void()>& control){if(control)control();++calls;observed_form=form;auto response=page(search_html);response.url=url;return response;});
  SearchRequest query{"\"rotation rules\" site:example.com","",5,{"example.com"},{}};
  auto first=engine.search(query,{});CHECK(observed_form=="q="+web_encode(query.query));query.cursor=first["next_cursor"];engine.search(query,{});CHECK(observed_form.find("s=30")!=std::string::npos);CHECK(calls==2);
  query.query="changed";rejects([&]{engine.search(query,{});});query.cursor="";query.query="!g rules";rejects([&]{engine.search(query,{});});query.query="\\rules";rejects([&]{engine.search(query,{});});query.query="rules";query.include_domains={"https://example.com"};rejects([&]{engine.search(query,{});});
  int fallback=0;DuckDuckGoEngine fallback_engine([&](auto& url,auto&,auto&){auto response=page(search_html,++fallback==1 ? 503:200);response.url=url;return response;});query.include_domains.clear();fallback_engine.search(query,{});CHECK(fallback==2);
  DuckDuckGoEngine challenge_engine([&](auto&,auto&,auto&){++fallback;return page("challenge",202);});rejects([&]{challenge_engine.search(query,{});});CHECK(fallback==3);
}
void storage() {
  auto root=fs::temp_directory_path()/("saga-web-tests-"+uuid());
  struct Cleanup{fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{root};
  Paths paths{root/"config",root/"data",root/"state",root/"run"};paths.create();private_dir(root/"project");Registry registry(paths);auto meta=registry.create("Synthetic research persona","");PersonaContext persona(paths,meta,root/"project");
  Config config;config.endpoint="http://example.invalid";config.model="synthetic";config.context_length=65536;config.save(paths);CHECK(Config::load(paths).search_engine=="duckduckgo");CHECK(Config::load(paths).web_read_limit==8);auto invalid_config=config;invalid_config.search_engine="unavailable";rejects([&]{invalid_config.validate();});
  auto session=persona.db->exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()});
  int network=0;WebResearch web(*persona.db,config,[&](const std::string& url,const std::string&,const std::function<void()>& service){++network;if(service)service();auto response=page(url.find("duckduckgo")!=std::string::npos ? search_html : "<main><h1>Modern rules</h1><p>Rotation is clockwise.</p></main>");response.url=url;return response;});
  web.begin_turn(session,0);auto search=web.dispatch("web_search",{{"query","rules"}},session,0);auto snippet=search["results"][0]["source_id"];
  auto q=web.question("How does rotation work?",true,session,0);rejects([&]{web.dispatch("research_resolve",{{"id",q},{"status","supported"},{"conclusion","Clockwise"},{"sources",Json::array({{{"source_id",snippet},{"quote","Version 2 rotation rules."}}})}},session,0);});
  auto doc=web.dispatch("web_read",{{"source_id",snippet}},session,0);auto id=doc["source_id"];CHECK(doc["kind"]=="document");CHECK(doc["content"].get<std::string>().find("Rotation is clockwise.")!=std::string::npos);
  web.dispatch("research_resolve",{{"id",q},{"status","supported"},{"conclusion","Clockwise"},{"sources",Json::array({{{"source_id",id},{"quote","Rotation is clockwise."}}})}},session,0);CHECK(web.unresolved_required(session,0).empty());
  rejects([&]{web.dispatch("research_resolve",{{"id",q},{"status","supported"},{"conclusion","Fabrication"},{"sources",Json::array({{{"source_id",id},{"quote","invented quote"}}})}},session,0);});
  rejects([&]{persona.db->exec("UPDATE web_sources SET text='tampered' WHERE id=?",{id});});
  web.settings(false);auto cached=web.dispatch("web_read",{{"source_id",id}},session,0);CHECK(cached["source_id"]==id);CHECK(network==2);rejects([&]{web.dispatch("web_search",{{"query","rules"}},session,0);});CHECK(network==2);
  web.settings(true);web.service([&]{web.settings(false);});rejects([&]{web.dispatch("web_read",{{"url","https://example.com/new"}},session,0);});CHECK(network==2);
  PersonaContext other(paths,registry.create("Other synthetic persona",""),root/"project");CHECK(other.db->query("SELECT id FROM web_sources").empty());
  persona.db->migrate();CHECK(persona.db->query("SELECT version FROM schema_version")[0]["version"]==7);
}
void proof_and_failure_gates() {
  auto root=fs::temp_directory_path()/("saga-web-proof-"+uuid());
  struct Cleanup{fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{root};
  Paths paths{root/"config",root/"data",root/"state",root/"run"};paths.create();private_dir(root/"project");Registry registry(paths);PersonaContext p(paths,registry.create("Evidence fixture",""),root/"project");
  Config c;c.model="fixture";c.endpoint="http://example.invalid";c.context_length=65536;
  Memory memory(p,c);int approvals=0,fetches=0;bool transferring=false;
  Tools tools(p,memory,[&](auto&,auto&){++approvals;return true;},c,[&](auto& url,auto&,auto& service){++fetches;struct Reset{bool& value;~Reset(){value=false;}} reset{transferring};transferring=true;if(service)service();auto r=page("<main><h1>Rules</h1><p>Rotation is clockwise.</p></main>");r.url=url;return r;});
  p.project=p.db->exec("INSERT INTO projects(name,root_path,created_at,last_seen_at) VALUES(?,?,?,?)",{"Fixture",p.project_root.string(),now(),now()});
  p.session=p.db->exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()});
  tools.permissions("always_approve");tools.execute("task_create",{{"title","Fixture"},{"objective","Research and implement"},{"risk","low"},{"checks",Json::array({"Program works"})}});
  tools.web().begin_turn(p.session,p.task);auto q=tools.web().question("Rotation?",true,p.session,p.task);
  auto premature=tools.execute("file_write",{{"path","early.txt"},{"content","premature"},{"description","fixture"}});CHECK(premature.contains("error"));CHECK(!fs::exists(root/"project/early.txt"));
  auto result=tools.execute("web_read",{{"url","https://docs.example.com/rules"}});CHECK(!result.contains("error"));CHECK(approvals==0 && fetches==1);
  auto checks=p.db->query("SELECT id FROM task_checks WHERE task_id=?",{p.task});auto check=checks[0]["id"];
  auto bad=tools.execute("check_resolve",{{"check_id",check},{"source_event_id",result["source_event_id"]},{"passed",true},{"explanation","Downloaded docs"},{"quote","Rotation is clockwise."}});CHECK(bad.contains("error"));CHECK(p.db->query("SELECT status FROM task_checks WHERE id=?",{check})[0]["status"]=="unresolved");
  auto research_check=tools.execute("task_add_check",{{"description","Find documented rotation direction"},{"kind","research"}})["check_id"];
  auto good=tools.execute("check_resolve",{{"check_id",research_check},{"source_event_id",result["source_event_id"]},{"passed",true},{"explanation","Documented rule"},{"quote","Rotation is clockwise."}});CHECK(!good.contains("error"));
  auto resolve=tools.execute("research_resolve",{{"id",q},{"status","supported"},{"conclusion","The source documents clockwise rotation."},{"sources",Json::array({{{"source_id",result["source_id"]},{"quote","Rotation is clockwise."}}})}});CHECK(!resolve.contains("error"));
  auto fact=tools.execute("learn_fact",{{"subject","rotation"},{"predicate","direction"},{"object","clockwise"},{"source_event_id",result["source_event_id"]}});CHECK(!fact.contains("error"));double before=memory.confidence("fact",fact["id"]);
  auto repeat=tools.execute("web_read",{{"url","https://docs.example.com/rules"},{"refresh",true}});tools.execute("learn_fact",{{"subject","rotation"},{"predicate","direction"},{"object","clockwise"},{"source_event_id",repeat["source_event_id"]}});CHECK(memory.confidence("fact",fact["id"])==before);
  auto source_count=p.db->query("SELECT count(*) AS n FROM web_sources")[0]["n"];
  memory.checkpoint("test_research",0);auto handoff=memory.handoff();CHECK(handoff.contains("research"));CHECK(handoff["research"][0]["source_ids"].get<std::string>().find(std::to_string(result["source_id"].get<Id>()))!=std::string::npos);CHECK(p.db->query("SELECT count(*) AS n FROM web_sources")[0]["n"]==source_count);
  tools.web().question("Unverifiable detail",true,p.session,p.task);tools.web().settings(false);Json warnings=Json::array();
  auto write=tools.execute("file_write",{{"path","uncertain.txt"},{"content","reversible draft"},{"description","fixture"}},[&](auto& type,auto& data){if(type=="research.warning")warnings.push_back(data);});CHECK(!write.contains("error"));CHECK(warnings.size()==1);CHECK(fs::exists(root/"project/uncertain.txt"));
  CHECK(tools.execute("task_update",{{"id",p.task},{"status","completed"}}).contains("error"));
  p.db->exec("UPDATE tasks SET risk='high' WHERE id=?",{p.task});CHECK(tools.execute("file_write",{{"path","dangerous.txt"},{"content","bad"},{"description","fixture"}}).contains("error"));CHECK(!fs::exists(root/"project/dangerous.txt"));
  CHECK(tools.execute("file_read",{{"path","uncertain.txt"}})["content"]=="reversible draft");
  tools.web().settings(true);tools.web().begin_turn(p.session,p.task);
  for(int i=0;i<8;++i)CHECK(!tools.execute("web_read",{{"url","https://docs.example.com/rules"}}).contains("error"));
  auto network_before=fetches;CHECK(tools.execute("web_read",{{"url","https://docs.example.com/rules"}}).contains("error"));CHECK(fetches==network_before);
  tools.web().begin_turn(p.session,p.task);
  auto snapshots_before=p.db->query("SELECT count(*) AS n FROM web_sources")[0]["n"];
  tools.service([&]{if(transferring)tools.web().settings(false);});
  auto interrupted=tools.execute("web_read",{{"url","https://docs.example.com/rules"}});
  CHECK(interrupted.contains("error") && interrupted["error"].get<std::string>().find("disabled")!=std::string::npos);
  CHECK(p.db->query("SELECT count(*) AS n FROM web_sources")[0]["n"]==snapshots_before);
  CHECK(fetches==network_before+1);network_before=fetches;tools.web().settings(true);
  tools.web().begin_turn(p.session,p.task);tools.service([]{throw TurnCancelled();});bool cancelled=false;try{tools.execute("web_read",{{"url","https://docs.example.com/rules"}});}catch(const TurnCancelled&){cancelled=true;}CHECK(cancelled && fetches==network_before);
}
class ResearchBackend final : public ModelBackend {
  int step_=0;
  Json doc_,search_;Id question_=0,task_=0,check_=0;
public:
  ModelInfo discover() override{return {"research-fixture",65536,true,true,false};}
  CapabilityReport probe() override{return {true,true,true,true};}
  void chat(const ChatRequest& request,StreamCallback callback) override {
    Json state=Json::object(),last=Json::object();
    for(auto& m:request.messages) {
      auto content=m.value("content",Json());if(!content.is_string())continue;auto text=content.get<std::string>();
      std::string marker="Runtime attention update (data):\n";
      if(m["role"]=="user" && text.starts_with(marker))state=Json::parse(text.substr(marker.size()));
      if(m["role"]=="tool")last=Json::parse(text);
    }
    if(!state.empty()) {task_=state["active_task"][0]["id"];check_=state["task_checks"][0]["id"];question_=state["research_questions"][0]["id"];}
    auto invoke=[&](std::string name,Json args){callback({{"choices",Json::array({{{"index",0},{"delta",{{"tool_calls",Json::array({{{"index",0},{"id","fixture-"+std::to_string(step_)},{"function",{{"name",name},{"arguments",args.dump()}}}}})}}},{"finish_reason","tool_calls"}}})}});};
    switch(step_++) {
      case 0:invoke("report_progress",{{"text","I will check the rules before implementing the program."}});break;
      case 1:invoke("web_search",{{"query","site:docs.example.com \"rotation\" rules"},{"include_domains",{"docs.example.com"}}});break;
      case 2:search_=last;CHECK(!search_.contains("error"));invoke("web_read",{{"source_id",search_["results"][0]["source_id"]}});break;
      case 3:doc_=last;CHECK(!doc_.contains("error"));invoke("research_resolve",{{"id",question_},{"status","supported"},{"conclusion","The documented fixture rule is clockwise rotation."},{"sources",Json::array({{{"source_id",doc_["source_id"]},{"quote","Rotation is clockwise."}}})}});break;
      case 4:CHECK(!last.contains("error"));invoke("file_write",{{"path","widget.cpp"},{"content","int main() { return 0; }\n"},{"description","Synthetic research implementation"}});break;
      case 5:CHECK(!last.contains("error"));invoke("shell_exec",{{"command","c++ widget.cpp -o widget && ./widget"},{"execution","host"}});break;
      case 6:CHECK(!last.contains("error") && last["exit_code"]==0);invoke("check_resolve",{{"check_id",check_},{"source_event_id",last["source_event_id"]},{"passed",true},{"explanation","Compiled and executed the fixture program successfully"}});break;
      case 7:CHECK(!last.contains("error"));invoke("task_update",{{"id",task_},{"status","completed"}});break;
      default:CHECK(!last.contains("error"));callback({{"choices",Json::array({{{"index",0},{"delta",{{"content","Implemented and executed the fixture. [Rules](https://docs.example.com/rules)"}}},{"finish_reason","stop"}}})}});break;
    }
  }
};
void runtime_research() {
  auto root=fs::temp_directory_path()/("saga-research-runtime-"+uuid());struct Cleanup{fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{root};
  Paths paths{root/"config",root/"data",root/"state",root/"run"};paths.create();private_dir(root/"project");Registry registry(paths);Config c;c.endpoint="http://example.invalid";c.model="fixture";c.context_length=65536;
  int approvals=0,network=0;Json events=Json::array();
  Runtime runtime(std::make_unique<PersonaContext>(paths,registry.create("Research runtime fixture",""),root/"project"),c,std::make_unique<ResearchBackend>(),[&](auto&,auto&){++approvals;return true;},[&](auto& url,auto&,auto& service){++network;if(service)service();auto r=page(url.find("duckduckgo")!=std::string::npos ? search_html : "<main>Rotation is clockwise.</main>");r.url=url;return r;});
  runtime.start();runtime.command("permissions",{{"mode","always_approve"}});runtime.chat("Create a widget; search online for its rules.",[&](auto& type,auto& data){events.push_back({{"type",type},{"payload",data}});});
  CHECK(network==2 && approvals==0);CHECK(fs::exists(root/"project/widget.cpp"));CHECK(fs::exists(root/"project/widget"));
  CHECK(runtime.persona().db->query("SELECT status FROM tasks ORDER BY id DESC LIMIT 1")[0]["status"]=="completed");CHECK(runtime.persona().db->query("SELECT status FROM research_questions")[0]["status"]=="supported");
  auto completed=std::find_if(events.begin(),events.end(),[](const Json& e){return e["type"]=="assistant.completed";});CHECK(completed!=events.end());CHECK((*completed)["payload"]["content"].get<std::string>().starts_with("Implemented"));CHECK(runtime.command("status")["web"]["searches"]==1);
  CHECK(live_command_allowed("web",{{"enabled",false}}));runtime.command("web",{{"enabled",false}});CHECK(runtime.command("status")["web"]["enabled"]==false);
}

class NoResearchBackend final : public ModelBackend {
public:
  ModelInfo discover() override{return {"fixture",65536,true,true,false};}
  CapabilityReport probe() override{return {true,true,true,true};}
  void chat(const ChatRequest&,StreamCallback callback) override{callback({{"choices",Json::array({{{"index",0},{"delta",{{"content","I researched this and it is verified."}}},{"finish_reason","stop"}}})}});}
};
void absent_research_gate() {
  auto root=fs::temp_directory_path()/("saga-research-missing-"+uuid());struct Cleanup{fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{root};
  Paths paths{root/"config",root/"data",root/"state",root/"run"};paths.create();private_dir(root/"project");Registry registry(paths);Config c;c.endpoint="http://example.invalid";c.model="fixture";c.context_length=65536;
  Runtime runtime(std::make_unique<PersonaContext>(paths,registry.create("Missing research fixture",""),root/"project"),c,std::make_unique<NoResearchBackend>(),[](auto&,auto&){return false;});runtime.start();Json events=Json::array();
  runtime.chat("Search online for the rotation specification.",[&](auto& type,auto& data){events.push_back({{"type",type},{"payload",data}});});
  CHECK(std::none_of(events.begin(),events.end(),[](const Json& e){return e["type"]=="assistant.delta";}));
  auto completed=std::find_if(events.begin(),events.end(),[](const Json& e){return e["type"]=="assistant.completed";});CHECK(completed!=events.end());CHECK((*completed)["payload"]["content"].get<std::string>().find("did not perform")!=std::string::npos);
}

}
int main(){try{parsing();boundaries();adapter();storage();proof_and_failure_gates();runtime_research();absent_research_gate();std::cout<<"Web adapter, extraction, boundaries and provenance checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
