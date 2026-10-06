#include "../../src/web/internal.hpp"
#include <iostream>
#include <random>
#include <saga/web/acquisition.hpp>
using namespace saga;
using namespace saga::web;
int main() {
  std::mt19937 random(123456);
  std::vector<std::string> seeds = {"<main><h1>Test</h1><p>Useful text</p></main>",
                                    "# Markdown\n\n```c\n  int x;\n```\n",
                                    "https://example.org/a?x=1", "std::vector iwn(4) --no-mmap"};
  for (int i = 0; i < 500; ++i) {
    auto input = seeds[static_cast<size_t>(i) % seeds.size()];
    for (int j = 0; j < 8; ++j) {
      auto at = static_cast<size_t>(random()) % (input.size() + 1);
      input.insert(at, 1, static_cast<char>(random() % 256));
    }
    try {
      SourceMetadata source;
      source.final_url = "https://example.org/";
      auto analysis = analyze_html(input, source, {});
      ExtractionInfo info;
      auto document = extract_regions(std::move(analysis), info, {});
      sanitize(document);
      render(document, 1024);
    } catch (const std::exception &) {
    }
    try {
      auto document = markdown_document(input, {}, {});
      sanitize(document);
      render(document, 1024);
    } catch (const std::exception &) {
    }
    try {
      web_url(input);
    } catch (const std::exception &) {
    }
    technical_tokens(input);
    sanitize_text(input);
  }
  std::cout << "500 deterministic parser/Markdown/URL/tokenizer mutations passed\n";
}
