#include <saga/debug.hpp>
#include "internal.hpp"
#include <algorithm>
#include <bit>
#include <sstream>

namespace saga::web {
namespace {
std::string hex(uint64_t value) {
  std::ostringstream out;
  out << std::hex << value;
  return out.str();
}
Error transport_error(const std::exception &e) {
  auto message = std::string(e.what()), low = lower(message);
  auto code = ErrorCode::NetworkError;
  if (low.find("timed out") != std::string::npos || low.find("timeout") != std::string::npos)
    code = ErrorCode::Timeout;
  else if (low.find("redirect") != std::string::npos)
    code = ErrorCode::TooManyRedirects;
  else if (low.find("exceed") != std::string::npos)
    code = ErrorCode::ResponseTooLarge;
  else if (low.find("private") != std::string::npos || low.find("reserved") != std::string::npos ||
           low.find("forbidden") != std::string::npos ||
           low.find("invalid web url") != std::string::npos ||
           low.find("credential-free") != std::string::npos)
    code = ErrorCode::BlockedAddress;
  return {code, message};
}
} // namespace
WebAcquisitionEngine::WebAcquisitionEngine(Database *db, WebTransport transport, WebLimits limits)
    : db_(db), legacy_transport_(std::move(transport)), limits_(limits) {
  if (auto pointer =
          legacy_transport_.target<WebResponse (*)(const std::string &, const std::string &,
                                                   const std::function<void()> &)>();
      pointer && *pointer == fetch_public_web)
    legacy_transport_ = {};
  if (limits_.max_response_bytes == 0 || limits_.max_response_bytes > 16 * 1024 * 1024 ||
      limits_.max_document_chars == 0 || limits_.max_document_chars > 4 * 1024 * 1024 ||
      limits_.max_dom_depth > 256 || limits_.max_rendered_tokens < 128 ||
      limits_.max_redirects < 0 || limits_.max_redirects > 10 || limits_.total_timeout <= 0 ||
      limits_.connect_timeout <= 0)
    throw Error(ErrorCode::ResponseTooLarge, "Invalid acquisition limits");
}
WebResponse WebAcquisitionEngine::fetch(const std::string &target, bool cache) {
  if (service_)
    service_();
  std::string url;
  try {
    url = web_url(target, {}, limits_.allow_private_network);
  } catch (const std::exception &e) {
    throw transport_error(e);
  }
  WebHttpOptions options;
  options.max_response_bytes = limits_.max_response_bytes;
  options.max_redirects = limits_.max_redirects;
  options.connect_timeout = limits_.connect_timeout;
  options.total_timeout = limits_.total_timeout;
  options.allow_private_network = limits_.allow_private_network;
  Json stored = Json::array();
  auto key = digest(url);
  if (db_ && cache)
    stored = db_->query("SELECT * FROM web_http_cache WHERE url_hash=? AND url=?", {key, url});
  if (!stored.empty()) {
    auto headers = Json::parse(stored[0]["headers_json"].get<std::string>());
    if (headers.contains("etag"))
      options.headers["If-None-Match"] = headers["etag"];
    if (headers.contains("last-modified"))
      options.headers["If-Modified-Since"] = headers["last-modified"];
  }
  WebResponse response;
  try {
    response = legacy_transport_ ? legacy_transport_(url, "", service_)
                                 : fetch_web_http(url, "", service_, options);
  } catch (const TurnCancelled &) {
    throw;
  } catch (const Error &) {
    throw;
  } catch (const std::exception &e) {
    throw transport_error(e);
  }
  if (response.body.size() > limits_.max_response_bytes)
    throw Error(ErrorCode::ResponseTooLarge, "Transport response exceeds limit");
  response.requested_url = url;
  if (response.url.empty())
    response.url = url;
  try {
    response.url = web_url(response.url, {}, limits_.allow_private_network);
  } catch (const std::exception &e) {
    throw transport_error(e);
  }
  if (response.status == 304) {
    if (stored.empty())
      throw Error(ErrorCode::NetworkError, "304 without a cached representation");
    auto &row = stored[0];
    response.url = row["final_url"];
    response.status = row["status"];
    response.content_type = row["content_type"];
    response.body = row["body"];
    auto headers = Json::parse(row["headers_json"].get<std::string>())
                       .get<std::map<std::string, std::string>>();
    for (auto &[k, v] : response.headers)
      headers[k] = v;
    response.headers = std::move(headers);
    response.cache_hit = true;
    // Cached redirects must obey the current policy, which may have changed.
    try {
      response.url = web_url(response.url, {}, limits_.allow_private_network);
    } catch (const std::exception &e) {
      throw transport_error(e);
    }
    if (response.body.size() > limits_.max_response_bytes)
      throw Error(ErrorCode::ResponseTooLarge, "Cached response exceeds current limit");
  }
  if (db_ && cache && response.status == 200 &&
      response.headers["cache-control"].find("no-store") == std::string::npos) {
    db_->exec("INSERT INTO "
              "web_http_cache(url_hash,url,final_url,status,content_type,headers_json,body,updated_"
              "at) VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(url_hash) DO UPDATE SET "
              "url=excluded.url,final_url=excluded.final_url,status=excluded.status,content_type="
              "excluded.content_type,headers_json=excluded.headers_json,body=excluded.body,updated_"
              "at=excluded.updated_at",
              {key, url, response.url, response.status, response.content_type,
               Json(response.headers).dump(), response.body, now()});
    db_->sql("DELETE FROM web_http_cache WHERE url_hash IN (SELECT url_hash FROM web_http_cache "
             "ORDER BY updated_at DESC LIMIT -1 OFFSET 128)");
  }
  if (service_)
    service_();
  return response;
}
AcquisitionResult WebAcquisitionEngine::acquire(const AcquisitionRequest &request) {
  TraceSpan acquisition("reader","web_acquisition",{{"url",request.url}});
  trace("reader","reader.fetch_started",{{"url",request.url},{"query",request.focus_query?Json(*request.focus_query):Json()}},DebugProfile::Trace);
  auto budget = request.max_output_tokens ? request.max_output_tokens : limits_.max_rendered_tokens;
  budget = std::min(budget, limits_.max_rendered_tokens);
  if (request.focus_query && request.focus_query->size() > 8192)
    throw Error(ErrorCode::ResponseTooLarge, "Focus query exceeds limit");
  include_diffs_ =
      request.focus_query && (lower(*request.focus_query).find("diff") != std::string::npos ||
                              lower(*request.focus_query).find("patch") != std::string::npos);
  auto response = fetch(request.url, request.allow_cache);
  ExtractionInfo info;
  info.downloaded_bytes = response.body.size();
  info.http_status = response.status;
  info.content_type = response.content_type;
  info.http_cache_hit = response.cache_hit;
  if (response.status == 429)
    throw Error(ErrorCode::RateLimited, "Source rate limited; retry later");
  if (response.status == 401 || response.status == 403)
    throw Error(ErrorCode::AuthenticationRequired,
                "Source requires authentication or refused access");
  if (response.status != 200)
    throw Error(ErrorCode::NetworkError, "Source HTTP status " + std::to_string(response.status));
  auto representation = sniff(response);
  if (representation == Representation::Binary || representation == Representation::Unknown)
    throw Error(ErrorCode::UnsupportedContent,
                "Only textual documents and supported platform resources are acquired");
  SourceMetadata source;
  source.requested_url = response.requested_url;
  source.final_url = response.url;
  CanonicalDocument document;
  bool document_cached = false;
  auto body_hash = digest(response.body), url_hash = digest(response.url);
  if (db_ && request.allow_cache) {
    auto cached = db_->query("SELECT document_json,diagnostics_json FROM web_document_cache WHERE "
                             "url_hash=? AND body_hash=?",
                             {url_hash, body_hash});
    if (!cached.empty()) {
      document = document_from_json(Json::parse(cached[0]["document_json"].get<std::string>()));
      // API resources can change without the surrounding HTML changing. Revalidate
      // their API representations rather than treating the UI body as a version.
      document_cached = document.source.acquisition_path != "API";
      if (document_cached) {
        auto diagnostics = Json::parse(cached[0]["diagnostics_json"].get<std::string>());
        info.profile = static_cast<PageProfile>(
            diagnostics.value("profile_id", static_cast<int>(PageProfile::Unknown)));
        info.blocks_before = diagnostics.value("blocks_before", document.blocks.size());
        info.blocks_after = document.blocks.size();
        auto mode = diagnostics.value("mode", "balanced");
        info.mode = mode == "strict"   ? ExtractionMode::Strict
                    : mode == "recall" ? ExtractionMode::Recall
                                       : ExtractionMode::Balanced;
        auto quality = diagnostics.value("quality", Json::object());
        info.quality = {quality.value("confidence", 0.0),
                        quality.value("source_coverage", 0.0),
                        quality.value("link_density", 0.0),
                        quality.value("boilerplate_probability", 0.0),
                        quality.value("extracted_chars", size_t(0)),
                        quality.value("extracted_blocks", size_t(0)),
                        quality.value("likely_dynamic", false)};
        info.scores = diagnostics.value("score_distribution", std::vector<double>{});
      }
    }
  }
  if (!document_cached) {
    std::optional<HtmlAnalysis> html;
    Json signals = Json::object();
    if (representation == Representation::Html) {
      html = analyze_html(normalize_encoding(response), source, limits_);
      signals = html->signals;
    }
    auto instance = detect(response, html ? html->document : CanonicalDocument{}, signals,
                           request.allow_cache, info);
    source.platform = instance.platform;
    std::optional<CanonicalDocument> structured_document;
    std::vector<std::string> fallbacks;
    if (instance.platform != Platform::Unknown)
      try {
        structured_document = structured(instance, response, request.allow_cache);
        if (!structured_document)
          fallbacks.push_back("Unsupported platform route; generic representation used");
      } catch (const TurnCancelled &) {
        throw;
      } catch (const Error &e) {
        if (e.code == ErrorCode::BlockedAddress)
          throw;
        fallbacks.push_back(sanitize_text(e.what()));
      } catch (const Json::exception &) {
        fallbacks.push_back("PlatformApiFailure: unexpected response shape");
      }
    if (structured_document)
      document = std::move(*structured_document);
    else if (html) {
      html->document.source = source;
      auto origin = url_origin(response.url);
      Json templates = Json::array();
      if (db_ && request.allow_cache)
        templates = db_->query(
            "SELECT text_hash,structure_hash,simhash,distinct_pages FROM web_templates WHERE "
            "origin=? AND distinct_pages>=3 ORDER BY distinct_pages DESC LIMIT 256",
            {origin});
      auto penalty = [&](const Block &b) {
        if (b.inside_main || b.inside_article || b.type == BlockType::Code ||
            b.type == BlockType::Table || b.hints.find("warning") != std::string::npos)
          return 0.0;
        auto hash = digest(sanitize_text(b.text));
        auto structure = digest(b.structural_signature);
        auto near = simhash(b.text);
        for (auto &row : templates) {
          if (row["structure_hash"] != structure)
            continue;
          auto similar = std::stoull(row["simhash"].get<std::string>(), nullptr, 16);
          if (row["text_hash"] == hash ||
              (b.text.size() > 80 && std::popcount(similar ^ near) <= 3))
            return std::min(4.0, row["distinct_pages"].get<int>() * 0.5);
        }
        return 0.0;
      };
      if (db_ && request.allow_cache)
        db_->transaction([&] {
          for (auto &b : html->document.blocks) {
            if (b.text.empty() || b.text.size() > 4096)
              continue;
            auto hash = digest(sanitize_text(b.text)), structure = digest(b.structural_signature);
            db_->exec(
                "INSERT OR IGNORE INTO "
                "web_template_pages(origin,text_hash,structure_hash,page_hash) VALUES(?,?,?,?)",
                {origin, hash, structure, url_hash});
            if (!db_->changes())
              continue;
            db_->exec(
                "INSERT INTO web_templates(origin,text_hash,structure_hash,simhash,distinct_pages) "
                "VALUES(?,?,?,?,1) ON CONFLICT(origin,text_hash,structure_hash) DO UPDATE SET "
                "distinct_pages=distinct_pages+1",
                {origin, hash, structure, hex(simhash(b.text))});
          }
        });
      document = extract_regions(std::move(*html), info, limits_, penalty);
    } else if (representation == Representation::Markdown) {
      source.acquisition_path = "Markdown";
      document = markdown_document(normalize_encoding(response), source, limits_);
      info.profile = PageProfile::Documentation;
    } else if (representation == Representation::PlainText) {
      source.acquisition_path = "PlainText";
      document.source = source;
      auto body = normalize_encoding(response);
      if (body.size() > limits_.max_document_chars)
        throw Error(ErrorCode::ResponseTooLarge, "Plain-text document limit");
      std::istringstream input(body);
      std::string paragraph, line;
      while (std::getline(input, line)) {
        if (trim(line).empty()) {
          if (!paragraph.empty()) {
            Block b;
            b.text = std::move(paragraph);
            document.blocks.push_back(std::move(b));
            paragraph.clear();
          }
        } else
          paragraph += line + '\n';
      }
      if (!paragraph.empty()) {
        Block b;
        b.text = std::move(paragraph);
        document.blocks.push_back(std::move(b));
      }
    } else
      throw Error(ErrorCode::UnsupportedContent,
                  "Unrecognized JSON is not passed to the agent; use a recognized resource URL");
    document.source.fallbacks.insert(document.source.fallbacks.end(), fallbacks.begin(),
                                     fallbacks.end());
    document.source.requested_url = request.url;
    sanitize(document, limits_);
    deduplicate(document);
    for (size_t i = 0; i < document.blocks.size(); ++i)
      document.blocks[i].id = i;
    info.blocks_after = document.blocks.size();
    if (document.blocks.empty() && document.metadata.title.empty())
      throw Error(ErrorCode::ExtractionFailure, "Document is empty after sanitization");
    if (db_ && request.allow_cache &&
        response.headers["cache-control"].find("no-store") == std::string::npos) {
      auto diagnostics = info.json();
      diagnostics["profile_id"] = static_cast<int>(info.profile);
      db_->exec("INSERT INTO "
                "web_document_cache(url_hash,body_hash,document_json,diagnostics_json,updated_at) "
                "VALUES(?,?,?,?,?) ON CONFLICT(url_hash) DO UPDATE SET "
                "body_hash=excluded.body_hash,document_json=excluded.document_json,diagnostics_"
                "json=excluded.diagnostics_json,updated_at=excluded.updated_at",
                {url_hash, body_hash, document_json(document).dump(), diagnostics.dump(), now()});
      db_->sql("DELETE FROM web_document_cache WHERE url_hash IN (SELECT url_hash FROM "
               "web_document_cache ORDER BY updated_at DESC LIMIT -1 OFFSET 128)");
    }
  }
  // The document cache is keyed by the final resource, not the request route.
  document.source.requested_url = response.requested_url;
  document.source.final_url = response.url;
  // Provenance URLs must not prevent identical source content being recognized.
  std::string semantic = document.metadata.title + '\n' + document.metadata.author + '\n';
  for (auto &block : document.blocks)
    semantic += render_block(block) + '\n';
  auto content_hash = digest(semantic);
  auto fingerprint = simhash(semantic);
  info.duplicate = operation_hashes_.contains(content_hash);
  for (auto prior : operation_documents_)
    if (semantic.size() > 400 && std::popcount(fingerprint ^ prior) <= 3) {
      info.duplicate = true;
      break;
    }
  operation_documents_.insert(fingerprint);
  operation_hashes_.insert(content_hash);
  if (db_) {
    db_->exec("INSERT INTO web_document_fingerprints(content_hash,simhash,url_hash,last_seen_at) "
              "VALUES(?,?,?,?) ON CONFLICT(content_hash) DO UPDATE SET "
              "last_seen_at=excluded.last_seen_at",
              {content_hash, hex(fingerprint), url_hash, now()});
  }
  auto selected = select_document(document, request.focus_query, budget, &info);
  auto rendered = render(selected, budget);
  if (info.duplicate) {
    selected.blocks.clear();
    selected.truncated = false;
    rendered = render(selected, budget) + "[Duplicate document omitted from this research "
                                          "operation; use its source_id for focused passages.]\n";
    if (estimate_tokens(rendered) > budget)
      rendered = render(selected, budget);
  }
  info.rendered_tokens = estimate_tokens(rendered);
  trace("reader","reader.parse_completed",{{"url",response.url},{"diagnostics",info.json()},{"provenance",document_json(document)["source"]}},DebugProfile::Trace);
  return {std::move(document), std::move(rendered), std::move(info)};
}
} // namespace saga::web
