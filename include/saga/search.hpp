#pragma once
#include <saga/web.hpp>
#include <chrono>
#include <mutex>

namespace saga {
using SearchTime = std::chrono::steady_clock::time_point;
using SearchDuration = std::chrono::milliseconds;
struct SearchClock {
  std::function<SearchTime()> time;
  std::function<void(SearchDuration)> pause;
  static SearchClock real();
};
enum class SearchErrorCode {
  RateLimited, BotChallenge, Timeout, ConnectionFailure, HttpFailure,
  InvalidResponse, BackendUnavailable, ApiDisabled, ParseError,
  PaginationUnavailable, SearchUnavailable
};
std::string search_error_name(SearchErrorCode);
struct SearchError : std::runtime_error {
  SearchErrorCode code;
  explicit SearchError(SearchErrorCode code, std::string message);
};
struct SearchContext {
  std::function<void()> service;
  Emit emit;
  bool human_initiated = false;
  std::string query_id;
  SearchTime deadline = SearchTime::max();
  SearchClock clock = SearchClock::real();
  void check() const;
  void event(const std::string &, Json = Json::object()) const;
};
// A shared gate admits requests at their actual start, not at reservation time.
// Waiting services cancellation/control messages without holding the gate lock.
class SearchRateLimiter {
  std::mutex mutex_, requests_;
  SearchTime next_{}, cooldown_{};
  SearchDuration interval_, backoff_;
public:
  explicit SearchRateLimiter(SearchDuration interval = SearchDuration(1000),
                             SearchDuration backoff = SearchDuration(60000));
  void configure(SearchDuration, SearchDuration);
  void wait(const SearchContext &);
  void degrade(const SearchContext &);
  WebResponse perform(const SearchContext &, const std::function<WebResponse()> &);
};
struct FourGetInstance {
  std::string origin;
  bool checked = false, api_enabled = false, manual = false;
  int bot_protection = -1, version = 0;
  double latency_ms = 0;
  unsigned failures = 0, successes = 0;
  Id last_probe = 0, last_success = 0, cooldown_until = 0;
  SearchTime retry_after{};
  bool eligible(const SearchContext &) const;
  double score(bool preferred) const;
  Json json() const;
};
class FourGetSearchEngine final : public SearchEngine {
  struct State;
  std::unique_ptr<State> state_;
public:
  explicit FourGetSearchEngine(Config = {}, WebTransport = fetch_public_web, Database * = nullptr);
  ~FourGetSearchEngine();
  Json capabilities() const override;
  std::string guidance() const override;
  Json run(const SearchRequest &, const SearchContext &) override;
  static std::vector<std::string> directory(const WebResponse &);
  static FourGetInstance probe(const WebResponse &, const std::string &origin);
  static Json parse(const WebResponse &, const SearchRequest &, const std::string &origin);
  Json health() const;
};
class SearchEngineRegistry {
  std::map<std::string, std::unique_ptr<SearchEngine>> engines_;
public:
  void add(std::unique_ptr<SearchEngine>);
  SearchEngine &get(const std::string &) const;
};
class SearchOrchestrator {
  Database &db_;
  Config config_;
  SearchEngineRegistry registry_;
  std::vector<std::string> order_;
  std::mutex mutex_; // Serializes this connection's cache/state; other engines/runtimes remain free.
  std::map<std::string, SearchTime> cooldowns_;
public:
  SearchOrchestrator(Database &, Config, WebTransport = fetch_public_web);
  SearchEngineRegistry &registry() { return registry_; }
  Json capabilities() const;
  std::string guidance() const;
  Json search(const SearchRequest &, SearchContext);
};
namespace search_detail {
std::string origin(std::string_view, bool allow_private = false);
void validate(const SearchRequest &);
bool allowed(const std::string &, const SearchRequest &);
std::string query(const SearchRequest &);
WebResponse fetch(const WebTransport &, const std::string &, const std::string &,
                  const SearchContext &, int timeout, bool allow_private = false);
Json payload(const WebResponse &);
std::unique_lock<std::mutex> lock(std::mutex &, const SearchContext &);
}
} // namespace saga
