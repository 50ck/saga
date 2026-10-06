#include <saga/search.hpp>
#include "../web/internal.hpp"
#include <algorithm>
#include <set>
#include <cmath>
#include <limits>

namespace saga {
bool FourGetInstance::eligible(const SearchContext &context) const {
  return context.clock.time()>=retry_after && (!checked || (api_enabled && bot_protection==0 && version>0));
}
double FourGetInstance::score(bool preferred) const {
  // Availability is a hard filter. Then favor observed reliability over latency.
  return (checked ? 100.0 : 0.0)+30.0*successes/(successes+failures+1.0)-20.0*failures+
    20.0/(1.0+latency_ms/200.0)+(preferred ? 12.0 : 0.0);
}
Json FourGetInstance::json() const {
  return {{"origin",origin},{"checked",checked},{"api_enabled",api_enabled},{"manual",manual},
          {"bot_protection",bot_protection},{"version",version},{"latency_ms",latency_ms},
          {"failures",failures},{"successes",successes},{"last_probe",last_probe},
          {"last_success",last_success},{"cooldown_until",cooldown_until}};
}
struct FourGetSearchEngine::State {
  Config config;
  WebTransport transport;
  Database *db;
  mutable std::mutex mutex;
  std::map<std::string,FourGetInstance> instances;
  Id directory_updated = 0;
  SearchTime directory_retry{}, directory_refresh{}, directory_stale{};
  bool clock_bound = false;
  std::string preferred;
  State(Config c,WebTransport t,Database *d) : config(std::move(c)),transport(std::move(t)),db(d) {
    for(auto &origin:config.fourget_manual_instances)add(origin,true);
    if(db) {
      auto rows=db->query("SELECT origins_json,updated_at FROM search_directory_cache WHERE directory='4get.ca'");
      if(!rows.empty() && now()-rows[0]["updated_at"].get<Id>()<Id(config.fourget_directory_ttl_seconds)*4000) {
        directory_updated=rows[0]["updated_at"];
        for(auto &origin:Json::parse(rows[0]["origins_json"].get<std::string>()))add(origin.get<std::string>(),false);
      }
      for(auto &row:db->query("SELECT state_json FROM search_instance_cache")) {
        try {
          auto value=Json::parse(row["state_json"].get<std::string>());
          auto found=instances.find(value.at("origin").get<std::string>());
          if(found==instances.end())continue;
          auto &instance=found->second;
          instance.checked=value.at("checked");instance.api_enabled=value.at("api_enabled");
          instance.bot_protection=value.at("bot_protection");instance.version=value.at("version");
          instance.latency_ms=value.at("latency_ms");instance.failures=value.at("failures");instance.successes=value.at("successes");
          instance.last_probe=value.at("last_probe");instance.last_success=value.at("last_success");instance.cooldown_until=value.at("cooldown_until");

        }catch(const std::exception &){} // Obsolete cache records never authorize a host.
      }
    }
  }
  void add(const std::string &url,bool manual) {
    try {
      auto origin=search_detail::origin(url,manual && config.fourget_allow_private_instances);
      if(instances.size()>=128 && !instances.contains(origin))return;
      auto &instance=instances[origin];instance.origin=origin;instance.manual=instance.manual || manual;
    }catch(const std::exception &) {if(manual)throw;}
  }
  void save(FourGetInstance &instance) {
    if(db)db->exec("INSERT INTO search_instance_cache(origin,state_json,updated_at) VALUES(?,?,?) ON CONFLICT(origin) DO UPDATE SET state_json=excluded.state_json,updated_at=excluded.updated_at",{instance.origin,instance.json().dump(),now()});
  }
  void bind_clock(const SearchContext &context) {
    if(clock_bound)return;
    auto time=context.clock.time();auto wall=now();
    auto remaining=[&](Id expiry,Id maximum){return SearchDuration(std::clamp<Id>(expiry-wall,0,maximum));};
    auto directory_ttl=Id(config.fourget_directory_ttl_seconds)*1000;
    directory_refresh=time+remaining(directory_updated+directory_ttl,directory_ttl);
    directory_stale=time+remaining(directory_updated+4*directory_ttl,4*directory_ttl);
    for(auto &[origin,instance]:instances) {
      instance.retry_after=time+remaining(instance.cooldown_until,3600000);
      instance.probe_after=time+remaining(instance.last_probe+Id(config.fourget_probe_ttl_seconds)*1000,Id(config.fourget_probe_ttl_seconds)*1000);
    }
    clock_bound=true;
  }
  void refresh(const SearchContext &context) {
    if(directory_updated && context.clock.time()<directory_refresh)return;
    if(directory_updated && context.clock.time()>=directory_stale) {
      std::erase_if(instances,[](auto &entry){return !entry.second.manual;});directory_updated=0;
    }
    if(context.clock.time()<directory_retry)return;
    try {
      auto response=search_detail::fetch(transport,"https://4get.ca/instances","",context,config.fourget_request_timeout_seconds);
      auto seeds=FourGetSearchEngine::directory(response);
      if(seeds.empty())throw SearchError(SearchErrorCode::InvalidResponse,"4get directory contains no eligible origins");
      for(auto &origin:seeds)add(origin,false);
      directory_updated=now();directory_refresh=context.clock.time()+std::chrono::seconds(config.fourget_directory_ttl_seconds);directory_stale=context.clock.time()+std::chrono::seconds(Id(config.fourget_directory_ttl_seconds)*4);
      if(db)db->exec("INSERT INTO search_directory_cache(directory,origins_json,updated_at) VALUES('4get.ca',?,?) ON CONFLICT(directory) DO UPDATE SET origins_json=excluded.origins_json,updated_at=excluded.updated_at",{Json(seeds).dump(),directory_updated});
    }catch(const TurnCancelled &){throw;}
     catch(const SearchError &error){
      directory_retry=context.clock.time()+std::chrono::minutes(1);
      context.event("search.directory_failed",{{"engine","fourget"},{"reason",search_error_name(error.code)}});
      if(instances.empty())throw;
    }
  }
  void failure(FourGetInstance &instance,const SearchContext &context,SearchErrorCode code) {
    ++instance.failures;
    auto factor=std::min(8u,1u<<std::min(instance.failures-1,3u));
    auto wait=SearchDuration(config.fourget_failure_backoff_ms)*factor;
    instance.retry_after=context.clock.time()+wait;
    instance.cooldown_until=now()+wait.count();
    if(code!=SearchErrorCode::ApiDisabled && code!=SearchErrorCode::BotChallenge)instance.checked=false;
    if(code==SearchErrorCode::ApiDisabled)instance.api_enabled=false;
    if(code==SearchErrorCode::BotChallenge)instance.bot_protection=1;
    if(preferred==instance.origin)preferred.clear();
    save(instance);
  }
};
FourGetSearchEngine::FourGetSearchEngine(Config config,WebTransport transport,Database *db)
    : state_(std::make_unique<State>(std::move(config),std::move(transport),db)) {}
FourGetSearchEngine::~FourGetSearchEngine() = default;
Json FourGetSearchEngine::capabilities() const {
  return {{"id","fourget"},{"web",true},{"structured_api",true},{"pagination",true},{"domain_constraints",true},{"advanced_query",true}};
}
std::string FourGetSearchEngine::guidance() const {
  return "4get provides web results through a public API. Use short, precise technical queries and exact errors. Native scraper operators may vary; explicit domain constraints are enforced locally. Search snippets are discovery only: read primary sources before verifying claims. Runtime selects compatible instances and respects their bot protection. No background harvesting.";
}
std::vector<std::string> FourGetSearchEngine::directory(const WebResponse &response) {
  if(response.status!=200)throw SearchError(SearchErrorCode::HttpFailure,"4get instance directory is unavailable");
  web::HtmlDocument document(response.body,web::WebLimits{});
  std::set<std::string> seen;std::vector<std::string> result;
  web::walk(document.root(),[&](web::Node *table,size_t){
    if(web::tag(table)!="table" || result.size()>=128)return;
    bool server_table=false;
    web::walk(table,[&](web::Node *cell,size_t){
      if(web::tag(cell)!="th")return;
      std::string text;web::walk(cell,[&](web::Node *part,size_t){text+=web::text(part);},{});
      if(lower(trim(text))=="server")server_table=true;
    },{});
    if(!server_table)return;
    web::walk(table,[&](web::Node *node,size_t){
      if(web::tag(node)!="a" || result.size()>=128)return;
      auto *parent=node->parent;bool cell=false;
      while(parent && parent!=table) {if(web::tag(parent)=="td")cell=true;parent=parent->parent;}
      if(!cell)return;
      try{auto origin=search_detail::origin(web::attr(node,"href"));if(seen.insert(origin).second)result.push_back(origin);}catch(const std::exception &){}
    },{});
  },{});
  return result;
}
FourGetInstance FourGetSearchEngine::probe(const WebResponse &response,const std::string &origin) {
  auto value=search_detail::payload(response);
  if(value.value("service",Json())!="4get" || !value.contains("server") || !value["server"].is_object())
    throw SearchError(SearchErrorCode::InvalidResponse,"Probe does not identify a 4get service");
  auto &server=value["server"];
  if(!server.contains("api_enabled") || !server["api_enabled"].is_boolean() || !server.contains("bot_protection") || !server["bot_protection"].is_number_integer() || !server.contains("version") || !server["version"].is_number_integer())
    throw SearchError(SearchErrorCode::InvalidResponse,"4get probe lacks required capabilities");
  FourGetInstance instance;instance.origin=origin;instance.checked=true;
  auto protection=server["bot_protection"].get<Id>(), version=server["version"].get<Id>();
  if(protection<0 || protection>std::numeric_limits<int>::max())throw SearchError(SearchErrorCode::InvalidResponse,"4get probe has an invalid protection mode");
  instance.api_enabled=server["api_enabled"];instance.bot_protection=static_cast<int>(protection);
  if(version<1 || version>1000000)throw SearchError(SearchErrorCode::InvalidResponse,"4get probe has an unsupported version");
  instance.version=static_cast<int>(version);instance.last_probe=now();return instance;
}
Json FourGetSearchEngine::parse(const WebResponse &response,const SearchRequest &request,const std::string &origin) {
  auto value=search_detail::payload(response);
  if(!value.contains("web") || !value["web"].is_array())throw SearchError(SearchErrorCode::InvalidResponse,"4get search response lacks web results");
  Json hits=Json::array();std::set<std::string> seen;size_t inspected=0;bool valid_target=false;
  for(auto &hit:value["web"]) {
    if(++inspected>1000)break;
    if(!hit.is_object() || !hit.contains("url") || !hit["url"].is_string())continue;
    try {
      auto url=web_url(hit["url"].get<std::string>());valid_target=true;
      if(!search_detail::allowed(url,request) || !seen.insert(url).second)continue;
      auto field=[&](const char *name,size_t bytes){return hit.contains(name) && hit[name].is_string() ? utf8_excerpt(web::sanitize_text(hit[name].get<std::string>()),bytes) : std::string();};
      hits.push_back({{"title",field("title",512)},{"url",url},{"snippet",field("description",2048)},{"rank",hits.size()+1}});
      if(hits.size()>=static_cast<size_t>(request.limit))break;
    }catch(const std::exception &){} // Malformed target links cannot become fetch destinations.
  }
  if(!value["web"].empty() && !valid_target)throw SearchError(SearchErrorCode::InvalidResponse,"4get results contain no valid target URLs");
  Json result={{"engine","fourget"},{"query",request.query},{"submitted_query",search_detail::query(request)},{"results",hits},{"empty",hits.empty()}};
  if(value.contains("npt") && !value["npt"].is_null()) {
    if(!value["npt"].is_string() || value["npt"].get_ref<const std::string &>().size()>4096)
      throw SearchError(SearchErrorCode::InvalidResponse,"4get pagination token is invalid");
    if(!value["npt"].get_ref<const std::string &>().empty())result["next_cursor"]=Json{{"engine","fourget"},{"instance",origin},{"npt",value["npt"]},{"query",request.query},{"include_domains",request.include_domains},{"exclude_domains",request.exclude_domains},{"expires_at",now()+900000}}.dump();
  }
  return result;
}
Json FourGetSearchEngine::health() const {
  std::lock_guard guard(state_->mutex);Json result=Json::array();for(auto &[origin,instance]:state_->instances)result.push_back(instance.json());return result;
}
Json FourGetSearchEngine::run(const SearchRequest &request,const SearchContext &context) {
  search_detail::validate(request);context.check();auto guard=search_detail::lock(state_->mutex,context);state_->bind_clock(context);
  std::string affinity,token;
  if(!request.cursor.empty()) {
    Json cursor;
    try{cursor=search_detail::cursor(request.cursor);}catch(const Json::exception &){throw SearchError(SearchErrorCode::PaginationUnavailable,"Invalid 4get cursor; restart the search");}
    if(cursor.value("engine","")!="fourget" || cursor.value("query","")!=request.query || cursor.value("include_domains",Json())!=Json(request.include_domains) || cursor.value("exclude_domains",Json())!=Json(request.exclude_domains) || cursor.value("expires_at",Id(0))<now() || !cursor.contains("npt") || !cursor["npt"].is_string())
      throw SearchError(SearchErrorCode::PaginationUnavailable,"4get cursor expired or belongs to another query; restart the search");
    affinity=cursor.value("instance","");token=cursor["npt"];if(token.empty() || token.size()>4096)throw SearchError(SearchErrorCode::PaginationUnavailable,"Invalid 4get cursor; restart the search");
    if(!state_->instances.contains(affinity))throw SearchError(SearchErrorCode::PaginationUnavailable,"Pagination instance is no longer known; restart the search");
  } else state_->refresh(context);
  std::vector<FourGetInstance *> candidates;
  for(auto &[origin,instance]:state_->instances) {
    if(!affinity.empty() && origin!=affinity)continue;
    // Ineligible capabilities are reconsidered only after their probe TTL and cooldown.
    if(instance.checked && context.clock.time()>=instance.probe_after && context.clock.time()>=instance.retry_after)instance.checked=false;
    if(instance.eligible(context))candidates.push_back(&instance);
  }
  std::stable_sort(candidates.begin(),candidates.end(),[&](auto *a,auto *b){
    auto score=[&](auto *instance){return instance->score(state_->preferred==instance->origin)+(instance->manual && state_->config.fourget_prefer_manual ? 150.0 : 0.0);};return score(a)>score(b);
  });
  size_t attempts=0;std::set<std::string> attempted;
  for(auto *instance:candidates) {
    if(!attempted.insert(instance->origin).second || ++attempts>static_cast<size_t>(state_->config.fourget_max_instance_attempts))break;
    context.check();bool private_allowed=instance->manual && state_->config.fourget_allow_private_instances;
    auto start=context.clock.time();
    try {
      if(!instance->checked) {
        auto response=search_detail::fetch(state_->transport,instance->origin+"/ami4get","",context,state_->config.fourget_probe_timeout_seconds,private_allowed);
        auto checked=probe(response,instance->origin);checked.manual=instance->manual;checked.failures=instance->failures;checked.successes=instance->successes;
        checked.probe_after=context.clock.time()+std::chrono::seconds(state_->config.fourget_probe_ttl_seconds);
        checked.latency_ms=std::chrono::duration_cast<SearchDuration>(context.clock.time()-start).count();*instance=checked;state_->save(*instance);
        if(!instance->api_enabled)throw SearchError(SearchErrorCode::ApiDisabled,"4get instance has disabled its API");
        if(instance->bot_protection!=0)throw SearchError(SearchErrorCode::BotChallenge,"4get instance has bot protection; Saga will not bypass it");
      }
      context.event("search.backend_selected",{{"engine","fourget"},{"instance",instance->origin}});
      auto url=instance->origin+"/api/v1/web?"+(token.empty() ? "s="+web_encode(search_detail::query(request)) : "npt="+web_encode(token));
      auto response=search_detail::fetch(state_->transport,url,"",context,state_->config.fourget_request_timeout_seconds,private_allowed);
      // Redirects to another server cannot transfer the operator's pagination pass/state.
      if(search_detail::origin(web::url_origin(response.url),private_allowed)!=instance->origin)
        throw SearchError(SearchErrorCode::InvalidResponse,"4get API redirected to a different origin");
      auto result=parse(response,request,instance->origin);
      auto latency=static_cast<double>(std::chrono::duration_cast<SearchDuration>(context.clock.time()-start).count());
      instance->latency_ms=instance->successes ? instance->latency_ms*0.75+latency*0.25 : latency;
      ++instance->successes;instance->failures=0;instance->last_success=now();instance->retry_after={};instance->cooldown_until=0;
      state_->preferred=instance->origin;state_->save(*instance);
      context.event("search.instance_completed",{{"engine","fourget"},{"instance",instance->origin},{"results",result["results"].size()},{"latency_ms",latency}});
      return result;
    }catch(const TurnCancelled &){throw;}
     catch(const SearchError &error){
      state_->failure(*instance,context,error.code);
      context.event("search.instance_failed",{{"engine","fourget"},{"instance",instance->origin},{"reason",search_error_name(error.code)}});
      if(!affinity.empty())throw SearchError(SearchErrorCode::PaginationUnavailable,"4get pagination instance failed; restart the search instead of transferring its token");
    }
  }
  if(!affinity.empty())throw SearchError(SearchErrorCode::PaginationUnavailable,"4get pagination instance is cooling down; restart the search");
  throw SearchError(SearchErrorCode::BackendUnavailable,"4get has no healthy API-enabled instance without bot protection");
}
} // namespace saga
