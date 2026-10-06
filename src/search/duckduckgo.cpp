#include <saga/search.hpp>
#include "../web/internal.hpp"
#include <curl/curl.h>
#if __has_include(<curl/urlapi.h>)
#include <curl/urlapi.h>
#endif
#include <set>
#include <sstream>
#include <algorithm>
namespace saga {
namespace {
struct Url {
  CURLU *value = curl_url();
  Url() {
    if (!value)
      throw std::runtime_error("Cannot initialize URL parser");
  }
  ~Url() { curl_url_cleanup(value); }
  std::string get(int part, unsigned flags = 0) const {
    char *raw = nullptr;
    if (curl_url_get(value, static_cast<decltype(CURLUPART_URL)>(part), &raw, flags))
      return {};
    std::unique_ptr<char, decltype(&curl_free)> owned(raw, curl_free);
    return raw;
  }
};
std::string decode(std::string_view value) {
  std::string out;
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size() && hex(value[i + 1]) >= 0 &&
        hex(value[i + 2]) >= 0) {
      out += static_cast<char>(hex(value[i + 1]) * 16 + hex(value[i + 2]));
      i += 2;
    } else
      out += value[i] == '+' ? ' ' : value[i];
  }
  return out;
}
std::string query_field(std::string_view query, std::string_view name) {
  while (!query.empty()) {
    auto end = query.find('&');
    auto part = query.substr(0, end);
    auto eq = part.find('=');
    if (decode(part.substr(0, eq)) == name)
      return eq == std::string_view::npos ? "" : decode(part.substr(eq + 1));
    if (end == std::string_view::npos)
      break;
    query.remove_prefix(end + 1);
  }
  return {};
}
using SearchNode = web::Node;
std::string attribute(SearchNode *node, const char *name) { return web::attr(node, name); }
std::string node_text(SearchNode *node) {
  std::string result;
  web::walk(node, [&](SearchNode *child, size_t) { result += web::text(child); }, {});
  return trim(utf8_text(result));
}
bool tag(SearchNode *node, std::string_view name) { return web::tag(node) == name; }
bool css(SearchNode *node, std::string_view name) {
  std::istringstream words(attribute(node, "class"));
  std::string word;
  while (words >> word)
    if (word == name)
      return true;
  return false;
}
void walk(SearchNode *node, const std::function<void(SearchNode *)> &visit) {
  for (; node; node = node->next)
    web::walk(node, [&](SearchNode *child, size_t) { visit(child); }, {});
}
SearchNode *next_element(SearchNode *node) {
  for (node = node->next; node; node = node->next)
    if (!web::tag(node).empty())
      return node;
  return nullptr;
}
std::unique_ptr<web::HtmlDocument> html(std::string_view body) {
  return std::make_unique<web::HtmlDocument>(body, web::WebLimits{});
}
bool matches_domain(const std::string &host, const std::string &domain) {
  return host == domain || (host.size() > domain.size() && host.ends_with("." + domain));
}
void validate_domains(const std::vector<std::string> &domains) {
  if (domains.size() > 16)
    throw std::runtime_error("At most 16 domain constraints are supported");
  for (auto &domain : domains)
    if (domain.empty() || domain.size() > 253 || domain != lower(domain) || domain.front() == '.' ||
        domain.back() == '.' ||
        domain.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != std::string::npos)
      throw std::runtime_error(
          "Domain constraints must be lowercase domain names, without a URL or wildcard");
}
std::string submitted_query(const SearchRequest &request) {
  auto query = request.query;
  if (request.include_domains.size() == 1 &&
      query.find("site:" + request.include_domains[0]) == std::string::npos)
    query += " site:" + request.include_domains[0];
  for (auto &domain : request.exclude_domains)
    if (query.find("-site:" + domain) == std::string::npos)
      query += " -site:" + domain;
  return query;
}

