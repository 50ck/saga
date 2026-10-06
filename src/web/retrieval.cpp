#include "internal.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace saga::web {
std::vector<std::string> technical_tokens(std::string_view input) {
  std::vector<std::string> tokens;
  std::string word;
  auto emit = [&] {
    if (word.empty())
      return;
    while (!word.empty() && (word.back() == '.' || word.back() == ':' || word.back() == '/'))
      word.pop_back();
    if (word.empty())
      return;
    tokens.push_back(lower(word));
    std::string part;
    for (unsigned char c : word) {
      if (std::isalnum(c) || c >= 128)
        part += static_cast<char>(c);
      else if (!part.empty()) {
        auto p = lower(part);
        if (p != tokens.back())
          tokens.push_back(p);
        part.clear();
      }
    }
    if (!part.empty()) {
      auto p = lower(part);
      if (p != lower(word))
        tokens.push_back(p);
    }
    word.clear();
  };
  for (size_t i = 0; i < input.size(); ++i) {
    auto c = static_cast<unsigned char>(input[i]);
    bool symbol = c == '_' || c == '-' || c == ':' || c == '/' || c == '.';
    if (std::isalnum(c) || c >= 128 || symbol ||
        (c == '(' && !word.empty() && i + 1 < input.size() &&
         std::isdigit(static_cast<unsigned char>(input[i + 1]))) ||
        (c == ')' && word.find('(') != std::string::npos)) {
      if (word.size() < 4096)
        word += static_cast<char>(c);
      else
        emit();
    } else
      emit();
  }
  emit();
  return tokens;
}
uint64_t simhash(std::string_view input) {
  std::array<int, 64> counts{};
  for (auto &token : technical_tokens(input)) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : token) {
      h ^= c;
      h *= 1099511628211ULL;
    }
    for (unsigned bit = 0; bit < 64; ++bit)
      counts[bit] += (h >> bit) & 1 ? 1 : -1;
  }
  uint64_t result = 0;
  for (unsigned bit = 0; bit < 64; ++bit)
    if (counts[bit] > 0)
      result |= uint64_t(1) << bit;
  return result;
}
namespace {
struct Unit {
  std::vector<size_t> blocks;
  std::array<std::vector<std::string>, 5> fields;
  double score = 0;
};
std::vector<Unit> units(const CanonicalDocument &d) {
  std::vector<Unit> result;
  std::array<std::string, 6> ancestors{};
  std::string headings;
  for (size_t i = 0; i < d.blocks.size();) {
    Unit unit;
    unit.fields[0] = technical_tokens(d.metadata.title);
    unit.fields[4] = technical_tokens(d.metadata.description);
    size_t end = i;
    if (d.blocks[i].type == BlockType::Heading) {
      auto level = std::clamp(d.blocks[i].level, 1U, 6U);
      ancestors[level - 1] = d.blocks[i].text;
      for (size_t l = level; l < 6; ++l)
        ancestors[l].clear();
      unit.blocks.push_back(i);
      ++end;
    }
    headings.clear();
    for (auto &heading : ancestors)
      headings += heading + ' ';
    unit.fields[1] = technical_tokens(headings);
    while (end < d.blocks.size() &&
           (end == i || (d.blocks[end].type != BlockType::Heading && unit.blocks.size() < 6))) {
      unit.blocks.push_back(end);
      auto &b = d.blocks[end];
      auto tokens = technical_tokens(render_block(b));
      auto field = b.type == BlockType::Code || b.type == BlockType::Diff ? 3U : 2U;
      unit.fields[field].insert(unit.fields[field].end(), tokens.begin(), tokens.end());
      ++end;
      if (b.type == BlockType::ForumPost)
        break;
    }
    if (end == i)
      ++end;
    result.push_back(std::move(unit));
    i = end;
  }
  return result;
}
bool token_match(std::string_view input, const std::unordered_set<std::string> &query) {
  for (auto &token : technical_tokens(input))
    if (query.contains(token))
      return true;
  return false;
}
} // namespace
std::vector<size_t> select_passages(const CanonicalDocument &d, std::string_view query,
                                    size_t tokens, const RetrievalWeights &weights) {
  auto indexed = units(d);
  if (indexed.empty())
    return {};
  auto terms = technical_tokens(query);
  std::sort(terms.begin(), terms.end());
  terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
  std::array<double, 5> average{};
  for (auto &u : indexed)
    for (size_t f = 0; f < 5; ++f)
      average[f] += u.fields[f].size() / static_cast<double>(indexed.size());
  std::array<double, 5> boosts = {weights.title, weights.heading, weights.body, weights.code,
                                  weights.metadata};
  for (auto &term : terms) {
    size_t docs = 0;
    for (auto &u : indexed)
      if (std::any_of(u.fields.begin(), u.fields.end(), [&](auto &field) {
            return std::find(field.begin(), field.end(), term) != field.end();
          }))
        ++docs;
    double idf = std::log(1 + (indexed.size() - docs + 0.5) / (docs + 0.5));
    for (auto &u : indexed) {
      double tf = 0;
      for (size_t f = 0; f < 5; ++f) {
        double count = std::count(u.fields[f].begin(), u.fields[f].end(), term);
        double norm = 1 - weights.b + weights.b * u.fields[f].size() / std::max(1.0, average[f]);
        tf += boosts[f] * count / norm;
      }
      u.score += idf * tf * (weights.k1 + 1) / (tf + weights.k1);
    }
  }
  std::vector<size_t> ranking(indexed.size());
  std::iota(ranking.begin(), ranking.end(), 0);
  std::stable_sort(ranking.begin(), ranking.end(),
                   [&](size_t a, size_t b) { return indexed[a].score > indexed[b].score; });
  double minimum = query.empty() ? 0 : indexed[ranking.front()].score * weights.min_score_ratio;
  std::set<size_t> selected;
  size_t cost = 0;
  for (auto index : ranking) {
    auto &unit = indexed[index];
    if (!query.empty() && (unit.score <= 0 || unit.score < minimum))
      continue;
    size_t added = 0;
    for (auto b : unit.blocks)
      added += estimate_tokens(render_block(d.blocks[b]));
    if (cost + added > tokens)
      continue;
    cost += added;
    selected.insert(unit.blocks.begin(), unit.blocks.end());
  }
  // If a subsection is larger than the budget, retain individual matching blocks
  // plus their parent headings and immediate context instead of blind prefix cuts.
  if (selected.empty())
    for (auto index : ranking) {
      auto &unit = indexed[index];
      if (!query.empty() && (unit.score <= 0 || unit.score < minimum))
        continue;
      for (auto b : unit.blocks) {
        auto added = estimate_tokens(render_block(d.blocks[b]));
        if (cost + added <= tokens) {
          selected.insert(b);
          cost += added;
        }
      }
      if (!selected.empty())
        break;
    }
  return {selected.begin(), selected.end()};
}
CanonicalDocument select_document(const CanonicalDocument &original,
                                  const std::optional<std::string> &query, size_t budget,
                                  ExtractionInfo *info) {
  auto d = original;
  CanonicalDocument header = d;
  header.blocks.clear();
  size_t reserve = estimate_tokens(render(header)) + 64;
  if (budget <= reserve)
    throw Error(ErrorCode::ResponseTooLarge, "Token budget cannot hold provenance");
  auto content_budget = budget - reserve;
  if (query) {
    std::unordered_set<std::string> terms;
    for (auto &t : technical_tokens(*query))
      terms.insert(t);
    for (auto &block : d.blocks) {
      if (block.type == BlockType::Table && block.rows.size() > 10) {
        auto rows = block.rows;
        block.rows = {rows.front()};
        for (size_t i = 1; i < rows.size(); ++i) {
          std::string line;
          for (auto &cell : rows[i])
            line += cell + ' ';
          if (token_match(line, terms))
            block.rows.push_back(rows[i]);
        }
        if (block.rows.size() == 1)
          block.rows = std::move(rows);
      }
      if ((block.type == BlockType::Code || block.type == BlockType::Diff) &&
          estimate_tokens(render_block(block)) > content_budget / 2) {
        std::vector<std::string> lines;
        std::istringstream input(block.text);
        std::string line;
        while (std::getline(input, line))
          lines.push_back(line);
        std::set<size_t> chosen;
        size_t cost = 0;
        for (size_t i = 0; i < lines.size(); ++i)
          if (token_match(lines[i], terms)) {
            size_t start = i > 4 ? i - 4 : 0, end = std::min(lines.size(), i + 5);
            for (size_t j = start; j < end; ++j)
              if (!chosen.contains(j) && cost + estimate_tokens(lines[j]) <= content_budget / 2) {
                cost += estimate_tokens(lines[j]);
                chosen.insert(j);
              }
          }
        if (!chosen.empty()) {
          std::string selected;
          size_t previous = 0;
          bool first = true;
          for (auto i : chosen) {
            if (!first && i > previous + 1)
              selected += "\n";
            selected += lines[i] + '\n';
            previous = i;
            first = false;
          }
          block.text = std::move(selected);
          d.truncated = true;
        }
      }
    }
  }
  auto selected = select_passages(d, query.value_or(""), content_budget);
  std::set<size_t> selected_set(selected.begin(), selected.end());
  // Parent headings are necessary context, including ancestors outside a window.
  size_t used = 0;
  for (auto i : selected)
    used += estimate_tokens(render_block(d.blocks[i]));
  for (auto i : selected)
    for (unsigned level = d.blocks[i].type == BlockType::Heading ? d.blocks[i].level : 7; i > 0;) {
      --i;
      auto &b = d.blocks[i];
      if (b.type != BlockType::Heading || b.level >= level)
        continue;
      level = b.level;
      auto added = estimate_tokens(render_block(b));
      if (!selected_set.contains(i) && used + added <= content_budget) {
        selected_set.insert(i);
        used += added;
      }
      if (level == 1)
        break;
    }
  CanonicalDocument result = d;
  result.blocks.clear();
  for (auto i : selected_set)
    result.blocks.push_back(d.blocks[i]);
  result.truncated |= result.blocks.size() != original.blocks.size();
  if (info) {
    info->selected_blocks = result.blocks.size();
    info->reduced = result.truncated;
    info->rendered_tokens = estimate_tokens(render(result, budget));
  }
  return result;
}
} // namespace saga::web
