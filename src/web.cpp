#include <saga/debug.hpp>
#include <curl/curl.h>
#include <saga/web.hpp>
#include <saga/search.hpp>
#include <saga/web/acquisition.hpp>
#if __has_include(<curl/multi.h>)
#include <curl/multi.h>
#include <curl/urlapi.h>
#endif
#include "web/internal.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unistd.h>

namespace saga {
namespace {
constexpr size_t response_limit = 2 * 1024 * 1024, text_limit = 256 * 1024;
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
} // namespace
std::string web_encode(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~')
      out += c;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  return out;
}
bool public_web_address(std::string_view address) {
  std::string text(address);
  in_addr v4{};
  in6_addr v6{};
  if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
    auto n = ntohl(v4.s_addr);
    for (auto [network, mask] :
         std::array<std::pair<uint32_t, uint32_t>, 15>{{{0, 0xff000000},
                                                        {0x0a000000, 0xff000000},
                                                        {0x64400000, 0xffc00000},
                                                        {0x7f000000, 0xff000000},
                                                        {0xa9fe0000, 0xffff0000},
                                                        {0xac100000, 0xfff00000},
                                                        {0xc0000000, 0xffffff00},
                                                        {0xc0000200, 0xffffff00},
                                                        {0xc0586300, 0xffffff00},
                                                        {0xc0a80000, 0xffff0000},
                                                        {0xc6120000, 0xfffe0000},
                                                        {0xc6336400, 0xffffff00},
                                                        {0xcb007100, 0xffffff00},
                                                        {0xe0000000, 0xf0000000},
                                                        {0xf0000000, 0xf0000000}}})
      if ((n & mask) == network)
        return false;
    return true;
  }
  if (inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
    auto *b = v6.s6_addr;
    // Global unicast only; reject transition, documentation and special-purpose prefixes.
    return (b[0] & 0xe0) == 0x20 &&
           !(b[0] == 0x20 && b[1] == 0x01 && ((b[2] == 0x0d && b[3] == 0xb8) || b[2] < 2)) &&
           !(b[0] == 0x20 && b[1] == 2) && !(b[0] == 0x3f && b[1] == 0xff && (b[2] & 0xf0) == 0);
  }
  return false;
}
std::string web_url(std::string_view value, std::string_view base, bool allow_private) {
  if (value.empty() || value.size() > 8192 ||
      value.find_first_of("\r\n\t\\") != std::string_view::npos ||
      value.find('\0') != std::string_view::npos)
    throw std::runtime_error("Invalid web URL");
  Url u;
  if (!base.empty() && curl_url_set(u.value, CURLUPART_URL, std::string(base).c_str(), 0))
    throw std::runtime_error("Invalid base URL");
  if (curl_url_set(u.value, CURLUPART_URL, std::string(value).c_str(), 0))
    throw std::runtime_error("Invalid web URL");
  auto scheme = lower(u.get(CURLUPART_SCHEME)), host = lower(u.get(CURLUPART_HOST));
  if ((scheme != "http" && scheme != "https") || host.empty() || !u.get(CURLUPART_USER).empty() ||
      !u.get(CURLUPART_PASSWORD).empty())
    throw std::runtime_error("Only credential-free HTTP/HTTPS URLs are supported");
  if (host.ends_with('.'))
    host.pop_back();
  curl_url_set(u.value, CURLUPART_HOST, host.c_str(), 0);
  curl_url_set(u.value, CURLUPART_SCHEME, scheme.c_str(), 0);
  auto port = u.get(CURLUPART_PORT);
  if ((scheme == "https" && port == "443") || (scheme == "http" && port == "80"))
    curl_url_set(u.value, CURLUPART_PORT, nullptr, 0);
  if (host.starts_with('[') && host.ends_with(']'))
    host = host.substr(1, host.size() - 2);
  if (!allow_private && (host == "localhost" || host.ends_with(".localhost") ||
                         host.ends_with(".local") || host.find('%') != std::string::npos))
    throw std::runtime_error("Local web destinations are forbidden");
  in_addr v4{};
  in6_addr v6{};
  if (!allow_private &&
      (inet_pton(AF_INET, host.c_str(), &v4) == 1 || inet_pton(AF_INET6, host.c_str(), &v6) == 1) &&
      !public_web_address(host))
    throw std::runtime_error("Private or reserved web destinations are forbidden");
  curl_url_set(u.value, CURLUPART_FRAGMENT, nullptr, 0);
  return u.get(CURLUPART_URL);
}
std::string web_host(std::string_view value) {
  Url u;
  auto normalized = web_url(value, {}, true);
  curl_url_set(u.value, CURLUPART_URL, normalized.c_str(), 0);
  auto host = lower(u.get(CURLUPART_HOST));
  if (host.ends_with('.'))
    host.pop_back();
  return host;
}
WebResponse fetch_public_web(const std::string &target, const std::string &form,
                             const std::function<void()> &service) {
  return fetch_web_http(target, form, service, {});
}
WebResponse fetch_web_http(const std::string &target, const std::string &form,
                           const std::function<void()> &service, const WebHttpOptions &options) {
  TraceSpan operation("http","web_request",{{"url",target}},DebugProfile::Wire);
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error("Cannot initialize web HTTP transport");
  });
  auto *version = curl_version_info(CURLVERSION_FIRST);
  if (!version || !(version->features & CURL_VERSION_ASYNCHDNS))
    throw std::runtime_error(
        "Responsive web research requires libcurl with asynchronous DNS support");
  auto url = web_url(target, {}, options.allow_private_network), body = form;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.total_timeout);
  for (int redirect = 0; redirect <= options.max_redirects; ++redirect) {
    if (service)
      service();
    trace("http","http.request.started",{{"url",url},{"redirect",redirect},{"method",body.empty()?"GET":"POST"},{"payload",body}},DebugProfile::Wire);
    struct State {
      WebResponse response;
      std::string failure;
      size_t headers = 0, limit = 0;
      bool allow_private = false;
    } state;
    state.response.url = url;
    state.response.requested_url = target;
    state.limit = options.max_response_bytes;
    state.allow_private = options.allow_private_network;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), curl_easy_cleanup);
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(),
                                                                curl_multi_cleanup);
    if (!easy || !multi)
      throw std::runtime_error("Cannot create web HTTP request");
    auto set = [&](auto option, auto value) {
      if (curl_easy_setopt(easy.get(), option, value) != CURLE_OK)
        throw std::runtime_error("Cannot configure secure web transport");
    };
    set(CURLOPT_URL, url.c_str());
    set(CURLOPT_PROXY, "");
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_CONNECTTIMEOUT, static_cast<long>(options.connect_timeout));
    set(CURLOPT_TIMEOUT, std::max(1L, static_cast<long>((std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count() + 999) / 1000)));
    set(CURLOPT_USERAGENT, "Saga/0.1 (public web research)");
    set(CURLOPT_ACCEPT_ENCODING, "");
    if (version->features & (1 << 16))
      set(CURLOPT_HTTP_VERSION, 4L);
    curl_slist *raw_headers = nullptr;
    struct Headers {
      curl_slist *&value;
      ~Headers() { curl_slist_free_all(value); }
    } headers{raw_headers};
    raw_headers = curl_slist_append(
        raw_headers, "Accept: text/markdown, text/html;q=0.9, application/xhtml+xml;q=0.8, "
                     "text/plain;q=0.7, application/json;q=0.5");
    if (redirect == 0)
      for (auto &[key, value] : options.headers) {
        if (key != "If-None-Match" && key != "If-Modified-Since")
          throw std::runtime_error("Unsupported web request header");
        if (value.find_first_of("\r\n") != std::string::npos)
          throw std::runtime_error("Invalid HTTP cache validator");
        auto line = key + ": " + value;
        auto *added = curl_slist_append(raw_headers, line.c_str());
        if (!added)
          throw std::bad_alloc();
        raw_headers = added;
      }
    set(CURLOPT_HTTPHEADER, raw_headers);
    set(CURLOPT_WRITEDATA, &state);
    set(
        CURLOPT_WRITEFUNCTION, +[](char *p, size_t size, size_t count, void *data) -> size_t {
          auto &s = *static_cast<State *>(data);
          auto bytes = size * count;
          try {
            if (bytes > s.limit - s.response.body.size()) {
              s.failure = "Web response exceeds download limit";
              return 0;
            }
            s.response.body.append(p, bytes);
            return bytes;
          } catch (...) {
            return 0;
          }
        });
    set(CURLOPT_HEADERDATA, &state);
    set(
        CURLOPT_HEADERFUNCTION, +[](char *p, size_t size, size_t count, void *data) -> size_t {
          auto &s = *static_cast<State *>(data);
          auto bytes = size * count;
          try {
            s.headers += bytes;
            if (s.headers > 65536) {
              s.failure = "Web headers exceed limit";
              return 0;
            }
            std::string_view line(p, bytes);
            if (line.starts_with("HTTP/"))
              s.response.headers.clear();
            auto colon = line.find(':');
            if (colon != std::string_view::npos) {
              auto key = lower(std::string(line.substr(0, colon)));
              auto value = trim(line.substr(colon + 1));
              if (key == "content-type")
                s.response.content_type = lower(value);
              else if (key == "location")
                s.response.location = value;
              if (key == "content-type" || key == "content-encoding" || key == "etag" ||
                  key == "last-modified" || key == "cache-control" || key == "retry-after" ||
                  key == "x-ratelimit-remaining" || key == "x-ratelimit-reset" ||
                  key == "ratelimit-remaining" || key == "x-github-enterprise-version" ||
                  key == "server")
                s.response.headers[key] = value;
            }
            return bytes;
          } catch (...) {
            return 0;
          }
        });
    set(CURLOPT_OPENSOCKETDATA, &state);
    set(
        CURLOPT_OPENSOCKETFUNCTION, +[](void *data, int, struct curl_sockaddr *address) -> int {
          auto &s = *static_cast<State *>(data);
          char text[INET6_ADDRSTRLEN]{};
          const void *raw = nullptr;
          if (address->family == AF_INET)
            raw = &reinterpret_cast<sockaddr_in *>(&address->addr)->sin_addr;
          else if (address->family == AF_INET6)
            raw = &reinterpret_cast<sockaddr_in6 *>(&address->addr)->sin6_addr;
          if (!raw || !inet_ntop(address->family, raw, text, sizeof text) ||
              (!s.allow_private && !public_web_address(text))) {
            s.failure = "DNS resolved to a private or reserved address";
            return CURL_SOCKET_BAD;
          }
          return socket(address->family, address->socktype | SOCK_CLOEXEC, address->protocol);
        });
    if (!body.empty()) {
      set(CURLOPT_POST, 1L);
      set(CURLOPT_POSTFIELDS, body.c_str());
      set(CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    if (curl_multi_add_handle(multi.get(), easy.get()) != 0)
      throw std::runtime_error("Cannot start web HTTP request");
    struct Remove {
      CURLM *multi;
      CURL *easy;
      ~Remove() { curl_multi_remove_handle(multi, easy); }
    } remove{multi.get(), easy.get()};
    int running = 1;
    while (running) {
      if (service)
        service();
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Web request timed out");
      if (curl_multi_perform(multi.get(), &running) != 0)
        throw std::runtime_error("Web transfer failed");
      if (running && curl_multi_poll(multi.get(), nullptr, 0, 100, nullptr) != 0)
        throw std::runtime_error("Web transfer poll failed");
    }
    int queued = 0;
    auto *message = curl_multi_info_read(multi.get(), &queued);
    if (!state.failure.empty())
      throw std::runtime_error(state.failure);
    if (!message || message->msg != CURLMSG_DONE || message->data.result != CURLE_OK)
      throw std::runtime_error(message ? std::string("Web transport: ") +
                                             curl_easy_strerror(message->data.result)
                                       : "Web transfer did not finish");
    curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &state.response.status);
    trace("http","http.response.completed",{{"url",url},{"status",state.response.status},{"headers",state.response.headers},{"bytes",state.response.body.size()}},DebugProfile::Wire);
    if(auto logger=debug_logger();logger && logger->enabled(DebugProfile::Wire))trace("http","http.response.body",{{"payload",state.response.body}},DebugProfile::Wire);
    if (state.response.status >= 300 && state.response.status < 400 &&
        !state.response.location.empty()) {
      auto next = web_url(state.response.location, url, options.allow_private_network);
      if (!body.empty() && web_host(next) != web_host(url))
        throw std::runtime_error("Search form cannot redirect to another host");
      if (state.response.status == 301 || state.response.status == 302 ||
          state.response.status == 303)
        body.clear();
      url = next;
      continue;
    }
    if (service)
      service();
    return state.response;
  }
  throw std::runtime_error("Too many web redirects");
}
WebDocument extract_web_document(const WebResponse &response) {
  if (response.status != 200)
    throw web::Error(web::ErrorCode::NetworkError,
                     "Source HTTP status " + std::to_string(response.status));
  if (response.body.size() > 256 * 1024)
    throw web::Error(web::ErrorCode::ResponseTooLarge, "Document limit");
  web::SourceMetadata source;
  source.requested_url = response.url;
  source.final_url = response.url;
  auto kind = web::sniff(response);
  web::CanonicalDocument document;
  if (kind == web::Representation::Html) {
    web::ExtractionInfo info;
    document = web::extract_regions(
        web::analyze_html(web::normalize_encoding(response), source, {}), info, {});
  } else if (kind == web::Representation::Markdown)
    document = web::markdown_document(web::normalize_encoding(response), source, {});
  else if (kind == web::Representation::PlainText) {
    document.source = source;
    web::Block block;
    block.text = web::normalize_encoding(response);
    document.blocks.push_back(std::move(block));
  } else
    throw web::Error(web::ErrorCode::UnsupportedContent, "Unsupported document representation");
  web::sanitize(document);
  web::deduplicate(document);
  std::string body;
  for (auto &block : document.blocks)
    body += web::render_block(block) + "\n";
  return {document.metadata.title, std::move(body), document.truncated};
}
WebResearch::WebResearch(Database &db, Config config, WebTransport transport)
    : db_(db), config_(std::move(config)), transport_(std::move(transport)),
      engine_(std::make_unique<SearchOrchestrator>(db_, config_, transport_)) {
  web::WebLimits limits;
  limits.allow_private_network = config_.web_allow_private_network;
  limits.max_rendered_tokens = config_.web_output_tokens;
  acquisition_ = std::make_shared<web::WebAcquisitionEngine>(&db_, transport_, limits);
}
WebResearch::~WebResearch() = default;
std::string WebResearch::guidance() const { return engine_->guidance(); }
void WebResearch::begin_turn(Id session, Id task, std::string turn) {
  turn_=std::move(turn);
  session_ = session;
  task_ = task;
  searches_ = reads_ = 0;
  acquisition_->begin_operation();
}
Json WebResearch::settings(const std::optional<bool> &enabled) {
  if (enabled) {
    db_.exec("INSERT INTO runtime_settings(key,value,updated_at) VALUES('web_enabled',?,?) ON "
             "CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at",
             {*enabled ? "true" : "false", now()});
    db_.event("web.access_changed", {{"enabled", *enabled}}, session_, task_);
  }
  auto rows = db_.query("SELECT value FROM runtime_settings WHERE key='web_enabled'");
  return {{"enabled", rows.empty() || rows[0]["value"] == "true"},
          {"engine", config_.search_engines.empty() ? config_.search_engine : config_.search_engines.front()},
          {"capabilities", engine_->capabilities()},
          {"searches", searches_},
          {"reads", reads_},
          {"search_limit", config_.web_search_limit},
          {"read_limit", config_.web_read_limit}};
}
void WebResearch::network_control() {
  if (service_)
    service_();
  if (!settings()["enabled"].get<bool>())
    throw std::runtime_error("Public web access is disabled; use /web on to enable it");
}
Json WebResearch::source_excerpt(Id id, Id offset, Id limit,
                                 const std::optional<std::string> &query, Id tokens) {
  auto rows = db_.query("SELECT * FROM web_sources WHERE id=?", {id});
  if (rows.empty())
    throw std::runtime_error("Unknown source in this persona");
  if (offset < 0 || limit < 1 || limit > 12288)
    throw std::runtime_error("Invalid web excerpt offset/limit; maximum 12 KiB");
  auto canonical =
      db_.query("SELECT document_json FROM web_source_documents WHERE source_id=?", {id});
  if (!canonical.empty()) {
    if (tokens < 0 || tokens > 4096 || (tokens && tokens < 128))
      throw std::runtime_error("Web output budget must be 128–4096 tokens");
    auto document =
        web::document_from_json(Json::parse(canonical[0]["document_json"].get<std::string>()));
    size_t total = 0;
    for (const auto &block : document.blocks) total += web::render_block(block).size() + 1;
    size_t start = 0, bytes = 0;
    while (start < document.blocks.size() && bytes < static_cast<size_t>(offset))
      bytes += web::render_block(document.blocks[start++]).size() + 1;
    if (start)
      document.blocks.erase(document.blocks.begin(),
                            document.blocks.begin() + static_cast<std::ptrdiff_t>(start));
    auto selected = web::select_document(
        document, query, static_cast<size_t>(tokens ? tokens : std::min<Id>(2048, limit / 2)));
    auto row = rows[0];
    row.erase("text");
    row["source_id"] = id;
    row["document_event_id"] = row["source_event_id"];
    row["content"] = web::render(selected);
    row["offset"] = bytes;
    size_t next = bytes;
    for (auto &block : selected.blocks)
      next += web::render_block(block).size() + 1;
    if (!query && !selected.blocks.empty()) {
      next = bytes;
      for (auto &block : document.blocks) {
        next += web::render_block(block).size() + 1;
        if (block.id == selected.blocks.back().id)
          break;
      }
    }
    row["next_offset"] = query || next >= total || selected.blocks.empty() ? Json() : Json(next);
    row["total_bytes"] = total;
    row["offset_basis"] = "rendered_body_blocks";
    row["focused"] = query.has_value();
    row["query"] = query ? Json(*query) : Json();
    if(query && offset)throw std::runtime_error("Focused retrieval has no sequential cursor; omit offset or use an unfocused read");
    row["passages"] = Json::array();
    constexpr size_t passage_catalog_limit=32;
    row["passage_catalog_complete"] = selected.blocks.size()<=passage_catalog_limit;
    db_.transaction([&] {
      for (const auto &block : selected.blocks) {
        auto text = web::render_block(block);
        if (text.empty()) continue;
        auto hash = digest(text);
        web::CanonicalDocument selection;
        selection.blocks.push_back(block);
        auto selected_json=web::document_json(selection);
        for(const auto &original:document.blocks)if(original.id==block.id) {
          auto original_text=web::render_block(original);
          selected_json["original_block_hash"]=digest(original_text);
          selected_json["partial"]=original_text!=text;
          if(block.type==web::BlockType::Table) {
            selected_json["original_row_indices"]=Json::array();size_t cursor=0;
            for(const auto &row:block.rows)for(;cursor<original.rows.size();++cursor)if(original.rows[cursor]==row) {selected_json["original_row_indices"].push_back(cursor++);break;}
          }
          break;
        }
        db_.exec("INSERT OR IGNORE INTO web_source_passages(source_id,block_id,text,content_hash,selection_json,created_at) VALUES(?,?,?,?,?,?)",
                 {id,block.id,text,hash,selected_json.dump(),now()});
        auto passage = db_.query("SELECT id FROM web_source_passages WHERE source_id=? AND block_id=? AND content_hash=?",{id,block.id,hash})[0]["id"];
        if(row["passages"].size()<passage_catalog_limit)row["passages"].push_back({{"passage_id",passage},{"block_id",block.id},{"preview",utf8_excerpt(text,120)},{"partial",selected_json.value("partial",false)}});
      }
    });
    row["complete"] = !selected.truncated;
    row["reduced"] = selected.truncated;
    row["untrusted"] = true;
    row["citation"] =
        "[" + row["title"].get<std::string>() + "](" + row["url"].get<std::string>() + ")";
    return row;
  }
  auto row = rows[0];
  auto text = row["text"].get<std::string>();
  auto begin = std::min(static_cast<size_t>(offset), text.size()),
       end = std::min(begin + static_cast<size_t>(limit), text.size());
  while (begin > 0 && begin < text.size() &&
         (static_cast<unsigned char>(text[begin]) & 0xc0) == 0x80)
    --begin;
  end = std::min(begin + static_cast<size_t>(limit), text.size());
  while (end > begin && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
    --end;
  if (end == begin && begin < text.size())
    throw std::runtime_error("Excerpt limit is too small for a complete UTF-8 character");
  row.erase("text");
  row["source_id"] = id;
  row["document_event_id"] = row["source_event_id"];
  row["content"] = text.substr(begin, end - begin);
  row["offset"] = begin;
  row["next_offset"] = end;
  row["total_bytes"] = text.size();
  row["complete"] = end == text.size();
  row["citation"] =
      "[" + row["title"].get<std::string>() + "](" + row["url"].get<std::string>() + ")";
  return row;
}
Json WebResearch::dispatch_operation(const std::string &name, const Json &args, Id session, Id task, const Emit &emit) {
  session_ = session;
  task_ = task;
  if (name == "research_plan")return plan(args,session,task);
  if (name == "research_status")return references(session,task);
  if (name == "research_question") {
    auto id=question(args.at("question"), args.value("required", false), session, task);
    return {{"id",id},{"claim_id",id},{"question_id",id},{"goal_id",lookup_claim(id,session,task)["goal_id"]}};
  }
  if (name == "research_revise") {
    auto original=lookup_claim(args.at("id").get<Id>(),session,task);
    auto text=trim(args.at("question").get<std::string>()),reason=trim(args.at("reason").get<std::string>());
    if(text.empty() || text.size()>512 || text.find_first_of("\r\n")!=std::string::npos || text==original["question"].get<std::string>() || reason.empty() || reason.size()>2048)
      throw std::runtime_error("Revision requires a different concise external proposition and a reason");
    auto user=db_.query("SELECT json_extract(payload_json,'$.content') AS content FROM events WHERE session_id=? AND type='user.message' ORDER BY id DESC LIMIT 1",{session});
    if(!user.empty() && text==trim(user[0]["content"].get<std::string>()))throw std::runtime_error("Operator instructions are not research propositions");
    Id replacement=0;
    db_.transaction([&] {
      replacement=db_.exec("INSERT INTO research_questions(session_id,task_id,goal_id,assumption_id,question,required,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)",
        {session,original["task_id"],original["goal_id"],original["assumption_id"],text,original["required"],now(),now()});
      db_.exec("UPDATE research_questions SET superseded_by=?,updated_at=? WHERE id=?",{replacement,now(),original["id"]});
      db_.event("research.claim_revised",{{"original_claim_id",original["id"]},{"claim_id",replacement},{"reason",reason}},session,task);
    });
    return {{"claim_id",replacement},{"question_id",replacement},{"goal_id",original["goal_id"]},{"proposition",text},{"status","pending"},{"required",original["required"]},{"supersedes",original["id"]}};
  }
  if (name == "research_resolve") {
    auto id = args.at("id").get<Id>();
    auto claim = lookup_claim(id,session,task);
    auto status = args.at("status").get<std::string>(),
         conclusion = args.at("conclusion").get<std::string>();
    auto citations = args.at("sources");
    if (trim(conclusion).empty() || conclusion.size() > 8192)
      throw std::runtime_error("Research resolution requires a concise conclusion");
    if (citations.size() > 8)
      throw std::runtime_error("Use at most eight focused source passages per research conclusion");
    auto failure=[&](const std::string &message,size_t index,const Json &cite) -> void {
      Json recovery={{"claim_id",id},{"citation_index",index},{"proposition",claim["question"]}};
      if(cite.contains("source_id"))recovery["recovery_action"]={{"tool","web_read"},{"arguments",{{"source_id",cite["source_id"]},{"query",claim["question"]},{"question_id",id}}}};
      else if(cite.contains("passage_id"))recovery["recovery_action"]={{"tool","web_read"},{"arguments",{{"passage_id",cite["passage_id"]},{"question_id",id}}}};
      else if(cite.contains("event_id"))recovery["recovery_action"]={{"tool","recall_observation"},{"arguments",{{"event_id",cite["event_id"]}}}};
      else recovery["recovery_action"]={{"tool","research_status"},{"arguments",Json::object()}};
      throw ResearchEvidenceError(message,std::move(recovery));
    };
    if ((status=="supported" || status=="contradicted") && citations.empty())failure("Supported/contradicted research requires fetched passages; read source documents or mark the claim unverified",0,Json::object());
    if(status!="supported" && status!="contradicted" && status!="unverified")throw std::runtime_error("Invalid research status");
    Json assessment=args.value("assessment",Json::object());
    bool evaluated=status=="supported" || status=="contradicted";
    if(evaluated || !assessment.empty()) {
      if(!assessment.is_object() || assessment.value("proposition","")!=claim["question"].get<std::string>() || trim(assessment.value("rationale","")).empty() || assessment.value("rationale","").size()>2048)
        failure("Assessment must address the exact original proposition and explain the evidence. Use research_revise for a corrected proposition.",0,Json::object());
      auto coverage=assessment.value("coverage","");
      if(coverage!="full" && coverage!="partial" && coverage!="none")failure("Assessment coverage must be full, partial or none",0,Json::object());
      if(evaluated && coverage!="full")failure("Partial coverage cannot establish or contradict the entire claim; resolve unverified or split the claim",0,Json::object());
    }
    bool supports=false,contradicts=false;
    size_t index=0;
    for (auto &cite : citations) {
      auto relation=cite.value("relation","");
      if(relation!="supports" && relation!="contradicts" && relation!="partial" && relation!="background")failure("Each citation needs its relation to the original proposition",index,cite);
      supports |= relation=="supports";contradicts |= relation=="contradicts";
      std::string text,hash;
      if(cite.contains("passage_id")) {
        if(cite.contains("event_id"))failure("A passage and local event cannot identify the same citation",index,cite);
        auto passage=db_.query("SELECT * FROM web_source_passages WHERE id=?",{cite["passage_id"]});
        if(passage.empty())failure("Unknown immutable passage_id; use IDs returned by web_read",index,Json::object());
        if(cite.contains("source_id") && cite["source_id"]!=passage[0]["source_id"])failure("Passage belongs to a different source",index,cite);
        cite["source_id"]=passage[0]["source_id"];text=passage[0]["text"];hash=passage[0]["content_hash"];
        cite["basis"]="web_document";
      } else if(cite.contains("event_id")) {
        if(cite.contains("source_id"))failure("Choose one evidence reference",index,cite);
        auto events=db_.query("SELECT type,payload_json FROM events WHERE id=?",{cite["event_id"]});
        if(events.empty() || events[0]["type"]!="tool.completed")failure("Local evidence must be a completed file_read observation",index,cite);
        auto payload=Json::parse(events[0]["payload_json"].get<std::string>());
        auto result=payload.value("result",Json::object());
        if(payload.value("tool","")!="file_read" || result.contains("error") || !result.value("content",Json()).is_string() || result.value("encoding","")!="utf-8")failure("Local documentation evidence requires file_read content, never a tool acknowledgement or execution claim",index,cite);
        text=result["content"];hash=digest(text);cite["basis"]="local_document";
      } else if(cite.contains("source_id")) {
        auto source=db_.query("SELECT text,kind,content_hash FROM web_sources WHERE id=?",{cite["source_id"]});
        if(source.empty() || source[0]["kind"]!="document")failure("Search snippets are discovery; cite a fetched document passage",index,cite);
        text=source[0]["text"];hash=source[0]["content_hash"];cite["basis"]="web_document";
      } else failure("Cite passage_id, or source_id/event_id with an exact quote",index,cite);
      if(!cite.contains("passage_id") || cite.contains("quote")) {
        auto quote=cite.value("quote","");
        if(quote.empty() || quote.size()>4096 || text.find(quote)==std::string::npos)
          failure("Citation quote is not present in the immutable snapshot. Reread the source and cite a returned passage_id; do not paraphrase a quotation.",index,cite);
      }
      cite["content_hash"]=hash;cite["citation_validated"]=true;++index;
    }
    if(evaluated && ((status=="supported" && (!supports || contradicts)) || (status=="contradicted" && (!contradicts || supports))))
      failure("Citation relations conflict with the requested status; conflicting evidence remains unverified",0,Json::object());
    if(!assessment.empty()) {assessment["assessed_by"]="agent";assessment["citation_validation"]="deterministic";assessment["semantic_validation"]="agent_assessed";}
    if (status == "unverified" && citations.empty() && claim["required"] == 1 && settings()["enabled"].get<bool>()) {
      auto attempts=db_.query("SELECT id FROM research_attempts WHERE claim_id=? LIMIT 1",{id});
      if (attempts.empty())
        throw std::runtime_error(
            "Required research must be attempted before marking it unverified");
    }
    db_.exec("UPDATE research_questions SET "
             "status=?,conclusion=?,sources_json=?,assessment_json=?,disclosed=0,updated_at=? WHERE id=?",
             {status, conclusion, citations.dump(), assessment.dump(), now(), id});
    auto recorded=args;recorded["sources"]=citations;recorded["assessment"]=assessment;
    db_.event("research.resolved", recorded, session, task);
    return {{"id", id}, {"claim_id",id},{"status", status}, {"sources", citations},{"assessment",assessment}};
  }
  if (name == "web_search") {
    network_control();
    if (searches_ >= config_.web_search_limit)
      throw std::runtime_error(
          "Research search budget exhausted; disclose uncertainty or continue next turn");
    ++searches_;
    SearchRequest request{args.at("query"), args.value("cursor", ""), args.value("limit", 5),
                          args.value("include_domains", std::vector<std::string>{}),
                          args.value("exclude_domains", std::vector<std::string>{})};
    if (request.cursor.starts_with("cursor:")) {
      auto cursor =
          db_.query("SELECT cursor_json FROM web_search_cursors WHERE token=?", {request.cursor});
      if (cursor.empty())
        throw std::runtime_error("Unknown stored search cursor; repeat the search");
      request.cursor = cursor[0]["cursor_json"];
    }
    SearchContext context; context.human_initiated = true; context.service = [&] { network_control(); }; context.emit = emit;
    auto result = engine_->search(request, context);
    if (result.contains("next_cursor")) {
      auto token = "cursor:" + uuid();
      db_.exec("INSERT INTO web_search_cursors(token,cursor_json,created_at) VALUES(?,?,?)",
               {token, result["next_cursor"], now()});
      result["next_cursor"] = token;
      db_.sql("DELETE FROM web_search_cursors WHERE token IN (SELECT token FROM web_search_cursors "
              "ORDER BY created_at DESC LIMIT -1 OFFSET 128)");
    }
    db_.transaction([&] {
      auto ev = db_.event("web.search_observed",
                          {{"engine", result["engine"]},
                           {"query", request.query},
                           {"submitted_query", result["submitted_query"]},
                           {"results", result["results"]}},
                          session, task);
      for (auto &entry : result["results"]) {
        entry["source_id"] =
            db_.exec("INSERT INTO "
                     "web_sources(session_id,task_id,kind,engine,url,title,query,retrieved_at,"
                     "content_hash,text,source_event_id) VALUES(?,?,'search',?,?,?,?,?,?,?,?)",
                     {session, task ? Json(task) : Json(), result["engine"], entry["url"],
                      entry["title"], result["submitted_query"], now(),
                      digest(entry["snippet"].get<std::string>()), entry["snippet"], ev});
      }
    });
    return result;
  }
  if (name == "web_read" || name == "web_fetch") {
    Id id = args.value("source_id", Id(0));
    if(args.contains("passage_id")) {
      if(args.contains("url") || args.value("refresh",false) || args.value("offset",Id(0))!=0)throw std::runtime_error("Passage reads address one immutable cached block; URL/refresh/offset cannot replace it");
      auto rows=db_.query("SELECT p.*,s.title,s.url FROM web_source_passages p JOIN web_sources s ON s.id=p.source_id WHERE p.id=?",{args["passage_id"]});
      if(rows.empty())throw ResearchEvidenceError("Unknown immutable passage_id",{{"recovery_action",{{"tool","research_status"},{"arguments",Json::object()}}}});
      if(id && rows[0]["source_id"]!=id)throw std::runtime_error("Passage belongs to a different source");
      auto row=rows[0];row["content"]=row["text"];row.erase("text");row["selection_partial"]=Json::parse(row["selection_json"].get<std::string>()).value("partial",false);row.erase("selection_json");row["passage_id"]=row["id"];row.erase("id");row["kind"]="document";row["untrusted"]=true;row["complete"]=true;row["next_offset"]=Json();
      return row;
    }
    std::string url;
    if (id) {
      auto rows = db_.query("SELECT kind,url FROM web_sources WHERE id=?", {id});
      if (rows.empty())
        throw std::runtime_error("Unknown source in this persona");
      if (rows[0]["kind"] == "document" && !args.value("refresh", false))
        return source_excerpt(id, args.value("offset", Id(0)), args.value("limit", Id(12288)),
                              args.contains("query")
                                  ? std::optional<std::string>(args["query"].get<std::string>())
                                  : std::nullopt,
                              args.value("max_output_tokens", Id(0)));
      url = rows[0]["url"];
    } else if (args.contains("url"))
      url = args.at("url");
    else
      throw std::runtime_error("web_read requires a URL or source_id");
    if (args.contains("url") && id &&
        web_url(args.at("url").get<std::string>(), {}, config_.web_allow_private_network) !=
            web_url(url, {}, config_.web_allow_private_network))
      throw std::runtime_error("URL and source_id refer to different sources");
    url = web_url(url, {}, config_.web_allow_private_network);
    network_control();
    if (reads_ >= config_.web_read_limit)
      throw std::runtime_error("Research page-read budget exhausted");
    ++reads_;
    acquisition_->service([&] { network_control(); });
    auto tokens = args.value("max_output_tokens", Id(2048));
    if (tokens < 128 || tokens > 4096)
      throw std::runtime_error("Web output budget must be 128–4096 tokens");
    web::AcquisitionRequest request;
    request.url = url;
    request.max_output_tokens = static_cast<size_t>(tokens);
    request.allow_cache = !args.value("refresh", false);
    if (args.contains("query"))
      request.focus_query = args.at("query").get<std::string>();
    auto acquired = acquisition_->acquire(request);
    network_control();
    auto &document = acquired.document;
    if (document.metadata.title.empty())
      document.metadata.title = document.source.final_url;
    auto full = web::render(document);
    auto diagnostics = acquired.extraction.json();
    auto provenance = web::document_json(document)["source"];
    for (auto *key : {"requested_url", "final_url", "canonical_url"})
      if (provenance.contains(key))
        provenance[key] = web::redacted_url(provenance[key].get<std::string>());
    db_.transaction([&] {
      auto ev = db_.event("web.document_observed",
                          {{"url", web::redacted_url(document.source.final_url)},
                           {"title", document.metadata.title},
                           {"content_hash", digest(full)},
                           {"truncated", document.truncated},
                           {"extraction", diagnostics},
                           {"provenance", provenance}},
                          session, task);
      id = db_.exec("INSERT INTO "
                    "web_sources(session_id,task_id,kind,engine,url,title,retrieved_at,content_"
                    "hash,text,truncated,source_event_id) VALUES(?,?,'document',?,?,?,?,?,?,?,?)",
                    {session, task ? Json(task) : Json(), "web_acquisition",
                     document.source.final_url, document.metadata.title, now(), digest(full), full,
                     document.truncated, ev});
      db_.exec("INSERT INTO web_source_documents(source_id,document_json,diagnostics_json) "
               "VALUES(?,?,?)",
               {id, web::document_json(document).dump(), diagnostics.dump()});
    });
    auto result = source_excerpt(id, 0, args.value("limit", Id(12288)),request.focus_query,tokens);
    result["untrusted"] = true;
    if(acquired.extraction.duplicate) {
      result["content"]=std::move(acquired.rendered);result["passages"]=Json::array();result["next_offset"]=Json();result["complete"]=false;
    }
    result["duplicate"] = acquired.extraction.duplicate;
    result["acquisition_path"] = document.source.acquisition_path;
    result["platform"] = web::platform_name(document.source.platform);
    return result;
  }
  throw std::runtime_error("Unknown web research tool");
}
} // namespace saga
