#include <saga/search.hpp>
#include <curl/curl.h>
#if __has_include(<curl/urlapi.h>)
#include <curl/urlapi.h>
#endif
#include <algorithm>
#include <set>
#include <thread>

namespace saga {
SearchClock SearchClock::real() {
  return {[]{ return std::chrono::steady_clock::now(); },
          [](SearchDuration duration){ std::this_thread::sleep_for(duration); }};
}
std::string search_error_name(SearchErrorCode code) {
  switch(code) {
#define NAME(value) case SearchErrorCode::value: return #value
    NAME(RateLimited); NAME(BotChallenge); NAME(Timeout); NAME(ConnectionFailure);
    NAME(HttpFailure); NAME(InvalidResponse); NAME(BackendUnavailable); NAME(ApiDisabled);
    NAME(ParseError); NAME(PaginationUnavailable); NAME(SearchUnavailable);
#undef NAME
  }
  return "SearchUnavailable";
}
SearchError::SearchError(SearchErrorCode value, std::string message)
    : std::runtime_error(std::move(message)), code(value) {}
void SearchContext::check() const {
  if(service) service();
  if(!human_initiated) throw SearchError(SearchErrorCode::BackendUnavailable, "Search requires a human-initiated operation");
  if(clock.time()>=deadline) throw SearchError(SearchErrorCode::Timeout, "Search time budget exhausted");
}
void SearchContext::event(const std::string &type, Json data) const {
  data["query_id"]=query_id;
  if(emit) emit(type,data);
}
SearchRateLimiter::SearchRateLimiter(SearchDuration interval, SearchDuration backoff)
    : interval_(std::max(interval, SearchDuration(1000))), backoff_(backoff) {}
void SearchRateLimiter::configure(SearchDuration interval, SearchDuration backoff) {
  std::lock_guard guard(mutex_);interval_=std::max(interval_,interval);backoff_=std::max(backoff_,backoff);
}
void SearchRateLimiter::wait(const SearchContext &context) {
  bool queued=false;
  auto start=context.clock.time();
  for(;;) {
    context.check();
    SearchDuration delay;
    {
      std::lock_guard guard(mutex_);
      auto time=context.clock.time();
      if(time<cooldown_) throw SearchError(SearchErrorCode::RateLimited,"DuckDuckGo is cooling down after a challenge or rate limit");
      if(time>=next_) {next_=time+interval_;break;}
      delay=std::chrono::duration_cast<SearchDuration>(next_-time);
    }
    if(!queued) {context.event("search.queued",{{"engine","duckduckgo"}});queued=true;}
    context.clock.pause(std::clamp(delay,SearchDuration(1),SearchDuration(25)));
  }
  if(queued) context.event("search.rate_limit_wait",{{"engine","duckduckgo"},{"wait_ms",std::chrono::duration_cast<SearchDuration>(context.clock.time()-start).count()}});
}
void SearchRateLimiter::degrade(const SearchContext &context) {
  std::lock_guard guard(mutex_);cooldown_=context.clock.time()+backoff_;
}
WebResponse SearchRateLimiter::perform(const SearchContext &context,const std::function<WebResponse()> &request) {
  auto guard=search_detail::lock(requests_,context);wait(context);return request();
}
Json SearchEngine::search(const SearchRequest &request, const std::function<void()> &service) {
  // Explicit calls of the public synchronous convenience API are operator actions.
  SearchContext context;context.human_initiated=true;context.service=service;
  return run(request,context);
}
namespace search_detail {
std::string origin(std::string_view input, bool allow_private) {
  auto normalized=web_url(input,{},allow_private);
  auto *raw=curl_url();if(!raw) throw std::runtime_error("Cannot initialize URL parser");
  std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> url(raw,curl_url_cleanup);
  if(curl_url_set(raw,CURLUPART_URL,normalized.c_str(),0))throw std::runtime_error("Invalid instance URL");
  auto part=[&](decltype(CURLUPART_URL) key){char *value=nullptr;if(curl_url_get(raw,key,&value,0))return std::string();std::unique_ptr<char,decltype(&curl_free)> owned(value,curl_free);return std::string(value);};
  if(part(CURLUPART_PATH)!="/" || !part(CURLUPART_QUERY).empty() || input.find('#')!=std::string_view::npos)
    throw std::runtime_error("4get instances must be origins without a path, query or fragment");
  return normalized.substr(0,normalized.size()-1);
}
void validate(const SearchRequest &request) {
  if(trim(request.query).empty() || request.query.size()>2048 || request.query.find_first_of("\r\n")!=std::string::npos || request.limit<1 || request.limit>10)
    throw std::runtime_error("Search requires a one-line query of at most 2048 bytes and 1–10 results");
  for(auto *domains:{&request.include_domains,&request.exclude_domains}) {
    if(domains->size()>16) throw std::runtime_error("At most sixteen domain constraints are supported");
    for(auto &domain:*domains) if(domain.empty() || domain.size()>253 || domain!=lower(domain) || domain.front()=='.' || domain.back()=='.' || domain.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-")!=std::string::npos)
      throw std::runtime_error("Domain constraints must be lowercase domain names");
  }
}
bool allowed(const std::string &url,const SearchRequest &request) {
  auto host=web_host(url);
  auto matches=[&](const std::string &domain){return host==domain || (host.size()>domain.size() && host.ends_with("."+domain));};
  return !std::any_of(request.exclude_domains.begin(),request.exclude_domains.end(),matches) &&
    (request.include_domains.empty() || std::any_of(request.include_domains.begin(),request.include_domains.end(),matches));
}
std::string query(const SearchRequest &request) {
  auto value=request.query;
  if(request.include_domains.size()==1 && value.find("site:"+request.include_domains[0])==std::string::npos)value+=" site:"+request.include_domains[0];
  for(auto &domain:request.exclude_domains) if(value.find("-site:"+domain)==std::string::npos)value+=" -site:"+domain;
  return value;
}
std::unique_lock<std::mutex> lock(std::mutex &mutex,const SearchContext &context) {
  std::unique_lock result(mutex,std::defer_lock);
  while(!result.try_lock()) {context.check();context.clock.pause(SearchDuration(10));}
  context.check();return result;
}
WebResponse fetch(const WebTransport &transport, const std::string &url,const std::string &form,
                  const SearchContext &context,int timeout,bool allow_private) {
  context.check();auto bounded=context;bounded.deadline=std::min(context.deadline,context.clock.time()+std::chrono::seconds(timeout));
  auto control=[&]{bounded.check();};
  try {
    using Native=WebResponse(*)(const std::string &,const std::string &,const std::function<void()> &);
    if(auto target=transport.target<Native>();target && *target==fetch_public_web) {
      WebHttpOptions options;options.connect_timeout=std::min(timeout,3);options.total_timeout=timeout;
      options.allow_private_network=allow_private;options.max_response_bytes=2*1024*1024;
      return fetch_web_http(url,form,control,options);
    }
    auto response=transport(url,form,control);control();
    if(response.body.size()>2*1024*1024)throw SearchError(SearchErrorCode::InvalidResponse,"Search response exceeds size limit");
    // Custom transports must observe the same redirect-origin policy.
    if(!response.url.empty())(void)web_url(response.url,{},allow_private);
    return response;
  }catch(const TurnCancelled &){throw;}
   catch(const SearchError &){throw;}
   catch(const std::exception &error){
    auto message=lower(error.what());
    auto code=message.find("timeout")!=std::string::npos || message.find("timed out")!=std::string::npos ? SearchErrorCode::Timeout : SearchErrorCode::ConnectionFailure;
    throw SearchError(code,"Search transport failed ("+search_error_name(code)+")");
  }
}
Json payload(const WebResponse &response) {
  if(response.status==429)throw SearchError(SearchErrorCode::RateLimited,"4get rate limited the request");
  if(response.status==401 || response.status==403)throw SearchError(SearchErrorCode::BotChallenge,"4get rejected the request; protected instances are not bypassed");
  if(response.status!=200)throw SearchError(SearchErrorCode::HttpFailure,"4get HTTP status "+std::to_string(response.status));
  Json value;
  try{value=Json::parse(response.body);}catch(const Json::exception &){throw SearchError(SearchErrorCode::ParseError,"4get returned malformed JSON");}
  if(!value.is_object() || !value.contains("status") || !value["status"].is_string())throw SearchError(SearchErrorCode::InvalidResponse,"4get response lacks its status field");
  if(value["status"]!="ok") {
    auto status=lower(value["status"].get<std::string>());
    auto code=status.find("disabled")!=std::string::npos ? SearchErrorCode::ApiDisabled :
      status.find("captcha")!=std::string::npos || status.find("bot")!=std::string::npos || status.find("pass")!=std::string::npos ? SearchErrorCode::BotChallenge : SearchErrorCode::BackendUnavailable;
    throw SearchError(code,"4get reported an unsuccessful API status ("+search_error_name(code)+")");
  }
  return value;
}
}
void SearchEngineRegistry::add(std::unique_ptr<SearchEngine> engine) {
  auto id=engine->capabilities().at("id").get<std::string>();
  engines_.insert_or_assign(id,std::move(engine));
}
SearchEngine &SearchEngineRegistry::get(const std::string &id) const {
  auto found=engines_.find(id);if(found==engines_.end())throw std::runtime_error("Unsupported search engine: "+id);
  return *found->second;
}
namespace {
std::shared_ptr<SearchRateLimiter> shared_ddg(const Config &config) {
  // All production runtimes share the gate. Retain the strongest configured policy.
  static std::mutex mutex;
  static std::shared_ptr<SearchRateLimiter> limiter;
  std::lock_guard guard(mutex);
  if(!limiter)limiter=std::make_shared<SearchRateLimiter>(SearchDuration(config.duckduckgo_min_request_interval_ms),SearchDuration(config.duckduckgo_challenge_backoff_ms));
  limiter->configure(SearchDuration(config.duckduckgo_min_request_interval_ms),SearchDuration(config.duckduckgo_challenge_backoff_ms));
  return limiter;
}
}
std::unique_ptr<SearchEngine> make_search_engine(const std::string &name,WebTransport transport) {
  if(name=="duckduckgo")return std::make_unique<DuckDuckGoEngine>(std::move(transport),shared_ddg(Config{}));
  if(name=="fourget")return std::make_unique<FourGetSearchEngine>(Config{},std::move(transport));
  throw std::runtime_error("Unsupported search engine: "+name);
}
SearchOrchestrator::SearchOrchestrator(Database &db,Config config,WebTransport transport)
    : db_(db),config_(std::move(config)),order_(config_.search_engines) {
  if(order_.empty()) {order_.push_back(config_.search_engine);order_.push_back(config_.search_engine=="duckduckgo" ? "fourget" : "duckduckgo");}
  using Native=WebResponse(*)(const std::string &,const std::string &,const std::function<void()> &);
  auto target=transport.target<Native>();
  auto limiter=target && *target==fetch_public_web ? shared_ddg(config_) : std::make_shared<SearchRateLimiter>(SearchDuration(config_.duckduckgo_min_request_interval_ms),SearchDuration(config_.duckduckgo_challenge_backoff_ms));
  registry_.add(std::make_unique<DuckDuckGoEngine>(transport,std::move(limiter)));
  registry_.add(std::make_unique<FourGetSearchEngine>(config_,std::move(transport),&db_));
  for(auto &id:order_)(void)registry_.get(id);
}
Json SearchOrchestrator::capabilities() const {
  Json result=Json::array();for(auto &id:order_)result.push_back(registry_.get(id).capabilities());return result;
}
std::string SearchOrchestrator::guidance() const {
  return "Search backend selection, caching and bounded failover are automatic. Never choose a public instance or bypass bot protection. "+registry_.get(order_.front()).guidance();
}
Json SearchOrchestrator::search(const SearchRequest &request,SearchContext context) {
  search_detail::validate(request);context.check();
  context.query_id=uuid();context.deadline=std::min(context.deadline,context.clock.time()+std::chrono::seconds(config_.search_total_timeout_seconds));
  auto guard=search_detail::lock(mutex_,context);
  auto candidates=order_;
  if(!request.cursor.empty()) {
    auto cursor=Json::parse(request.cursor);
    candidates={cursor.value("engine",std::string("duckduckgo"))};
  }
  context.event("search.requested");Json failures=Json::array();std::set<std::string> attempted;
  for(auto &id:candidates) {
    if(!attempted.insert(id).second || attempted.size()>static_cast<size_t>(config_.search_max_engine_attempts))continue;
    context.check();auto &engine=registry_.get(id);
    auto cache_key=digest(Json{{"engine",id},{"query",trim(request.query)},{"limit",request.limit},{"include",request.include_domains},{"exclude",request.exclude_domains}}.dump());
    if(request.cursor.empty()) {
      auto rows=db_.query("SELECT response_json FROM search_result_cache WHERE cache_key=? AND expires_at>?",{cache_key,now()});
      if(!rows.empty()) {auto result=Json::parse(rows[0]["response_json"].get<std::string>());context.event("search.completed",{{"engine",id},{"cache_hit",true},{"results",result["results"].size()}});return result;}
    }
    try {
      if(context.clock.time()<cooldowns_[id])throw SearchError(SearchErrorCode::BackendUnavailable,"Search backend is temporarily cooling down");
      context.event("search.started",{{"engine",id}});auto start=context.clock.time();
      auto result=engine.run(request,context);
      if(!result.is_object() || !result.contains("results") || !result["results"].is_array())throw SearchError(SearchErrorCode::InvalidResponse,"Search backend returned invalid normalized results");
      auto cached=result;cached.erase("next_cursor"); // 4get continuation tokens are single-use.
      if(request.cursor.empty())db_.exec("INSERT INTO search_result_cache(cache_key,response_json,expires_at) VALUES(?,?,?) ON CONFLICT(cache_key) DO UPDATE SET response_json=excluded.response_json,expires_at=excluded.expires_at",{cache_key,cached.dump(),now()+Id(config_.search_cache_ttl_seconds)*1000});
      db_.sql("DELETE FROM search_result_cache WHERE cache_key IN (SELECT cache_key FROM search_result_cache ORDER BY expires_at DESC LIMIT -1 OFFSET 256)");
      context.event("search.completed",{{"engine",id},{"cache_hit",false},{"results",result["results"].size()},{"latency_ms",std::chrono::duration_cast<SearchDuration>(context.clock.time()-start).count()}});
      return result;
    }catch(const TurnCancelled &){throw;}
     catch(const SearchError &error){
      if(error.code==SearchErrorCode::PaginationUnavailable || !request.cursor.empty())throw;
      failures.push_back({{"engine",id},{"reason",search_error_name(error.code)}});
      cooldowns_[id]=context.clock.time()+SearchDuration(id=="duckduckgo" ? config_.duckduckgo_challenge_backoff_ms : 1000);
      context.event("search.fallback",{{"engine",id},{"reason",search_error_name(error.code)}});
    }
  }
  context.event("search.failed",{{"attempts",failures}});
  throw SearchError(SearchErrorCode::SearchUnavailable,"Search unavailable: "+failures.dump()+". Existing source evidence remains valid; direct URLs can still be read.");
}
} // namespace saga
