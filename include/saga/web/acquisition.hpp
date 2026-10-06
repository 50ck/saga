#pragma once
#include <map>
#include <optional>
#include <saga/web.hpp>
#include <set>

namespace saga::web {
enum class ErrorCode {
  NetworkError,
  Timeout,
  TooManyRedirects,
  ResponseTooLarge,
  UnsupportedContent,
  InvalidEncoding,
  BlockedAddress,
  PlatformApiFailure,
  ExtractionFailure,
  DynamicContentUnavailable,
  LowConfidenceExtraction,
  RateLimited,
  AuthenticationRequired
};
std::string error_name(ErrorCode);
struct Error : std::runtime_error {
  ErrorCode code;
  Error(ErrorCode kind, const std::string &message)
      : std::runtime_error(error_name(kind) + ": " + message), code(kind) {}
};
enum class Platform {
  Unknown,
  GitHub,
  GitHubEnterprise,
  GitLab,
  Forgejo,
  Gitea,
  Discourse,
  MediaWiki
};
enum class ResourceType {
  Unknown,
  Article,
  Documentation,
  ForumThread,
  Repository,
  Directory,
  File,
  Commit,
  Diff,
  Issue,
  PullRequest,
  MergeRequest,
  Release,
  WikiPage
};
enum class Representation { Html, Markdown, PlainText, Json, Binary, Unknown };
enum class PageProfile { Prose, Documentation, Discussion, Reference, Listing, Unknown };
enum class ExtractionMode { Strict, Balanced, Recall };
enum class BlockType {
  Heading,
  Paragraph,
  ListItem,
  Definition,
  Code,
  Quote,
  Table,
  FigureCaption,
  ForumPost,
  Diff
};
std::string platform_name(Platform);
std::string resource_name(ResourceType);
std::string profile_name(PageProfile);
struct WebLimits {
  size_t max_response_bytes = 2 * 1024 * 1024, max_document_chars = 256 * 1024;
  size_t max_code_block_bytes = 64 * 1024, max_table_rows = 100, max_table_columns = 12,
         max_cell_size = 2048;
  size_t max_forum_posts = 100, max_blocks = 8000, max_dom_nodes = 100000, max_dom_depth = 128;
  size_t max_rendered_tokens = 4096;
  int max_redirects = 5, connect_timeout = 10, total_timeout = 30;
  bool allow_private_network = false;
};
struct Block {
  BlockType type = BlockType::Paragraph;
  std::string text, language, author, timestamp, path;
  unsigned level = 0, indent = 0;
  std::optional<int64_t> post_number, reply_to;
  std::vector<Block> children;
  std::vector<std::vector<std::string>> rows;
  size_t id = 0, dom_complexity = 1, character_count = 0, link_chars = 0, link_count = 0;
  bool inside_main = false, inside_article = false, inside_nav = false, inside_aside = false,
       inside_header = false, inside_footer = false;
  std::string hints, structural_signature;
  double readability = 0, density = 0, semantic = 0, continuity = 0, template_penalty = 0,
         score = 0;
};
struct SourceMetadata {
  std::string requested_url, final_url, canonical_url, repository, acquisition_path;
  Platform platform = Platform::Unknown;
  ResourceType resource_type = ResourceType::Unknown;
  std::vector<std::string> fallbacks;
  bool untrusted = true;
};
struct DocumentMetadata {
  std::string title, author, language, published, modified, description;
  std::map<std::string, std::string> fields;
};
struct CanonicalDocument {
  SourceMetadata source;
  DocumentMetadata metadata;
  std::vector<Block> blocks;
  bool truncated = false;
};
struct PlatformCapabilities {
  bool api_available = false, raw_files = false, issues = false, pull_requests = false,
       merge_requests = false, repository_tree = false, releases = false;
};
struct InstanceDescriptor {
  Platform platform = Platform::Unknown;
  std::string origin, mount_path, api_base, version;
  double confidence = 0;
  PlatformCapabilities capabilities;
};
struct ExtractionWeights {
  double readability = 1, density = 0.7, semantic = 1.5, continuity = 0.6, template_penalty = 2;
};
ExtractionWeights extraction_weights(PageProfile);
struct ExtractionQuality {
  double confidence = 0, source_coverage = 0, link_density = 0, boilerplate_probability = 0;
  size_t extracted_chars = 0, extracted_blocks = 0;
  bool likely_dynamic = false;
};
struct ExtractionInfo {
  PageProfile profile = PageProfile::Unknown;
  ExtractionMode mode = ExtractionMode::Balanced;
  ExtractionQuality quality;
  size_t downloaded_bytes = 0, blocks_before = 0, blocks_after = 0, selected_blocks = 0,
         rendered_tokens = 0;
  long http_status = 0;
  bool http_cache_hit = false, instance_cache_hit = false, duplicate = false, reduced = false;
  std::string content_type;
  std::vector<double> scores;
  Json json() const;
};
struct AcquisitionRequest {
  std::string url;
  std::optional<std::string> focus_query;
  size_t max_output_tokens = 0;
  bool allow_cache = true;
};
struct AcquisitionResult {
  CanonicalDocument document;
  std::string rendered;
  ExtractionInfo extraction;
};
struct RetrievalWeights {
  double title = 3, heading = 2.5, body = 1, code = 1.3, metadata = 0.5, k1 = 1.2, b = 0.75,
         min_score_ratio = 0.2;
};
Representation sniff(const WebResponse &);
std::string normalize_encoding(const WebResponse &);
std::string sanitize_text(std::string_view, bool preserve_whitespace = false);
std::string clean_url(std::string_view, std::string_view base = {});
std::string redacted_url(std::string_view);
std::string url_origin(std::string_view);
std::vector<std::string> technical_tokens(std::string_view);
uint64_t simhash(std::string_view);
CanonicalDocument markdown_document(std::string_view, const SourceMetadata &,
                                    const WebLimits & = {});
void sanitize(CanonicalDocument &, const WebLimits & = {});
void deduplicate(CanonicalDocument &);
std::string render_block(const Block &);
std::string render(const CanonicalDocument &, size_t max_tokens = 0);
std::vector<size_t> select_passages(const CanonicalDocument &, std::string_view, size_t,
                                    const RetrievalWeights & = {});
CanonicalDocument select_document(const CanonicalDocument &, const std::optional<std::string> &,
                                  size_t, ExtractionInfo * = nullptr);
Json document_json(const CanonicalDocument &);
CanonicalDocument document_from_json(const Json &);

// One owner per runtime. Calls are serialized by Saga's executive controller;
// callbacks service live controls without sharing mutable globals.
class WebAcquisitionEngine {
  Database *db_;
  WebTransport legacy_transport_;
  WebLimits limits_;
  std::function<void()> service_;
  std::set<uint64_t> operation_documents_;
  std::set<std::string> operation_hashes_;
  bool include_diffs_ = false;
  WebResponse fetch(const std::string &, bool cache);
  InstanceDescriptor detect(const WebResponse &, const CanonicalDocument &, const Json &signals,
                            bool cache, ExtractionInfo &);
  std::optional<CanonicalDocument> structured(const InstanceDescriptor &, const WebResponse &,
                                              bool cache);

public:
  explicit WebAcquisitionEngine(Database *db = nullptr, WebTransport transport = {},
                                WebLimits limits = {});
  void service(std::function<void()> callback) { service_ = std::move(callback); }
  void begin_operation() {
    operation_documents_.clear();
    operation_hashes_.clear();
  }
  AcquisitionResult acquire(const AcquisitionRequest &);
  const WebLimits &limits() const { return limits_; }
};
} // namespace saga::web
