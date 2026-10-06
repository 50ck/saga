#include "internal.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <iconv.h>
#include <lexbor/unicode/unicode.h>
#include <md4c.h>
#include <sstream>
#include <unordered_set>

extern "C" const char *saga_web_named_entity(const char *, size_t, size_t *);

namespace saga::web {
namespace {
std::string markdown_entity(std::string_view entity) {
  if (entity.size() < 3 || entity.front() != '&')
    return std::string(entity);
  entity.remove_prefix(1);
  if (entity.front() != '#') {
    size_t length = 0;
    auto *value = saga_web_named_entity(entity.data(), entity.size(), &length);
    return value ? std::string(value, length) : "&" + std::string(entity);
  }
  entity.remove_prefix(1);
  if (entity.ends_with(';'))
    entity.remove_suffix(1);
  int base = 10;
  if (entity.starts_with('x') || entity.starts_with('X')) {
    entity.remove_prefix(1);
    base = 16;
  }
  uint32_t cp = 0;
  auto parsed = std::from_chars(entity.data(), entity.data() + entity.size(), cp, base);
  if (parsed.ec != std::errc() || parsed.ptr != entity.data() + entity.size() || cp == 0 ||
      cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
    return "�";
  std::string out;
  if (cp < 0x80)
    out += static_cast<char>(cp);
  else if (cp < 0x800) {
    out += static_cast<char>(0xc0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xe0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  }
  return out;
}
} // namespace
std::string error_name(ErrorCode c) {
  constexpr std::array names = {"NetworkError",
                                "Timeout",
                                "TooManyRedirects",
                                "ResponseTooLarge",
                                "UnsupportedContent",
                                "InvalidEncoding",
                                "BlockedAddress",
                                "PlatformApiFailure",
                                "ExtractionFailure",
                                "DynamicContentUnavailable",
                                "LowConfidenceExtraction",
                                "RateLimited",
                                "AuthenticationRequired"};
  return names.at(static_cast<size_t>(c));
}
std::string platform_name(Platform p) {
  constexpr std::array names = {"web",     "github", "github_enterprise", "gitlab",
                                "forgejo", "gitea",  "discourse",         "mediawiki"};
  return names.at(static_cast<size_t>(p));
}
std::string resource_name(ResourceType r) {
  constexpr std::array names = {"unknown",    "article",   "documentation", "forum_thread",
                                "repository", "directory", "file",          "commit",
                                "diff",       "issue",     "pull_request",  "merge_request",
                                "release",    "wiki_page"};
  return names.at(static_cast<size_t>(r));
}
std::string profile_name(PageProfile p) {
  constexpr std::array names = {"prose",     "documentation", "discussion",
                                "reference", "listing",       "unknown"};
  return names.at(static_cast<size_t>(p));
}
Json ExtractionInfo::json() const {
  return {{"profile", profile_name(profile)},
          {"mode", mode == ExtractionMode::Strict   ? "strict"
                   : mode == ExtractionMode::Recall ? "recall"
                                                    : "balanced"},
          {"quality",
           {{"confidence", quality.confidence},
            {"source_coverage", quality.source_coverage},
            {"link_density", quality.link_density},
            {"boilerplate_probability", quality.boilerplate_probability},
            {"extracted_chars", quality.extracted_chars},
            {"extracted_blocks", quality.extracted_blocks},
            {"likely_dynamic", quality.likely_dynamic}}},
          {"downloaded_bytes", downloaded_bytes},
          {"http_status", http_status},
          {"content_type", content_type},
          {"blocks_before", blocks_before},
          {"blocks_after", blocks_after},
          {"selected_blocks", selected_blocks},
          {"rendered_tokens", rendered_tokens},
          {"http_cache_hit", http_cache_hit},
          {"instance_cache_hit", instance_cache_hit},
          {"duplicate", duplicate},
          {"reduced", reduced},
          {"score_distribution", scores}};
}
std::string decoded(std::string_view value) {
  std::string out;
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
  };
  for (size_t i = 0; i < value.size(); ++i)
    if (value[i] == '%' && i + 2 < value.size() && hex(value[i + 1]) >= 0 &&
        hex(value[i + 2]) >= 0) {
      out += static_cast<char>((hex(value[i + 1]) << 4) | hex(value[i + 2]));
      i += 2;
    } else
      out += value[i];
  return out;
}
std::string url_origin(std::string_view url) {
  auto at = url.find("://");
  if (at == std::string_view::npos)
    throw Error(ErrorCode::BlockedAddress, "Invalid URL origin");
  auto end = url.find('/', at + 3);
  return std::string(url.substr(0, end));
}
std::string clean_url(std::string_view input, std::string_view base) {
  auto fragment_at = input.find('#');
  auto fragment = fragment_at == std::string_view::npos ? std::string()
                                                        : std::string(input.substr(fragment_at));
  auto url = web_url(input, base);
  auto at = url.find('?');
  if (at == std::string::npos)
    return url + fragment;
  auto query = std::string_view(url).substr(at + 1);
  std::string result = url.substr(0, at);
  bool first = true;
  while (!query.empty()) {
    auto end = query.find('&');
    auto pair = query.substr(0, end);
    auto key = lower(decoded(pair.substr(0, pair.find('='))));
    if (!key.starts_with("utm_") && key != "fbclid" && key != "gclid") {
      result += first ? '?' : '&';
      result += pair;
      first = false;
    }
    if (end == std::string_view::npos)
      break;
    query.remove_prefix(end + 1);
  }
  return result + fragment;
}
std::string redacted_url(std::string_view input) {
  auto at = input.find('?');
  if (at == std::string_view::npos)
    return std::string(input);
  return std::string(input.substr(0, at)) + "?[redacted]";
}
Representation sniff(const WebResponse &r) {
  auto mime = lower(r.content_type), head = lower(trim(std::string_view(r.body).substr(0, 1024)));
  if (head.starts_with("\xef\xbb\xbf"))
    head.erase(0, 3);
  if (head.starts_with("%pdf") || head.starts_with("pk\003\004") || head.starts_with("\x89png") ||
      head.starts_with("gif8") ||
      head.starts_with("\x7f"
                       "elf") ||
      mime.starts_with("image/") || mime.starts_with("audio/") || mime.starts_with("video/") ||
      mime.find("pdf") != std::string::npos || mime.find("zip") != std::string::npos)
    return Representation::Binary;
  if (head.starts_with("<!doctype html") || head.starts_with("<html") ||
      head.starts_with("<head") || head.starts_with("<body") || head.starts_with("<article") ||
      head.starts_with("<main") || head.starts_with("<div") || head.starts_with("<p>") ||
      head.starts_with("<h1"))
    return Representation::Html;
  if (mime.find("markdown") != std::string::npos)
    return Representation::Markdown;
  if (mime.find("html") != std::string::npos)
    return Representation::Html;
  if (head.starts_with('{') || head.starts_with('[') || mime.find("json") != std::string::npos)
    return Representation::Json;
  if (r.body.find('\0') != std::string::npos && !r.body.starts_with("\xff\xfe") &&
      !r.body.starts_with("\xfe\xff"))
    return Representation::Binary;
  if (head.starts_with("# ") || head.starts_with("## ") || head.starts_with("```"))
    return Representation::Markdown;
  if (mime.starts_with("text/") || mime.empty())
    return Representation::PlainText;
  return Representation::Unknown;
}
std::string normalize_encoding(const WebResponse &response) {
  std::string charset = response.charset, body = response.body;
  auto type = lower(response.content_type);
  if (charset.empty()) {
    auto at = type.find("charset=");
    if (at != std::string::npos) {
      charset = trim(std::string_view(type).substr(at + 8));
      if (!charset.empty() && (charset.front() == '\"' || charset.front() == '\''))
        charset.erase(0, 1);
      auto end = charset.find_first_of("; \"'");
      charset = charset.substr(0, end);
    }
  }
  if (body.starts_with("\xef\xbb\xbf")) {
    body.erase(0, 3);
    charset = "utf-8";
  } else if (body.starts_with("\xff\xfe")) {
    body.erase(0, 2);
    charset = "utf-16le";
  } else if (body.starts_with("\xfe\xff")) {
    body.erase(0, 2);
    charset = "utf-16be";
  }
  if (charset.empty()) {
    // Encoding sniff only, not HTML parsing. WHATWG metadata is bounded to the preamble.
    auto prefix = lower(std::string(std::string_view(body).substr(0, 4096)));
    auto at = prefix.find("charset=");
    if (at != std::string::npos) {
      at += 8;
      while (at < prefix.size() && (prefix[at] == '\'' || prefix[at] == '\"' || prefix[at] == ' '))
        ++at;
      auto end = prefix.find_first_of("\"' >;/\r\n", at);
      charset = prefix.substr(at, end - at);
    }
  }
  charset = lower(charset);
  if (charset.empty() || charset == "utf-8" || charset == "utf8")
    return utf8_text(body);
  iconv_t converter = iconv_open("UTF-8", charset.c_str());
  if (converter == reinterpret_cast<iconv_t>(-1))
    throw Error(ErrorCode::InvalidEncoding, "Unsupported charset: " + sanitize_text(charset));
  struct Close {
    iconv_t value;
    ~Close() { iconv_close(value); }
  } close{converter};
  std::string output(body.size() * 4 + 16, '\0');
  char *in = body.data();
  char *out = output.data();
  size_t remaining = body.size(), available = output.size();
  while (remaining) {
    if (iconv(converter, &in, &remaining, &out, &available) != static_cast<size_t>(-1))
      break;
    if (errno == E2BIG)
      throw Error(ErrorCode::InvalidEncoding, "Encoding expansion exceeds limit");
    if (available < 3)
      break;
    *out++ = '\xef';
    *out++ = '\xbf';
    *out++ = '\xbd';
    available -= 3;
    ++in;
    --remaining;
  }
  output.resize(output.size() - available);
  return output;
}
std::string sanitize_text(std::string_view raw, bool preserve) {
  auto valid = utf8_text(raw);
  std::string text;
  bool space = false;
  unsigned blank = 0;
  for (size_t i = 0; i < valid.size();) {
    auto c = static_cast<unsigned char>(valid[i]);
    if (c == 27) {
      ++i;
      if (i < valid.size() && valid[i] == '[') {
        ++i;
        while (i < valid.size() && !(valid[i] >= 0x40 && valid[i] <= 0x7e))
          ++i;
        if (i < valid.size())
          ++i;
      } else if (i < valid.size() && valid[i] == ']') {
        ++i;
        while (i < valid.size() && valid[i] != 7 &&
               !(valid[i] == 27 && i + 1 < valid.size() && valid[i + 1] == '\\'))
          ++i;
        if (i < valid.size())
          i += valid[i] == 7 ? 1 : 2;
      }
      continue;
    }
    if (c == 13) {
      if (i + 1 < valid.size() && valid[i + 1] == '\n')
        ++i;
      c = '\n';
    }
    if ((c < 32 && c != '\n' && c != '\t') || c == 127) {
      ++i;
      continue;
    }
    if (c == 0xc2 && i + 1 < valid.size() && static_cast<unsigned char>(valid[i + 1]) >= 0x80 &&
        static_cast<unsigned char>(valid[i + 1]) <= 0x9f) {
      i += 2;
      continue;
    }
    if (valid.compare(i, 2, "\xc2\xa0") == 0) {
      i += 2;
      c = ' ';
    } else if (valid.compare(i, 3, "\xe2\x80\x8b") == 0 ||
               valid.compare(i, 3, "\xe2\x80\x8c") == 0 ||
               valid.compare(i, 3, "\xe2\x80\x8d") == 0 ||
               valid.compare(i, 3, "\xef\xbb\xbf") == 0 ||
               (c == 0xe2 && i + 2 < valid.size() &&
                static_cast<unsigned char>(valid[i + 1]) == 0x80 &&
                static_cast<unsigned char>(valid[i + 2]) >= 0xaa &&
                static_cast<unsigned char>(valid[i + 2]) <= 0xae) ||
               (c == 0xe2 && i + 2 < valid.size() &&
                static_cast<unsigned char>(valid[i + 1]) == 0x81 &&
                static_cast<unsigned char>(valid[i + 2]) >= 0xa6 &&
                static_cast<unsigned char>(valid[i + 2]) <= 0xa9)) {
      i += 3;
      continue;
    } else
      ++i;
    if (!preserve && (c == ' ' || c == '\t')) {
      if (space)
        continue;
      space = true;
      text += ' ';
      continue;
    }
    if (!preserve && c == '\n') {
      if (++blank > 2)
        continue;
    } else if (c != ' ' && c != '\t')
      blank = 0;
    space = false;
    text += static_cast<char>(c);
  }
  if (preserve)
    return text;
  auto *normalizer = lxb_unicode_normalizer_create();
  if (!normalizer)
    throw std::bad_alloc();
  struct Destroy {
    lxb_unicode_normalizer_t *value;
    ~Destroy() { lxb_unicode_normalizer_destroy(value, true); }
  } destroy{normalizer};
  if (lxb_unicode_normalizer_init(normalizer, LXB_UNICODE_NFC) != LXB_STATUS_OK)
    throw Error(ErrorCode::InvalidEncoding, "Cannot initialize Unicode normalization");
  struct State {
    std::string text;
    bool failed = false;
  } state;
  auto status = lxb_unicode_normalize(
      normalizer, reinterpret_cast<const lxb_char_t *>(text.data()), text.size(),
      +[](const lxb_char_t *p, size_t n, void *opaque) -> lxb_status_t {
        auto &s = *static_cast<State *>(opaque);
        try {
          s.text.append(reinterpret_cast<const char *>(p), n);
          return LXB_STATUS_OK;
        } catch (...) {
          s.failed = true;
          return LXB_STATUS_ERROR;
        }
      },
      &state, true);
  if (status != LXB_STATUS_OK || state.failed)
    throw Error(ErrorCode::InvalidEncoding, "Unicode normalization failed");
  return trim(state.text);
}
namespace {
struct MarkdownParser {
  CanonicalDocument document;
  std::vector<Block> stack;
  std::vector<MD_BLOCKTYPE> types;
  std::vector<std::string> links;
  std::vector<size_t> inline_code;
  unsigned list_depth = 0;
  bool raw_html = false;
  std::exception_ptr error;
  const WebLimits &limits;
  explicit MarkdownParser(const SourceMetadata &source, const WebLimits &l) : limits(l) {
    document.source = source;
  }
  void enter(MD_BLOCKTYPE type, void *detail) {
    types.push_back(type);
    if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) {
      ++list_depth;
      return;
    }
    Block b;
    switch (type) {
    case MD_BLOCK_H:
      b.type = BlockType::Heading;
      b.level = static_cast<MD_BLOCK_H_DETAIL *>(detail)->level;
      break;
    case MD_BLOCK_CODE: {
      b.type = BlockType::Code;
      auto *d = static_cast<MD_BLOCK_CODE_DETAIL *>(detail);
      b.language.assign(d->lang.text ? d->lang.text : "", d->lang.size);
      break;
    }
    case MD_BLOCK_LI:
      b.type = BlockType::ListItem;
      b.indent = list_depth ? list_depth - 1 : 0;
      break;
    case MD_BLOCK_QUOTE:
      b.type = BlockType::Quote;
      break;
    case MD_BLOCK_TABLE:
      b.type = BlockType::Table;
      break;
    case MD_BLOCK_TD:
    case MD_BLOCK_TH:
      b.type = BlockType::Paragraph;
      break;
    case MD_BLOCK_HTML:
      raw_html = true;
      return;
    case MD_BLOCK_P:
      if (!stack.empty() && stack.back().type == BlockType::ListItem)
        return;
      break;
    default:
      return;
    }
    if (stack.size() >= limits.max_dom_depth)
      throw Error(ErrorCode::ResponseTooLarge, "Markdown nesting limit");
    stack.push_back(std::move(b));
  }
  void leave(MD_BLOCKTYPE type) {
    if (!types.empty())
      types.pop_back();
    if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) {
      if (list_depth)
        --list_depth;
      return;
    }
    if (type == MD_BLOCK_HTML) {
      raw_html = false;
      return;
    }
    if (type == MD_BLOCK_TR) {
      if (!stack.empty() && stack.back().type == BlockType::Table)
        stack.back().rows.emplace_back();
      return;
    }
    bool take = type == MD_BLOCK_H || type == MD_BLOCK_CODE || type == MD_BLOCK_LI ||
                type == MD_BLOCK_QUOTE || type == MD_BLOCK_TABLE || type == MD_BLOCK_TD ||
                type == MD_BLOCK_TH ||
                (type == MD_BLOCK_P && !stack.empty() && stack.back().type == BlockType::Paragraph);
    if (!take || stack.empty())
      return;
    auto block = std::move(stack.back());
    stack.pop_back();
    if (!stack.empty() && stack.back().type == BlockType::Table) {
      auto &table = stack.back();
      if (table.rows.empty())
        table.rows.emplace_back();
      table.rows.back().push_back(std::move(block.text));
    } else if (!stack.empty() &&
               (stack.back().type == BlockType::Quote || stack.back().type == BlockType::ListItem))
      stack.back().children.push_back(std::move(block));
    else {
      block.id = document.blocks.size();
      if (document.blocks.size() >= limits.max_blocks)
        throw Error(ErrorCode::ResponseTooLarge, "Markdown block limit");
      document.blocks.push_back(std::move(block));
    }
  }
  template <class F> int safe(F f) {
    try {
      f();
      return 0;
    } catch (...) {
      error = std::current_exception();
      return -1;
    }
  }
};
std::string fence(std::string_view text) {
  size_t largest = 2, run = 0;
  for (char c : text) {
    if (c == '`')
      largest = std::max(largest, ++run);
    else
      run = 0;
  }
  return std::string(largest + 1, '`');
}
} // namespace
CanonicalDocument markdown_document(std::string_view input, const SourceMetadata &source,
                                    const WebLimits &limits) {
  if (input.size() > limits.max_document_chars)
    throw Error(ErrorCode::ResponseTooLarge, "Markdown document limit");
  MarkdownParser parser(source, limits);
  MD_PARSER callbacks{};
  callbacks.flags = 0x100 | 0x200 | 0x800; // tables, strikethrough, task lists
  callbacks.enter_block = +[](MD_BLOCKTYPE t, void *d, void *p) {
    auto &s = *static_cast<MarkdownParser *>(p);
    return s.safe([&] { s.enter(t, d); });
  };
  callbacks.leave_block = +[](MD_BLOCKTYPE t, void *, void *p) {
    auto &s = *static_cast<MarkdownParser *>(p);
    return s.safe([&] { s.leave(t); });
  };
  callbacks.enter_span = +[](MD_SPANTYPE t, void *d, void *p) {
    auto &s = *static_cast<MarkdownParser *>(p);
    return s.safe([&] {
      if (t == MD_SPAN_CODE && !s.stack.empty())
        s.inline_code.push_back(s.stack.back().text.size());
      if (t == MD_SPAN_A) {
        auto *a = static_cast<MD_SPAN_A_DETAIL *>(d);
        std::string url;
        try {
          url =
              clean_url(std::string_view(a->href.text, a->href.size), s.document.source.final_url);
        } catch (const std::exception &) {
        }
        s.links.push_back(url);
        if (!s.stack.empty() && !url.empty())
          s.stack.back().text += '[';
      }
    });
  };
  callbacks.leave_span = +[](MD_SPANTYPE t, void *, void *p) {
    auto &s = *static_cast<MarkdownParser *>(p);
    return s.safe([&] {
      if (t == MD_SPAN_CODE && !s.inline_code.empty() && !s.stack.empty()) {
        auto at = s.inline_code.back();
        s.inline_code.pop_back();
        auto &text = s.stack.back().text;
        auto code = text.substr(at);
        size_t run = 0, longest = 0;
        for (char c : code) {
          if (c == '`')
            longest = std::max(longest, ++run);
          else
            run = 0;
        }
        auto mark = std::string(longest + 1, '`');
        text.resize(at);
        text += mark + ' ' + code + ' ' + mark;
      }
      if (t == MD_SPAN_A && !s.links.empty()) {
        auto url = std::move(s.links.back());
        s.links.pop_back();
        if (!s.stack.empty() && !url.empty())
          s.stack.back().text += "](" + url + ")";
      }
    });
  };
  callbacks.text = +[](MD_TEXTTYPE t, const char *text, unsigned n, void *p) {
    auto &s = *static_cast<MarkdownParser *>(p);
    return s.safe([&] {
      if (s.raw_html || t == MD_TEXT_HTML || s.stack.empty())
        return;
      if (t == MD_TEXT_BR || t == MD_TEXT_SOFTBR)
        s.stack.back().text += '\n';
      else if (t == MD_TEXT_ENTITY)
        s.stack.back().text += markdown_entity(std::string_view(text, n));
      else
        s.stack.back().text.append(text, n);
    });
  };
  int status = md_parse(input.data(), static_cast<unsigned>(input.size()), &callbacks, &parser);
  if (parser.error)
    std::rethrow_exception(parser.error);
  if (status)
    throw Error(ErrorCode::ExtractionFailure, "Markdown parser failed");
  for (auto &b : parser.document.blocks)
    if (b.type == BlockType::Heading) {
      parser.document.metadata.title = b.text;
      break;
    }
  return std::move(parser.document);
}
void sanitize(CanonicalDocument &document, const WebLimits &limits) {
  for (auto *text :
       {&document.metadata.title, &document.metadata.author, &document.metadata.language,
        &document.metadata.description, &document.metadata.published, &document.metadata.modified})
    *text = sanitize_text(*text);
  document.metadata.title = utf8_excerpt(document.metadata.title, 512);
  document.metadata.author = utf8_excerpt(document.metadata.author, 256);
  document.metadata.language = utf8_excerpt(document.metadata.language, 32);
  document.metadata.description = utf8_excerpt(document.metadata.description, 2048);
  for (auto &[key, value] : document.metadata.fields)
    value = utf8_excerpt(sanitize_text(value), 512);
  size_t chars = 0, count = 0;
  std::function<void(std::vector<Block> &, unsigned)> blocks = [&](auto &values, unsigned depth) {
    if (depth > limits.max_dom_depth)
      throw Error(ErrorCode::ResponseTooLarge, "Canonical nesting limit");
    for (auto it = values.begin(); it != values.end();) {
      if (++count > limits.max_blocks || chars >= limits.max_document_chars) {
        values.erase(it, values.end());
        document.truncated = true;
        break;
      }
      auto &b = *it;
      bool verbatim = b.type == BlockType::Code || b.type == BlockType::Diff;
      b.text = sanitize_text(b.text, verbatim);
      b.author = sanitize_text(b.author);
      b.timestamp = sanitize_text(b.timestamp);
      b.language = sanitize_text(b.language);
      b.path = sanitize_text(b.path);
      auto bound = verbatim ? limits.max_code_block_bytes : limits.max_document_chars;
      if (b.text.size() > bound || chars + b.text.size() > limits.max_document_chars) {
        // Keep complete source lines, never half a token or a code line.
        auto end = b.text.rfind('\n', std::min(bound, limits.max_document_chars - chars));
        if (end == std::string::npos)
          b.text.clear();
        else
          b.text.resize(end + 1);
        document.truncated = true;
      }
      chars += b.text.size();
      blocks(b.children, depth + 1);
      if (b.rows.size() > limits.max_table_rows) {
        b.rows.resize(limits.max_table_rows);
        document.truncated = true;
      }
      for (auto &row : b.rows) {
        if (row.size() > limits.max_table_columns) {
          row.resize(limits.max_table_columns);
          document.truncated = true;
        }
        for (auto &cell : row) {
          cell = sanitize_text(cell);
          if (cell.size() > limits.max_cell_size) {
            cell = "[oversized cell omitted]";
            document.truncated = true;
          }
          if (chars + cell.size() > limits.max_document_chars) {
            cell.clear();
            document.truncated = true;
          }
          chars += cell.size();
        }
      }
      if (b.text.empty() && b.children.empty() && b.rows.empty())
        it = values.erase(it);
      else
        ++it;
    }
  };
  blocks(document.blocks, 0);
}
void deduplicate(CanonicalDocument &d) {
  std::unordered_set<std::string> seen;
  d.blocks.erase(std::remove_if(d.blocks.begin(), d.blocks.end(),
                                [&](auto &b) {
                                  if (b.type == BlockType::Heading || b.type == BlockType::Code ||
                                      b.type == BlockType::Diff || b.type == BlockType::ForumPost)
                                    return false;
                                  auto key = std::to_string(static_cast<int>(b.type)) + ":" +
                                             digest(render_block(b));
                                  return !seen.insert(key).second;
                                }),
                 d.blocks.end());
}
std::string render_block(const Block &b) {
  switch (b.type) {
  case BlockType::Heading:
    return std::string(std::clamp(b.level, 1U, 6U), '#') + " " + b.text + "\n";
  case BlockType::Code:
  case BlockType::Diff: {
    auto mark = fence(b.text);
    return (b.path.empty() ? std::string() : "### " + b.path + "\n") + mark +
           (b.type == BlockType::Diff ? "diff" : b.language) + "\n" + b.text +
           (b.text.ends_with('\n') ? "" : "\n") + mark + "\n";
  }
  case BlockType::ListItem: {
    auto out = std::string(b.indent * 2, ' ') + "- " + b.text + "\n";
    for (auto &child : b.children)
      out += render_block(child);
    return out;
  }
  case BlockType::Quote: {
    std::string body = b.text;
    for (auto &child : b.children)
      body += render_block(child);
    std::istringstream input(body);
    std::string line, out;
    while (std::getline(input, line))
      out += "> " + line + '\n';
    return out;
  }
  case BlockType::ForumPost: {
    std::string out = "## " + (b.author.empty() ? "Post" : b.author);
    if (b.post_number)
      out += " #" + std::to_string(*b.post_number);
    if (!b.timestamp.empty())
      out += " · " + b.timestamp;
    if (b.reply_to)
      out += " (reply to #" + std::to_string(*b.reply_to) + ")";
    out += "\n";
    for (auto &child : b.children)
      out += render_block(child) + "\n";
    return out;
  }
  case BlockType::Table: {
    std::string out;
    for (size_t i = 0; i < b.rows.size(); ++i) {
      auto &row = b.rows[i];
      out += '|';
      for (auto cell : row) {
        for (auto &c : cell)
          if (c == '\n')
            c = ' ';
        size_t at = 0;
        while ((at = cell.find('|', at)) != std::string::npos) {
          cell.insert(at, "\\");
          at += 2;
        }
        out += ' ' + cell + " |";
      }
      out += '\n';
      if (i == 0) {
        out += '|';
        for (size_t j = 0; j < row.size(); ++j)
          out += " --- |";
        out += '\n';
      }
    }
    return out;
  }
  default:
    return b.text + "\n";
  }
}
std::string render(const CanonicalDocument &d, size_t max_tokens) {
  std::string out = "UNTRUSTED EXTERNAL CONTENT\nsource: " + platform_name(d.source.platform) +
                    "\ntype: " + resource_name(d.source.resource_type) +
                    "\nurl: " + d.source.final_url + "\n";
  if (!d.source.repository.empty())
    out += "repo: " + d.source.repository + "\n";
  for (auto [key, value] :
       std::array<std::pair<std::string, std::string>, 5>{{{"title", d.metadata.title},
                                                           {"author", d.metadata.author},
                                                           {"published", d.metadata.published},
                                                           {"modified", d.metadata.modified},
                                                           {"language", d.metadata.language}}})
    if (!value.empty())
      out += key + ": " + value + "\n";
  for (auto &[key, value] : d.metadata.fields)
    if (!value.empty())
      out += key + ": " + value + "\n";
  out += '\n';
  if (max_tokens && estimate_tokens(out) > max_tokens)
    throw Error(ErrorCode::ResponseTooLarge, "Budget too small for document provenance");
  bool omitted = d.truncated;
  for (auto &block : d.blocks) {
    auto part = render_block(block) + "\n";
    if (max_tokens &&
        estimate_tokens(out + part + "[Additional sections omitted]\n") > max_tokens) {
      omitted = true;
      continue;
    }
    out += part;
  }
  if (omitted &&
      (!max_tokens || estimate_tokens(out + "[Additional sections omitted]\n") <= max_tokens))
    out += "[Additional sections omitted]\n";
  return out;
}
Json block_json(const Block &b) {
  Json children = Json::array();
  for (auto &child : b.children)
    children.push_back(block_json(child));
  return {{"id", b.id},
          {"type", static_cast<int>(b.type)},
          {"text", b.text},
          {"language", b.language},
          {"author", b.author},
          {"timestamp", b.timestamp},
          {"path", b.path},
          {"level", b.level},
          {"indent", b.indent},
          {"post_number", b.post_number ? Json(*b.post_number) : Json()},
          {"reply_to", b.reply_to ? Json(*b.reply_to) : Json()},
          {"children", children},
          {"rows", b.rows}};
}
Block block_from_json(const Json &j, unsigned depth) {
  if (depth > 128)
    throw Error(ErrorCode::ResponseTooLarge, "Cached nesting limit");
  Block b;
  b.id = j.value("id", size_t(0));
  b.type = static_cast<BlockType>(j.at("type").get<int>());
  b.text = j.value("text", "");
  b.language = j.value("language", "");
  b.author = j.value("author", "");
  b.timestamp = j.value("timestamp", "");
  b.path = j.value("path", "");
  b.level = j.value("level", 0U);
  b.indent = j.value("indent", 0U);
  if (!j.value("post_number", Json()).is_null())
    b.post_number = j["post_number"];
  if (!j.value("reply_to", Json()).is_null())
    b.reply_to = j["reply_to"];
  b.rows = j.value("rows", std::vector<std::vector<std::string>>{});
  for (auto &v : j.value("children", Json::array()))
    b.children.push_back(block_from_json(v, depth + 1));
  return b;
}
Json document_json(const CanonicalDocument &d) {
  Json blocks = Json::array();
  for (auto &b : d.blocks)
    blocks.push_back(block_json(b));
  auto &s = d.source;
  auto &m = d.metadata;
  return {{"source",
           {{"requested_url", s.requested_url},
            {"final_url", s.final_url},
            {"canonical_url", s.canonical_url},
            {"repository", s.repository},
            {"path", s.acquisition_path},
            {"platform", static_cast<int>(s.platform)},
            {"type", static_cast<int>(s.resource_type)},
            {"fallbacks", s.fallbacks}}},
          {"metadata",
           {{"title", m.title},
            {"author", m.author},
            {"language", m.language},
            {"description", m.description},
            {"published", m.published},
            {"modified", m.modified},
            {"fields", m.fields}}},
          {"blocks", blocks},
          {"truncated", d.truncated}};
}
CanonicalDocument document_from_json(const Json &j) {
  CanonicalDocument d;
  auto &s = j.at("source");
  auto &m = j.at("metadata");
  d.source = {s.value("requested_url", ""),
              s.value("final_url", ""),
              s.value("canonical_url", ""),
              s.value("repository", ""),
              s.value("path", ""),
              static_cast<Platform>(s.value("platform", 0)),
              static_cast<ResourceType>(s.value("type", 0)),
              s.value("fallbacks", std::vector<std::string>{}),
              true};
  d.metadata = {m.value("title", ""),
                m.value("author", ""),
                m.value("language", ""),
                m.value("published", ""),
                m.value("modified", ""),
                m.value("description", ""),
                m.value("fields", std::map<std::string, std::string>{})};
  for (auto &b : j.at("blocks"))
    d.blocks.push_back(block_from_json(b));
  d.truncated = j.value("truncated", false);
  return d;
}
} // namespace saga::web
