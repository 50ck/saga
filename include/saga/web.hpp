#pragma once
#include <map>
#include <saga/common.hpp>
#include <saga/db.hpp>
namespace saga {
namespace web {
class WebAcquisitionEngine;
}
struct WebResponse {
  std::string url, body, content_type, location;
  long status = 0;
  std::map<std::string, std::string> headers;
  std::string charset, requested_url;
  bool cache_hit = false;
};
struct WebHttpOptions {
  size_t max_response_bytes = 2 * 1024 * 1024;
  int max_redirects = 5, connect_timeout = 10, total_timeout = 30;
  bool allow_private_network = false;
  std::map<std::string, std::string> headers;
};
WebResponse fetch_web_http(const std::string &, const std::string &, const std::function<void()> &,
                           const WebHttpOptions &);
using WebTransport = std::function<WebResponse(const std::string &, const std::string &,
                                               const std::function<void()> &)>;
// Public destinations only. Socket addresses are checked at connection time,
// including after DNS resolution; no environment proxy or credentials are used.
WebResponse fetch_public_web(const std::string &url, const std::string &form,
                             const std::function<void()> &service);
bool public_web_address(std::string_view address);
std::string web_url(std::string_view value, std::string_view base = {}, bool allow_private = false);
std::string web_host(std::string_view value);
std::string web_encode(std::string_view value);
struct WebDocument {
  std::string title, text;
  bool truncated = false;
};
WebDocument extract_web_document(const WebResponse &response);
class SearchOrchestrator;
class SearchRateLimiter;
struct SearchContext;
struct SearchRequest {
  std::string query, cursor;
  int limit = 5;
  std::vector<std::string> include_domains, exclude_domains;
};
class SearchEngine {
public:
  virtual ~SearchEngine() = default;
  virtual Json capabilities() const = 0;
  virtual std::string guidance() const = 0;
  virtual Json run(const SearchRequest &, const SearchContext &) = 0;
  Json search(const SearchRequest &, const std::function<void()> &service);
};
class DuckDuckGoEngine final : public SearchEngine {
  WebTransport transport_;
  std::shared_ptr<SearchRateLimiter> limiter_;

public:
  explicit DuckDuckGoEngine(WebTransport transport = fetch_public_web,
                           std::shared_ptr<SearchRateLimiter> limiter = {});
  Json capabilities() const override;
  std::string guidance() const override;
  Json run(const SearchRequest &, const SearchContext &) override;
  static Json parse(const WebResponse &, const SearchRequest &);
};
std::unique_ptr<SearchEngine> make_search_engine(const std::string &name,
                                                 WebTransport transport = fetch_public_web);
Json research_context(Database &, Id session, Id task);
Json research_coverage(Database &, Id session, Id task);
Json research_plan_context(Database &, Id session, Id task);
struct ResearchReferenceError : std::runtime_error {
  Json references;
  ResearchReferenceError(std::string message, Json available)
      : std::runtime_error(std::move(message)), references(std::move(available)) {}
};
struct ResearchEvidenceError : std::runtime_error {
  Json recovery;
  ResearchEvidenceError(std::string message, Json data)
      : std::runtime_error(std::move(message)), recovery(std::move(data)) {}
};
class WebResearch {
  Database &db_;
  Config config_;
  WebTransport transport_;
  std::unique_ptr<SearchOrchestrator> engine_;
  std::shared_ptr<web::WebAcquisitionEngine> acquisition_;
  int searches_ = 0, reads_ = 0;
  Id session_ = 0, task_ = 0, source_cutoff_ = 0;
  std::string turn_;
  std::map<std::string,Json> turn_searches_;
  std::function<void()> service_;
  void network_control();
  Json lookup_claim(Id, Id session, Id task) const;
  Json dispatch_operation(const std::string &, const Json &, Id, Id, const Emit &);
  Json source_excerpt(Id id, Id offset, Id limit, const std::optional<std::string> &query = {},
                      Id tokens = 0);

public:
  WebResearch(Database &, Config, WebTransport transport = fetch_public_web);
  ~WebResearch();
  void service(std::function<void()> callback) {
    service_ = std::move(callback);
  }
  void begin_turn(Id session, Id task, std::string turn = {});
  void rebind_task(Id previous, Id next, const std::string &turn);
  Json settings(const std::optional<bool> &enabled = {});
  std::string guidance() const;
  Json dispatch(const std::string &name, const Json &args, Id session, Id task, const Emit &emit = {});
  void require_plan(Id user_event, const std::string &turn, Id session, Id task);
  bool plan_pending(Id session, Id task) const;
  Json plan(const Json &, Id session, Id task);
  Id question(const std::string &text, bool required, Id session, Id task, Id assumption = 0);
  Json questions(Id session, Id task) const;
  Json references(Id session, Id task) const;
  Json unresolved_required(Id session, Id task) const;
  void disclose_failures(Id session, Id task, const Emit &emit);
  void account_for_pending(Id session, Id task, const Emit &emit);
};
} // namespace saga