bool allowed_result(const std::string &url, const SearchRequest &request) {
  auto host = web_host(url);
  for (auto &domain : request.exclude_domains)
    if (matches_domain(host, domain))
      return false;
  return request.include_domains.empty() ||
         std::any_of(request.include_domains.begin(), request.include_domains.end(),
                     [&](auto &domain) { return matches_domain(host, domain); });
}
}
Json DuckDuckGoEngine::capabilities() const {
  return {{"id", "duckduckgo"},
          {"advanced_query", true},
          {"pagination", true},
          {"domain_constraints", true},
          {"formats", {"html", "text"}}};
}
std::string DuckDuckGoEngine::guidance() const {
  return "Selected search engine: DuckDuckGo. Use focused technical queries with "
         "versions/platforms. Operators: \"exact phrase\", site:domain, -site:domain, "
         "intitle:term, inurl:term, filetype:pdf/doc/docx/xls/xlsx/ppt/pptx/html; +term emphasizes "
         "and -term reduces results. ~\"phrase\" is experimental semantic expansion. Operators may "
         "be imperfect and empty advanced searches may fall back to related results. Inspect "
         "sources; explicit include_domains/exclude_domains are enforced locally. Start with "
         "primary documentation, narrow noise, broaden empty results explicitly, search exact "
         "errors, and investigate contradictions. Read pages before treating claims as verified. "
         "Search snippets are discovery only. Bangs (!site), safe-search bangs and first-result "
         "shortcuts are unsupported; do not use them. HTML/text reading only; a PDF search result "
         "cannot be read in v1.";
}
Json DuckDuckGoEngine::parse(const WebResponse &response, const SearchRequest &request) {
  auto low = lower(response.body);
  if (response.status == 429)
    throw SearchError(SearchErrorCode::RateLimited,"DuckDuckGo rate limited the search; retry later");
  if (response.status == 403 || response.status == 202 ||
      low.find("anomaly.js") != std::string::npos ||
      low.find("challenge-form") != std::string::npos ||
      low.find("bots use duckduckgo") != std::string::npos)
    throw SearchError(SearchErrorCode::BotChallenge,"DuckDuckGo returned a bot challenge; Saga does not bypass CAPTCHA");
  if (response.status != 200)
    throw SearchError(SearchErrorCode::HttpFailure,"DuckDuckGo HTTP status " + std::to_string(response.status));
  auto doc = html(response.body);
  Json results = Json::array();
  std::set<std::string> seen;
  Json next = Json::object();
  bool recognized = false;
  walk(doc->root(), [&](SearchNode *node) {
    if (tag(node, "a") && (css(node, "result__a") || css(node, "result-link") ||
                           (tag(node->parent, "h2") && css(node->parent, "result__title")))) {
      recognized = true;
      auto href = attribute(node, "href");
      std::string url;
      try {
        url = web_url(href, response.url);
        if (web_host(url) == "duckduckgo.com" || web_host(url).ends_with(".duckduckgo.com")) {
          Url u;
          curl_url_set(u.value, CURLUPART_URL, url.c_str(), 0);
          auto destination = query_field(u.get(CURLUPART_QUERY), "uddg");
          if (!destination.empty())
            url = web_url(destination);
          else
            return;
        }
      } catch (const std::exception &) {
        return;
      }
      if (!allowed_result(url, request) || !seen.insert(url).second ||
          results.size() >= static_cast<size_t>(request.limit))
        return;
      auto *container = node;
      while (container->parent && !css(container, "result") && !tag(container, "tr"))
        container = container->parent;
      if (css(container, "result--ad") || css(container, "result--sponsored"))
        return;
      std::string snippet;
      walk(container->first_child, [&](SearchNode *n) {
        if (css(n, "result__snippet") || css(n, "result-snippet"))
          snippet = node_text(n);
      });
      if (snippet.empty() && tag(container, "tr")) {
        auto *row = next_element(container);
        for (int i = 0; row && i < 3 && snippet.empty(); ++i, row = next_element(row)) {
          bool next_result = false;
          walk(row->first_child, [&](SearchNode *n) {
            if (css(n, "result-link"))
              next_result = true;
            if (css(n, "result-snippet"))
              snippet = node_text(n);
          });
          if (next_result)
            break;
        }
      }
      results.push_back({{"title", utf8_excerpt(node_text(node), 512)},
                         {"url", url},
                         {"snippet", utf8_excerpt(snippet, 2048)},
                         {"rank", results.size() + 1}});
    }
    if (tag(node, "form")) {
      Json fields = Json::object();
      walk(node->first_child, [&](SearchNode *n) {
        if (tag(n, "input") && lower(attribute(n, "type")) == "hidden") {
          auto key = attribute(n, "name"), value = attribute(n, "value");
          if (!key.empty() && key.size() < 128 && value.size() < 4096)
            fields[key] = value;
        }
      });
      if (fields.contains("q") && (fields.contains("s") || fields.contains("dc")) &&
          !fields.contains("prev") && !fields.contains("previous"))
        next = fields;
    }
  });
  if (!recognized && low.find("no-results") == std::string::npos &&
      low.find("no more results") == std::string::npos &&
      low.find("no results found") == std::string::npos)
    throw SearchError(SearchErrorCode::InvalidResponse,"DuckDuckGo result format was not recognized");
  auto provider_url = response.url;
  Json result = {{"engine", "duckduckgo"},
                 {"query", request.query},
                 {"submitted_query", submitted_query(request)},
                 {"results", results},
                 {"empty", results.empty()},
                 {"operator_warning",
                  "DuckDuckGo may return related matches; verify constraints against sources"}};
  if (!next.empty()) {
    next["q"] = submitted_query(request);
    auto cursor = Json{{"endpoint", provider_url},
                       {"fields", next},
                       {"query", request.query},
                       {"include_domains", request.include_domains},
                       {"exclude_domains", request.exclude_domains}}
                      .dump();
    if (cursor.size() <= 8192)
      result["next_cursor"] = cursor;
  }
  return result;
}
DuckDuckGoEngine::DuckDuckGoEngine(WebTransport transport, std::shared_ptr<SearchRateLimiter> limiter)
    : transport_(std::move(transport)), limiter_(limiter ? std::move(limiter) : std::make_shared<SearchRateLimiter>()) {}
