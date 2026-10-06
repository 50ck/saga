#include "internal.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace saga::web {
HtmlDocument::HtmlDocument(std::string_view input, const WebLimits &limits) {
  if (input.size() > limits.max_response_bytes)
    throw Error(ErrorCode::ResponseTooLarge, "HTML response limit");
  value = lxb_html_document_create();
  if (!value)
    throw std::bad_alloc();
  if (lxb_html_document_parse(value, reinterpret_cast<const lxb_char_t *>(input.data()),
                              input.size()) != LXB_STATUS_OK) {
    lxb_html_document_destroy(value);
    value = nullptr;
    throw Error(ErrorCode::ExtractionFailure, "Lexbor HTML parsing failed");
  }
}
HtmlDocument::~HtmlDocument() {
  if (value)
    lxb_html_document_destroy(value);
}
std::string tag(Node *n) {
  if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT)
    return {};
  size_t size = 0;
  auto *value = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &size);
  return value ? std::string(reinterpret_cast<const char *>(value), size) : "";
}
std::string attr(Node *n, std::string_view key) {
  if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT)
    return {};
  size_t size = 0;
  auto *value = lxb_dom_element_get_attribute(lxb_dom_interface_element(n),
                                              reinterpret_cast<const lxb_char_t *>(key.data()),
                                              key.size(), &size);
  return value ? std::string(reinterpret_cast<const char *>(value), size) : "";
}
std::string text(Node *n) {
  if (!n || n->type != LXB_DOM_NODE_TYPE_TEXT)
    return {};
  auto &s = lxb_dom_interface_text(n)->char_data.data;
  return std::string(reinterpret_cast<const char *>(s.data), s.length);
}
bool hard_hidden(Node *n) {
  auto name = tag(n);
  if (name == "script" || name == "style" || name == "template" || name == "canvas" ||
      name == "input" || name == "button" || name == "select" || name == "textarea" ||
      name == "noscript")
    return true;
  if (n->type != LXB_DOM_NODE_TYPE_ELEMENT)
    return false;
  auto *e = lxb_dom_interface_element(n);
  if (lxb_dom_element_has_attribute(e, reinterpret_cast<const lxb_char_t *>("hidden"), 6) ||
      lxb_dom_element_has_attribute(e, reinterpret_cast<const lxb_char_t *>("inert"), 5) ||
      lower(attr(n, "aria-hidden")) == "true")
    return true;
  auto style = lower(attr(n, "style"));
  style.erase(
      std::remove_if(style.begin(), style.end(), [](unsigned char c) { return std::isspace(c); }),
      style.end());
  return style.find("display:none") != std::string::npos ||
         style.find("visibility:hidden") != std::string::npos;
}
void walk(Node *root, const std::function<void(Node *, size_t)> &callback,
          const WebLimits &limits) {
  std::vector<std::pair<Node *, size_t>> stack{{root, 0}};
  size_t count = 0;
  while (!stack.empty()) {
    auto [node, depth] = stack.back();
    stack.pop_back();
    if (++count > limits.max_dom_nodes || depth > limits.max_dom_depth)
      throw Error(ErrorCode::ResponseTooLarge, "HTML DOM complexity limit");
    callback(node, depth);
    for (auto *child = node->last_child; child; child = child->prev)
      stack.emplace_back(child, depth + 1);
  }
}
namespace {
bool contains_any(std::string_view s, std::initializer_list<std::string_view> values) {
  for (auto word : values)
    if (s.find(word) != std::string_view::npos)
      return true;
  return false;
}
bool boundary(const std::string &t) {
  return t == "p" || t == "pre" || t == "ul" || t == "ol" || t == "li" || t == "table" ||
         t == "blockquote" || t == "dl" || t == "dt" || t == "dd" ||
         (t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6');
}
std::string collect(Node *root, const std::string &base, bool code, Block *features,
                    const WebLimits &limits, bool omit_nested_lists = false) {
  std::string out;
  std::function<void(Node *, size_t)> visit = [&](Node *n, size_t depth) {
    if (depth > limits.max_dom_depth)
      throw Error(ErrorCode::ResponseTooLarge, "HTML text depth limit");
    if (hard_hidden(n))
      return;
    auto t = tag(n);
    if (omit_nested_lists && n != root && (t == "ul" || t == "ol"))
      return;
    if (features)
      ++features->dom_complexity;
    if (n->type == LXB_DOM_NODE_TYPE_TEXT) {
      auto value = text(n);
      out += value;
      if (features)
        features->character_count += value.size();
      for (auto *p = n->parent; p && p != root->parent; p = p->parent)
        if (tag(p) == "a") {
          if (features)
            features->link_chars += value.size();
          break;
        }
      return;
    }
    if (t == "br") {
      out += '\n';
      return;
    }
    if (t == "img") {
      auto alt = attr(n, "alt");
      if (!alt.empty())
        out += alt;
      return;
    }
    std::string href;
    if (t == "a" && !code) {
      try {
        href = clean_url(attr(n, "href"), base);
      } catch (const std::exception &) {
      }
      if (!href.empty()) {
        out += '[';
        if (features)
          ++features->link_count;
      }
    }
    if (t == "code" && !code)
      out += '`';
    for (auto *child = n->first_child; child; child = child->next)
      visit(child, depth + 1);
    if (t == "code" && !code)
      out += '`';
    if (!href.empty())
      out += "](" + href + ")";
    if (!code && n != root && boundary(t))
      out += '\n';
    if (out.size() > limits.max_document_chars * 2)
      throw Error(ErrorCode::ResponseTooLarge, "HTML text expansion limit");
  };
  visit(root, 0);
  return code ? out : trim(out);
}
void semantics(Block &b, Node *n) {
  for (unsigned depth = 0; n && depth < 128; n = n->parent, ++depth) {
    auto t = tag(n);
    auto role = lower(attr(n, "role"));
    b.inside_main |= t == "main" || role == "main" || attr(n, "itemprop") == "articleBody";
    b.inside_article |= t == "article";
    b.inside_nav |= t == "nav" || role == "navigation";
    b.inside_aside |= t == "aside";
    b.inside_header |= t == "header";
    b.inside_footer |= t == "footer";
    if (depth < 4)
      b.hints += ' ' + lower(attr(n, "class") + ' ' + attr(n, "id"));
    if (depth < 5)
      b.structural_signature = t + "/" + b.structural_signature;
  }
}
void json_metadata(const Json &value, DocumentMetadata &metadata, Json &signals,
                   unsigned depth = 0) {
  if (depth > 32)
    return;
  if (value.is_array()) {
    for (auto &item : value)
      json_metadata(item, metadata, signals, depth + 1);
    return;
  }
  if (!value.is_object())
    return;
  if (value.contains("@graph"))
    json_metadata(value["@graph"], metadata, signals, depth + 1);
  auto type = value.value("@type", Json());
  std::string types = type.is_string() ? type.get<std::string>() : type.dump();
  signals["schema_types"] = signals.value("schema_types", std::string()) + ' ' + types;
  if (contains_any(types, {"Article", "BlogPosting", "DiscussionForumPosting", "QAPage", "FAQPage",
                           "HowTo", "SoftwareApplication", "Product"})) {
    auto string = [&](const char *key) {
      return value.contains(key) && value[key].is_string() ? value[key].get<std::string>()
                                                           : std::string();
    };
    if (metadata.title.empty())
      metadata.title = string("headline");
    if (metadata.description.empty())
      metadata.description = string("description");
    if (metadata.published.empty())
      metadata.published = string("datePublished");
    if (metadata.modified.empty())
      metadata.modified = string("dateModified");
    auto author = value.value("author", Json());
    if (metadata.author.empty()) {
      if (author.is_string())
        metadata.author = author;
      else if (author.is_object() && author.value("name", Json()).is_string())
        metadata.author = author["name"];
    }
  }
}
} // namespace
HtmlAnalysis analyze_html(std::string_view input, const SourceMetadata &source,
                          const WebLimits &limits) {
  HtmlDocument dom(input, limits);
  HtmlAnalysis analysis;
  analysis.document.source = source;
  auto &m = analysis.document.metadata;
  std::string dom_title, og_title, h1;
  Json signals = {{"assets", Json::array()},
                  {"bases", Json::array()},
                  {"generator", ""},
                  {"footer", ""},
                  {"schema_types", ""}};
  walk(
      dom.root(),
      [&](Node *n, size_t) {
        auto t = tag(n);
        if (t == "html")
          m.language = attr(n, "lang");
        if (t == "title")
          dom_title = collect(n, source.final_url, false, nullptr, limits);
        if (t == "h1" && h1.empty())
          h1 = collect(n, source.final_url, false, nullptr, limits);
        if (t == "base" && signals["bases"].size() < 4)
          signals["bases"].push_back(attr(n, "href"));
        if (t == "meta") {
          auto key = lower(attr(n, "name"));
          if (key.empty())
            key = lower(attr(n, "property"));
          auto value = attr(n, "content");
          if (key == "generator")
            signals["generator"] = value;
          else if (key == "og:title" || key == "twitter:title")
            og_title = value;
          else if (key == "author")
            m.author = value;
          else if (key == "description" || key == "og:description")
            m.description = value;
          else if (key == "article:published_time")
            m.published = value;
          else if (key == "article:modified_time")
            m.modified = value;
        }
        if (t == "link" && lower(attr(n, "rel")) == "canonical") {
          try {
            analysis.document.source.canonical_url = clean_url(attr(n, "href"), source.final_url);
          } catch (const std::exception &) {
          }
        }
        if ((t == "script" || t == "link") && signals["assets"].size() < 64) {
          auto url = attr(n, t == "script" ? "src" : "href");
          if (!url.empty())
            signals["assets"].push_back(url);
        }
        if (t == "script") {
          ++analysis.scripts;
          std::string body;
          for (auto *c = n->first_child; c; c = c->next)
            body += text(c);
          if (body.find("_cf_chl_opt") != std::string::npos)
            signals["challenge"] = true;
          if (lower(attr(n, "type")) == "application/ld+json") {
            if (body.size() < 128 * 1024)
              try {
                auto value = Json::parse(body, [](int depth, Json::parse_event_t, Json &) {
                  if (depth > 64)
                    throw Error(ErrorCode::ResponseTooLarge, "JSON-LD nesting limit");
                  return true;
                });
                json_metadata(value, m, signals);
              } catch (const std::exception &) {
              }
          }
        }
        if (t == "footer" && signals["footer"].get_ref<const std::string &>().size() < 2048)
          signals["footer"] = collect(n, source.final_url, false, nullptr, limits);
        if (!attr(n, "class").empty() && signals.value("classes", std::string()).size() < 8192)
          signals["classes"] = signals.value("classes", std::string()) + ' ' + attr(n, "class");
      },
      limits);
  m.title = !h1.empty()          ? h1
            : !og_title.empty()  ? og_title
            : !dom_title.empty() ? dom_title
                                 : m.title;
  std::unordered_set<Node *> consumed;
  walk(
      dom.root(),
      [&](Node *n, size_t) {
        if (consumed.contains(n))
          return;
        bool hidden = false;
        for (auto *p = n; p; p = p->parent)
          if (hard_hidden(p) || tag(p) == "head") {
            hidden = true;
            break;
          }
        if (hidden)
          return;
        auto t = tag(n);
        if (t.empty())
          return;
        Block b;
        bool candidate = false;
        if (t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6') {
          candidate = true;
          b.type = BlockType::Heading;
          b.level = static_cast<unsigned>(t[1] - '0');
        } else if (t == "p") {
          candidate = true;
          b.type = BlockType::Paragraph;
        } else if (t == "pre") {
          candidate = true;
          b.type = BlockType::Code;
          auto hints = attr(n, "class");
          for (auto *c = n->first_child; c; c = c->next)
            if (tag(c) == "code")
              hints += ' ' + attr(c, "class");
          auto at = hints.find("language-");
          if (at != std::string::npos)
            b.language = hints.substr(at + 9, hints.find(' ', at + 9) - at - 9);
        } else if (t == "li") {
          candidate = true;
          b.type = BlockType::ListItem;
          for (auto *p = n->parent; p; p = p->parent)
            if (tag(p) == "ul" || tag(p) == "ol")
              ++b.indent;
          if (b.indent)
            --b.indent;
        } else if (t == "dt" || t == "dd") {
          candidate = true;
          b.type = BlockType::Definition;
        } else if (t == "blockquote") {
          candidate = true;
          b.type = BlockType::Quote;
        } else if (t == "figcaption") {
          candidate = true;
          b.type = BlockType::FigureCaption;
        } else if (t == "a" && (tag(n->parent) == "main" || tag(n->parent) == "article" ||
                                tag(n->parent) == "section" || tag(n->parent) == "div")) {
          candidate = true;
          b.type = BlockType::Paragraph;
        } else if (t == "table") {
          candidate = true;
          b.type = BlockType::Table;
          walk(
              n,
              [&](Node *row, size_t) {
                if (tag(row) != "tr")
                  return;
                if (b.rows.size() >= limits.max_table_rows) {
                  analysis.document.truncated = true;
                  return;
                }
                std::vector<std::string> cells;
                for (auto *c = row->first_child; c; c = c->next)
                  if (tag(c) == "td" || tag(c) == "th") {
                    if (cells.size() < limits.max_table_columns)
                      cells.push_back(collect(c, source.final_url, false, &b, limits));
                    else
                      analysis.document.truncated = true;
                  }
                if (!cells.empty())
                  b.rows.push_back(std::move(cells));
              },
              limits);
        } else if (t == "div" || t == "section" || t == "article" || t == "main" || t == "body") {
          bool has_boundary = false;
          walk(
              n,
              [&](Node *child, size_t) {
                if (child != n &&
                    (boundary(tag(child)) || tag(child) == "div" || tag(child) == "section"))
                  has_boundary = true;
              },
              limits);
          if (!has_boundary) {
            candidate = true;
            b.type = BlockType::Paragraph;
          }
        }
        if (!candidate)
          return;
        semantics(b, n);
        b.id = analysis.document.blocks.size();
        if (b.type != BlockType::Table)
          b.text = collect(n, source.final_url, b.type == BlockType::Code, &b, limits, t == "li");
        if (!b.text.empty() || !b.rows.empty()) {
          if (analysis.document.blocks.size() >= limits.max_blocks)
            throw Error(ErrorCode::ResponseTooLarge, "HTML block limit");
          analysis.visible_chars += b.text.size();
          for (auto &row : b.rows)
            for (auto &cell : row)
              analysis.visible_chars += cell.size();
          analysis.document.blocks.push_back(std::move(b));
        }
        walk(
            n,
            [&](Node *child, size_t) {
              if (child == n)
                return;
              if (t == "li") {
                for (auto *p = child; p && p != n; p = p->parent)
                  if (tag(p) == "ul" || tag(p) == "ol")
                    return;
              }
              consumed.insert(child);
            },
            limits);
      },
      limits);
  analysis.signals = std::move(signals);
  return analysis;
}
ExtractionWeights extraction_weights(PageProfile p) {
  switch (p) {
  case PageProfile::Documentation:
    return {0.45, 0.8, 1.8, 1, 2};
  case PageProfile::Discussion:
    return {0.8, 0.8, 1.4, 1, 1.8};
  case PageProfile::Reference:
    return {0.3, 0.8, 1.8, 0.9, 2};
  case PageProfile::Listing:
    return {0.4, 0.3, 1.5, 0.4, 2};
  default:
    return {};
  }
}
CanonicalDocument extract_regions(HtmlAnalysis analysis, ExtractionInfo &info,
                                  const WebLimits &limits,
                                  const std::function<double(const Block &)> &template_score) {
  auto &blocks = analysis.document.blocks;
  info.blocks_before = blocks.size();
  if (analysis.signals.value("challenge", false))
    throw Error(ErrorCode::DynamicContentUnavailable, "Source returned a browser challenge");
  if (analysis.visible_chars < 200)
    for (auto &b : blocks)
      if (lower(b.text).find("enable javascript") != std::string::npos)
        throw Error(ErrorCode::DynamicContentUnavailable, "Page requires JavaScript for content");
  size_t headings = 0, codes = 0, tables = 0, definitions = 0, long_paragraphs = 0, links = 0;
  double link_chars = 0, chars = 0;
  for (auto &b : blocks) {
    headings += b.type == BlockType::Heading;
    codes += b.type == BlockType::Code;
    tables += b.type == BlockType::Table;
    definitions += b.type == BlockType::Definition;
    long_paragraphs += b.text.size() > 200;
    links += b.link_count;
    chars += b.character_count ? b.character_count : b.text.size();
    link_chars += b.link_chars;
  }
  auto schema = analysis.signals.value("schema_types", std::string());
  auto classes = lower(analysis.signals.value("classes", std::string()));
  if (contains_any(schema, {"DiscussionForumPosting", "QAPage"}) ||
      contains_any(classes, {"post-body", "message-body", "forum-post"}))
    info.profile = PageProfile::Discussion;
  else if ((tables >= 2 || definitions >= 3) && long_paragraphs < 3)
    info.profile = PageProfile::Reference;
  else if (codes || (headings >= 3 && links >= 3) ||
           contains_any(classes, {"documentation", "docs-content"}))
    info.profile = PageProfile::Documentation;
  else if (chars && link_chars / chars > 0.5)
    info.profile = PageProfile::Listing;
  else if (long_paragraphs || contains_any(schema, {"Article", "BlogPosting"}))
    info.profile = PageProfile::Prose;
  auto weights = extraction_weights(info.profile);
  for (auto &b : blocks) {
    auto size = b.character_count ? b.character_count : b.text.size();
    auto words = technical_tokens(b.text).size();
    auto punctuation = std::count_if(b.text.begin(), b.text.end(), [](char c) {
      return c == ',' || c == '.' || c == ';' || c == '!' || c == '?';
    });
    double density = size ? std::min(1.0, b.link_chars / static_cast<double>(size)) : 0;
    b.readability = std::min(4.0, size / 140.0) + std::min(2.0, punctuation / 4.0) - 3 * density;
    b.density =
        std::min(3.0, words / static_cast<double>(std::max<size_t>(1, b.dom_complexity)) * 0.7) -
        2 * density;
    b.semantic =
        (b.inside_main ? 3 : 0) + (b.inside_article ? 2 : 0) +
        (b.type == BlockType::Heading ? 1.5 : 0) +
        (b.type == BlockType::Code || b.type == BlockType::Table || b.type == BlockType::Definition
             ? 2.2
             : 0);
    if (contains_any(b.hints, {"article-body", "entry-content", "markdown-body", "content-body",
                               "documentation", "warning", "admonition", "notice", "message-body",
                               "post-body"}))
      b.semantic += 3;
    if (b.inside_nav)
      b.semantic -= 3;
    if (b.inside_header || b.inside_footer)
      b.semantic -= 2;
    if (b.inside_aside && !contains_any(b.hints, {"warning", "notice", "admonition"}))
      b.semantic -= 1;
    if (contains_any(b.hints, {"cookie", "advert", "newsletter", "subscribe", "share-button",
                               "social-links", "related-post", "sidebar", "pagination", "login"}))
      b.semantic -= 6;
    b.template_penalty = template_score ? template_score(b) : 0;
  }
  for (size_t i = 0; i < blocks.size(); ++i) {
    auto &b = blocks[i];
    double neighbors = 0;
    if (i && blocks[i - 1].semantic > 1)
      neighbors += 0.6;
    if (i + 1 < blocks.size() && blocks[i + 1].semantic > 1)
      neighbors += 0.6;
    b.continuity = neighbors;
    b.score = weights.readability * b.readability + weights.density * b.density +
              weights.semantic * b.semantic + weights.continuity * b.continuity -
              weights.template_penalty * b.template_penalty;
    info.scores.push_back(b.score);
  }
  auto run = [&](ExtractionMode mode) {
    double threshold = mode == ExtractionMode::Strict     ? 4.5
                       : mode == ExtractionMode::Balanced ? 1.0
                                                          : -0.8;
    std::vector<bool> keep(blocks.size(), false);
    for (size_t i = 0; i < blocks.size(); ++i)
      keep[i] = blocks[i].score >= threshold;
    // Join strong sibling regions while preserving section headings and useful intermediates.
    for (size_t i = 0; i < blocks.size(); ++i)
      if (keep[i]) {
        for (size_t j = i; j > 0;) {
          --j;
          if (blocks[j].type == BlockType::Heading) {
            if (blocks[j].semantic >= 0)
              keep[j] = true;
            break;
          }
          if (i - j > 12)
            break;
        }
        if (i + 1 < blocks.size() && blocks[i + 1].semantic >= 0 &&
            blocks[i + 1].score > threshold - 2)
          keep[i + 1] = true;
      }
    CanonicalDocument result = analysis.document;
    result.blocks.clear();
    size_t extracted = 0, linked = 0, boilerplate = 0;
    for (size_t i = 0; i < blocks.size(); ++i)
      if (keep[i]) {
        result.blocks.push_back(blocks[i]);
        extracted += render_block(blocks[i]).size();
        linked += blocks[i].link_chars;
        boilerplate += blocks[i].semantic < 0;
      }
    ExtractionQuality quality;
    quality.extracted_chars = extracted;
    quality.extracted_blocks = result.blocks.size();
    quality.source_coverage =
        analysis.visible_chars
            ? std::min(1.0, extracted / static_cast<double>(analysis.visible_chars))
            : 0;
    quality.link_density = extracted ? linked / static_cast<double>(extracted) : 0;
    quality.boilerplate_probability =
        result.blocks.empty() ? 1 : boilerplate / static_cast<double>(result.blocks.size());
    quality.likely_dynamic = analysis.scripts >= 3 && analysis.visible_chars < 120;
    quality.confidence =
        std::clamp((result.blocks.empty() ? 0.0 : 0.55) + std::min(0.25, extracted / 4000.0) -
                       quality.link_density * 0.4 - quality.boilerplate_probability * 0.25,
                   0.0, 1.0);
    info.quality = quality;
    info.mode = mode;
    return result;
  };
  auto result = run(ExtractionMode::Strict);
  if (info.quality.extracted_chars < 200 || info.quality.source_coverage < 0.15 ||
      info.quality.link_density > 0.6)
    result = run(ExtractionMode::Balanced);
  if ((info.quality.extracted_chars < 100 && info.quality.source_coverage < 0.5) ||
      (info.quality.source_coverage < 0.08 && analysis.visible_chars > 1000))
    result = run(ExtractionMode::Recall);
  if (info.quality.likely_dynamic && info.quality.extracted_chars < 120)
    throw Error(ErrorCode::DynamicContentUnavailable,
                "JavaScript shell has insufficient server-rendered content");
  if (result.blocks.empty())
    throw Error(ErrorCode::ExtractionFailure, "No useful semantic regions found");
  if (result.source.resource_type == ResourceType::Unknown)
    result.source.resource_type =
        info.profile == PageProfile::Documentation || info.profile == PageProfile::Reference
            ? ResourceType::Documentation
        : info.profile == PageProfile::Discussion ? ResourceType::ForumThread
                                                  : ResourceType::Article;
  result.source.acquisition_path = "Generic HTML";
  info.blocks_after = result.blocks.size();
  sanitize(result, limits);
  return result;
}
} // namespace saga::web
