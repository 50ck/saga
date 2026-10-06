#pragma once
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/text.h>
#include <lexbor/html/parser.h>
#include <saga/web/acquisition.hpp>
namespace saga::web {
using Node = lxb_dom_node_t;
struct HtmlDocument {
  lxb_html_document_t *value = nullptr;
  HtmlDocument(std::string_view, const WebLimits &);
  ~HtmlDocument();
  HtmlDocument(const HtmlDocument &) = delete;
  Node *root() const {
    return lxb_dom_interface_node(value);
  }
};
std::string tag(Node *);
std::string attr(Node *, std::string_view);
std::string text(Node *);
bool hard_hidden(Node *);
void walk(Node *, const std::function<void(Node *, size_t)> &, const WebLimits &);
struct HtmlAnalysis {
  CanonicalDocument document;
  Json signals = Json::object();
  size_t visible_chars = 0, scripts = 0;
};
HtmlAnalysis analyze_html(std::string_view, const SourceMetadata &, const WebLimits &);
CanonicalDocument extract_regions(HtmlAnalysis, ExtractionInfo &, const WebLimits &,
                                  const std::function<double(const Block &)> &template_score = {});
InstanceDescriptor passive_instance(std::string_view, const Json &);
std::string decoded(std::string_view);
Json block_json(const Block &);
Block block_from_json(const Json &, unsigned depth = 0);
} // namespace saga::web