Json DuckDuckGoEngine::run(const SearchRequest &request, const SearchContext &context) {
  auto q = trim(request.query);
  if (q.empty() || q.size() > 2048 || q.find_first_of("\r\n") != std::string::npos)
    throw std::runtime_error("Search query must contain 1–2048 bytes on one line");
  if (q.find('\\') != std::string::npos)
    throw std::runtime_error("First-result redirects are unsupported");
  std::istringstream tokens(q);
  std::string token;
  while (tokens >> token)
    if (token.find('!') != std::string::npos)
      throw std::runtime_error(
          "DuckDuckGo bangs are unsupported; use the selected engine's results");
  if (request.limit < 1 || request.limit > 10)
    throw std::runtime_error("Search limit must be between 1 and 10");
  validate_domains(request.include_domains);
  validate_domains(request.exclude_domains);
  auto endpoint = std::string("https://html.duckduckgo.com/html/");
  Json fields = {{"q", submitted_query(request)}};
  if (!request.cursor.empty()) {
    if (request.cursor.size() > 8192)
      throw std::runtime_error("Search cursor exceeds limit");
    auto cursor = search_detail::cursor(request.cursor);
    if (cursor.at("query") != request.query ||
        cursor.at("include_domains") != Json(request.include_domains) ||
        cursor.at("exclude_domains") != Json(request.exclude_domains))
      throw std::runtime_error("Search cursor belongs to another query or constraints");
    endpoint = cursor.at("endpoint");
    fields = cursor.at("fields");
    if (endpoint != "https://html.duckduckgo.com/html/" &&
        endpoint != "https://lite.duckduckgo.com/lite/")
      throw std::runtime_error("Invalid DuckDuckGo cursor endpoint");
    if (!fields.is_object() || fields.size() > 32 ||
        fields.value("q", "") != submitted_query(request))
      throw std::runtime_error("Invalid search pagination fields");
    for (auto &[key, value] : fields.items())
      if (!value.is_string() || key.size() > 128 ||
          value.get_ref<const std::string &>().size() > 4096)
        throw std::runtime_error("Invalid search pagination field");
  }
  auto form = [&] {
    std::string body;
    for (auto &[key, value] : fields.items()) {
      if (!body.empty())
        body += '&';
      body += web_encode(key) + "=" + web_encode(value.get<std::string>());
    }
    return body;
  };
  context.check();
  auto response = limiter_->perform(context,[&]{return search_detail::fetch(transport_,endpoint,form(),context,15);});
  // One endpoint fallback for a server-side failure; challenges and rate limits remain visible.
  if (request.cursor.empty() && (response.status >= 500 || response.status == 404)) {
    response = limiter_->perform(context,[&]{return search_detail::fetch(transport_,"https://lite.duckduckgo.com/lite/",form(),context,15);});
  }
  try {return parse(response, request);}
  catch(const SearchError &error) {
    if(error.code==SearchErrorCode::RateLimited || error.code==SearchErrorCode::BotChallenge)limiter_->degrade(context);
    throw;
  }
}
}
