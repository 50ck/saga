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
  virtual Json search(const SearchRequest &, const std::function<void()> &service) = 0;
};
class DuckDuckGoEngine final : public SearchEngine {
  WebTransport transport_;

public:
  explicit DuckDuckGoEngine(WebTransport transport = fetch_public_web)
      : transport_(std::move(transport)) {}
  Json capabilities() const override;
  std::string guidance() const override;
  Json search(const SearchRequest &, const std::function<void()> &service) override;
  static Json parse(const WebResponse &, const SearchRequest &);
};
std::unique_ptr<SearchEngine> make_search_engine(const std::string &name,
                                                 WebTransport transport = fetch_public_web);
Json research_context(Database &, Id session, Id task);
class WebResearch {
  Database &db_;
  Config config_;
  WebTransport transport_;
  std::unique_ptr<SearchEngine> engine_;
  std::shared_ptr<web::WebAcquisitionEngine> acquisition_;
  int searches_ = 0, reads_ = 0;
  Id session_ = 0, task_ = 0;
  std::function<void()> service_;
  void network_control();
  Json source_excerpt(Id id, Id offset, Id limit, const std::optional<std::string> &query = {},
                      Id tokens = 0);

public:
  WebResearch(Database &, Config, WebTransport transport = fetch_public_web);
  void service(std::function<void()> callback) {
    service_ = std::move(callback);
  }
  void begin_turn(Id session, Id task);
  Json settings(const std::optional<bool> &enabled = {});
  std::string guidance() const {
    return engine_->guidance();
  }
  Json dispatch(const std::string &name, const Json &args, Id session, Id task);
  Id question(const std::string &text, bool required, Id session, Id task, Id assumption = 0);
  Json questions(Id session, Id task) const;
  Json unresolved_required(Id session, Id task) const;
  void disclose_failures(Id session, Id task, const Emit &emit);
  void account_for_pending(Id session, Id task, const Emit &emit);
};
} // namespace saga
