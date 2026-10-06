#include "internal.hpp"
#include <algorithm>
#include <sstream>

namespace saga::web {
namespace {
std::vector<std::string> segments(std::string path) {
  auto at = path.find('?');
  path.resize(at == std::string::npos ? path.size() : at);
  std::vector<std::string> out;
  std::istringstream input(path);
  std::string part;
  while (std::getline(input, part, '/'))
    if (!part.empty())
      out.push_back(decoded(part));
  return out;
}
std::string join(const std::vector<std::string> &parts, size_t start,
                 size_t end = std::string::npos) {
  std::string result;
  end = std::min(end, parts.size());
  for (size_t i = start; i < end; ++i) {
    if (!result.empty())
      result += '/';
    result += parts[i];
  }
  return result;
}
std::string scalar(const Json &j, const char *key) {
  if (!j.is_object() || !j.contains(key))
    return {};
  auto &value = j[key];
  return value.is_string()                         ? value.get<std::string>()
         : value.is_number() || value.is_boolean() ? value.dump()
                                                   : std::string();
}
std::string author(const Json &j) {
  auto username = scalar(j, "username");
  if (!username.empty())
    return username;
  auto a = j.value("user", j.value("author", Json::object()));
  return scalar(a, "login").empty()
             ? scalar(a, "username").empty() ? scalar(a, "name") : scalar(a, "username")
             : scalar(a, "login");
}
std::string base64_decode(std::string_view input) {
  std::string out;
  uint32_t value = 0;
  unsigned bits = 0;
  for (unsigned char c : input) {
    if (std::isspace(c))
      continue;
    if (c == '=')
      break;
    auto at = std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")
                  .find(c);
    if (at == std::string_view::npos)
      throw Error(ErrorCode::PlatformApiFailure, "Invalid file encoding");
    value = (value << 6) | static_cast<unsigned>(at);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>(value >> bits);
    }
  }
  return out;
}
void append_markdown(CanonicalDocument &d, std::string_view body, const WebLimits &limits) {
  if (body.empty())
    return;
  auto parsed = markdown_document(body, d.source, limits);
  for (auto &b : parsed.blocks)
    d.blocks.push_back(std::move(b));
}
void append_text(CanonicalDocument &d, std::string value, BlockType type = BlockType::Paragraph,
                 std::string language = {}) {
  if (value.empty())
    return;
  Block b;
  b.type = type;
  b.text = std::move(value);
  b.language = std::move(language);
  d.blocks.push_back(std::move(b));
}
void metadata(CanonicalDocument &d, const Json &j) {
  d.metadata.title = scalar(j, "title");
  if (d.metadata.title.empty())
    d.metadata.title = scalar(j, "name");
  d.metadata.author = author(j);
  d.metadata.published = scalar(j, "created_at");
  d.metadata.modified = scalar(j, "updated_at");
  for (auto *key : {"state", "web_url", "html_url", "sha", "tag_name"}) {
    auto value = scalar(j, key);
    if (!value.empty() && std::string_view(key) != "web_url" && std::string_view(key) != "html_url")
      d.metadata.fields[key] = value;
  }
  auto labels = j.value("labels", Json::array());
  std::string text;
  if (labels.is_array())
    for (auto &label : labels) {
      auto name = label.is_string() ? label.get<std::string>() : scalar(label, "name");
      if (!text.empty())
        text += ", ";
      text += name;
    }
  if (!text.empty())
    d.metadata.fields["labels"] = text;
}
void comments(CanonicalDocument &d, const Json &values, const WebLimits &limits,
              bool html = false) {
  if (!values.is_array())
    throw Error(ErrorCode::PlatformApiFailure, "Unexpected comment response");
  size_t count = 0;
  for (auto &value : values) {
    if (!value.is_object())
      continue;
    if (count++ >= limits.max_forum_posts) {
      d.truncated = true;
      break;
    }
    if (value.value("system", false))
      continue;
    Block post;
    post.type = BlockType::ForumPost;
    post.author = author(value);
    post.timestamp = scalar(value, "created_at");
    if (value.contains("post_number") && value["post_number"].is_number_integer())
      post.post_number = value["post_number"];
    if (value.contains("reply_to_post_number") && value["reply_to_post_number"].is_number_integer())
      post.reply_to = value["reply_to_post_number"];
    auto body = scalar(value, html ? "cooked" : "body");
    if (body.empty())
      body = scalar(value, "description");
    if (html) {
      auto analysis = analyze_html(body, d.source, limits);
      post.children = std::move(analysis.document.blocks);
    } else
      post.children = markdown_document(body, d.source, limits).blocks;
    if (!post.children.empty())
      d.blocks.push_back(std::move(post));
  }
}
void directory(CanonicalDocument &d, const Json &values, const WebLimits &limits) {
  if (!values.is_array())
    throw Error(ErrorCode::PlatformApiFailure, "Unexpected directory response");
  size_t count = 0;
  for (auto &value : values) {
    if (count++ >= limits.max_table_rows) {
      d.truncated = true;
      break;
    }
    std::string name = scalar(value, "path");
    if (name.empty())
      name = scalar(value, "name");
    auto type = scalar(value, "type");
    append_text(d, (type.empty() ? "" : type + " ") + name, BlockType::ListItem);
  }
}
} // namespace
std::optional<CanonicalDocument>
WebAcquisitionEngine::structured(const InstanceDescriptor &instance, const WebResponse &response,
                                 bool cache) {
  auto get_response = [&](const std::string &endpoint) {
    auto result = fetch(endpoint, cache);
    if (result.status == 429 ||
        (result.status == 403 && (result.headers.contains("x-ratelimit-remaining") &&
                                  result.headers.at("x-ratelimit-remaining") == "0")))
      throw Error(ErrorCode::RateLimited, "Platform API rate limited; use HTML fallback");
    if (result.status == 401 || result.status == 403)
      throw Error(ErrorCode::AuthenticationRequired, "Public API access unavailable");
    if (result.status != 200)
      throw Error(ErrorCode::PlatformApiFailure,
                  "HTTP " + std::to_string(result.status) + " from platform API");
    return result;
  };
  auto json = [&](const std::string &endpoint) {
    auto result = get_response(endpoint);
    try {
      return Json::parse(result.body, [](int depth, Json::parse_event_t, Json &) {
        if (depth > 64)
          throw Error(ErrorCode::PlatformApiFailure, "JSON nesting exceeds limit");
        return true;
      });
    } catch (const Json::exception &) {
      throw Error(ErrorCode::PlatformApiFailure, "API returned invalid JSON");
    }
  };
  CanonicalDocument d;
  d.source = {response.requested_url.empty() ? response.url : response.requested_url,
              response.url,
              "",
              "",
              "API",
              instance.platform,
              ResourceType::Unknown,
              {},
              true};
  auto route = response.url.substr(instance.origin.size() + instance.mount_path.size());
  auto parts = segments(route);
  auto base = instance.api_base;
  if (instance.platform == Platform::Discourse) {
    if (parts.empty() || parts[0] != "t")
      return {};
    std::string id;
    for (size_t i = 1; i < parts.size(); ++i)
      if (!parts[i].empty() && std::all_of(parts[i].begin(), parts[i].end(),
                                           [](unsigned char c) { return std::isdigit(c); })) {
        id = parts[i];
        break;
      }
    if (id.empty())
      return {};
    auto topic = json(instance.origin + instance.mount_path + "/t/" + id + ".json");
    if (!topic.is_object() ||
        !topic.value("post_stream", Json::object()).value("posts", Json()).is_array())
      throw Error(ErrorCode::PlatformApiFailure, "Invalid Discourse topic");
    d.source.resource_type = ResourceType::ForumThread;
    metadata(d, topic);
    auto posts = topic["post_stream"]["posts"];
    comments(d, posts, limits_, true);
    auto stream = topic["post_stream"].value("stream", Json::array());
    if (stream.size() > posts.size()) {
      std::set<int64_t> present;
      for (auto &post : posts)
        if (post.value("id", Json()).is_number_integer())
          present.insert(post["id"].get<int64_t>());
      std::string query;
      size_t fetched = posts.size();
      for (auto &post_id : stream)
        if (post_id.is_number_integer() && !present.contains(post_id.get<int64_t>())) {
          if (fetched++ >= limits_.max_forum_posts) {
            d.truncated = true;
            break;
          }
          query += (query.empty() ? "?" : "&") + std::string("post_ids%5B%5D=") + post_id.dump();
        }
      if (!query.empty()) {
        auto additional =
            json(instance.origin + instance.mount_path + "/t/" + id + "/posts.json" + query);
        comments(d, additional.at("post_stream").at("posts"), limits_, true);
      }
    }
    return d;
  }
  if (instance.platform == Platform::MediaWiki) {
    std::string title, revision;
    auto query_at = route.find('?');
    auto route_path = route.substr(0, query_at);
    auto wiki = route.find("/wiki/");
    if (wiki != std::string::npos)
      title = decoded(route_path.substr(wiki + 6));
    if (query_at != std::string::npos) {
      std::istringstream query(route.substr(query_at + 1));
      std::string parameter;
      while (std::getline(query, parameter, '&')) {
        auto equal = parameter.find('=');
        auto key = decoded(parameter.substr(0, equal));
        auto value = equal == std::string::npos ? std::string() : parameter.substr(equal + 1);
        std::replace(value.begin(), value.end(), '+', ' ');
        value = decoded(value);
        if (key == "title")
          title = value;
        if (key == "oldid" && !value.empty() &&
            std::all_of(value.begin(), value.end(),
                        [](unsigned char c) { return std::isdigit(c); }))
          revision = value;
      }
    }
    // MediaWiki also supports short URLs such as /Page_Name and /Page/Subpage.
    if (title.empty() && revision.empty() && route_path.size() > 1 && !route_path.ends_with(".php"))
      title = decoded(route_path.substr(1));
    if (title.empty() && revision.empty())
      return {};
    auto result =
        json(base + "?action=parse&" +
             (revision.empty() ? "page=" + web_encode(title) : "oldid=" + web_encode(revision)) +
             "&prop=text%7Csections%7Cdisplaytitle&format=json&formatversion=2&redirects=1");
    if (!result.value("parse", Json()).is_object())
      throw Error(ErrorCode::PlatformApiFailure, "Invalid MediaWiki parse response");
    auto &article = result["parse"];
    auto body = scalar(article, "text");
    if (body.empty() && article.value("text", Json()).is_object())
      body = scalar(article["text"], "*");
    auto analysis = analyze_html(body, d.source, limits_);
    // The parse API already selects the article body. Applying page-region
    // scoring again can discard short options or tables without <main> wrappers.
    d.blocks = std::move(analysis.document.blocks);
    d.metadata = std::move(analysis.document.metadata);
    d.metadata.title = scalar(article, "title");
    d.source.resource_type = ResourceType::WikiPage;
    if (!revision.empty())
      d.metadata.fields["revision"] = revision;
    return d;
  }
  bool gitlab = instance.platform == Platform::GitLab;
  bool github =
      instance.platform == Platform::GitHub || instance.platform == Platform::GitHubEnterprise;
  bool gitea = instance.platform == Platform::Gitea || instance.platform == Platform::Forgejo;
  if (!gitlab && !github && !gitea)
    return {};
  std::string repo;
  size_t resource = 0;
  std::string endpoint;
  if (gitlab) {
    auto marker = std::find(parts.begin(), parts.end(), "-");
    size_t count =
        marker == parts.end() ? parts.size() : static_cast<size_t>(marker - parts.begin());
    if (count < 2)
      return {};
    repo = join(parts, 0, count);
    resource = marker == parts.end() ? parts.size() : count + 1;
    endpoint = base + "/projects/" + web_encode(repo);
  } else {
    if (parts.size() < 2)
      return {};
    repo = join(parts, 0, 2);
    resource = 2;
    endpoint = base + "/repos/" + web_encode(parts[0]) + "/" + web_encode(parts[1]);
  }
  d.source.repository = repo;
  if (resource >= parts.size()) {
    auto repository = json(endpoint);
    if (!repository.is_object())
      throw Error(ErrorCode::PlatformApiFailure, "Invalid repository response");
    d.source.resource_type = ResourceType::Repository;
    metadata(d, repository);
    append_markdown(d, scalar(repository, "description"), limits_);
    auto branch = scalar(repository, "default_branch");
    d.metadata.fields["default_branch"] = branch;
    if (gitlab) {
      try {
        auto readme =
            get_response(endpoint + "/repository/files/README.md/raw?ref=" + web_encode(branch));
        append_markdown(d, normalize_encoding(readme), limits_);
      } catch (const Error &) {
      }
    } else {
      try {
        auto readme = json(endpoint + "/readme");
        auto content = scalar(readme, "content");
        if (!content.empty())
          append_markdown(d, base64_decode(content), limits_);
      } catch (const Error &) {
      }
    }
    return d;
  }
  auto kind = parts[resource];
  auto id = resource + 1 < parts.size() ? parts[resource + 1] : std::string();
  if (kind == "issues" || kind == "pull" || kind == "pulls" || kind == "merge_requests") {
    if (id.empty())
      return {};
    bool pull = kind != "issues";
    auto path = gitlab ? (pull ? "/merge_requests/" : "/issues/") : (pull ? "/pulls/" : "/issues/");
    auto item = json(endpoint + path + web_encode(id));
    if (!item.is_object() || scalar(item, "title").empty())
      throw Error(ErrorCode::PlatformApiFailure, "Invalid issue/pull request");
    d.source.resource_type = pull ? gitlab ? ResourceType::MergeRequest : ResourceType::PullRequest
                                  : ResourceType::Issue;
    metadata(d, item);
    d.metadata.fields["number"] = id;
    auto body = scalar(item, gitlab ? "description" : "body");
    append_markdown(d, body, limits_);
    auto comments_endpoint =
        gitlab ? endpoint + path + web_encode(id) + "/notes?per_page=100"
               : endpoint + "/issues/" + web_encode(id) + "/comments?per_page=100&limit=100";
    comments(d, json(comments_endpoint), limits_);
    if (pull && include_diffs_) {
      auto diffs = json(endpoint + path + web_encode(id) +
                        (gitlab ? "/diffs?per_page=100" : "/files?per_page=100&limit=100"));
      if (!diffs.is_array())
        throw Error(ErrorCode::PlatformApiFailure, "Invalid review diff response");
      size_t count = 0;
      for (auto &diff : diffs) {
        if (count++ >= limits_.max_table_rows) {
          d.truncated = true;
          break;
        }
        Block block;
        block.type = BlockType::Diff;
        block.path = scalar(diff, gitlab ? "new_path" : "filename");
        block.text = scalar(diff, gitlab ? "diff" : "patch");
        if (!block.text.empty())
          d.blocks.push_back(std::move(block));
      }
    }
    return d;
  }
  if (kind == "commit" || kind == "commits") {
    if (id.empty())
      return {};
    auto item = json(endpoint + (gitlab ? "/repository/commits/" : "/commits/") + web_encode(id));
    if (!item.is_object())
      throw Error(ErrorCode::PlatformApiFailure, "Invalid commit response");
    d.source.resource_type = ResourceType::Commit;
    metadata(d, item);
    auto commit = item.value("commit", item);
    auto message = scalar(commit, "message");
    if (d.metadata.title.empty())
      d.metadata.title = message;
    append_text(d, message);
    Json files =
        gitlab ? json(endpoint + "/repository/commits/" + web_encode(id) + "/diff?per_page=100")
               : item.value("files", Json::array());
    if (files.is_array())
      for (auto &file : files) {
        Block b;
        b.type = BlockType::Diff;
        b.path = scalar(file, gitlab ? "new_path" : "filename");
        b.text = scalar(file, gitlab ? "diff" : "patch");
        if (!b.text.empty())
          d.blocks.push_back(std::move(b));
      }
    return d;
  }
  if (kind == "releases") {
    if (id == "tag" && resource + 2 < parts.size())
      id = join(parts, resource + 2);
    if (id.empty())
      return {};
    auto item = json(endpoint + "/releases/" + (gitlab ? "" : "tags/") + web_encode(id));
    if (!item.is_object())
      throw Error(ErrorCode::PlatformApiFailure, "Invalid release response");
    d.source.resource_type = ResourceType::Release;
    metadata(d, item);
    append_markdown(d, scalar(item, gitlab ? "description" : "body"), limits_);
    return d;
  }
  if (kind == "blob" || kind == "tree" || kind == "src" || kind == "raw") {
    size_t start = resource + 1;
    if (gitea && start < parts.size() &&
        (parts[start] == "branch" || parts[start] == "tag" || parts[start] == "commit"))
      ++start;
    if (start >= parts.size())
      return {};
    std::string ref = parts[start], path = join(parts, start + 1);
    bool tree = kind == "tree" || path.empty();
    if (gitlab) {
      if (tree) {
        d.source.resource_type = ResourceType::Directory;
        d.metadata.title = repo + " / " + path;
        directory(d,
                  json(endpoint + "/repository/tree?ref=" + web_encode(ref) +
                       "&path=" + web_encode(path) + "&per_page=100"),
                  limits_);
        return d;
      }
      auto file = get_response(endpoint + "/repository/files/" + web_encode(path) +
                               "/raw?ref=" + web_encode(ref));
      if (sniff(file) == Representation::Binary)
        throw Error(ErrorCode::UnsupportedContent, "Binary repository file");
      d.source.resource_type = ResourceType::File;
      d.metadata.title = path;
      auto body = normalize_encoding(file);
      if (path.ends_with(".md") || path.ends_with(".markdown"))
        append_markdown(d, body, limits_);
      else
        append_text(d, body, BlockType::Code, path.substr(path.find_last_of('.') + 1));
      return d;
    }
    auto contents = json(endpoint + "/contents/" + web_encode(path) + "?ref=" + web_encode(ref));
    if (contents.is_array()) {
      d.source.resource_type = ResourceType::Directory;
      d.metadata.title = repo + " / " + path;
      directory(d, contents, limits_);
      return d;
    }
    if (!contents.is_object())
      throw Error(ErrorCode::PlatformApiFailure, "Invalid repository content response");
    d.source.resource_type = ResourceType::File;
    d.metadata.title = path;
    auto content = scalar(contents, "content");
    std::string body;
    if (!content.empty()) {
      if (scalar(contents, "encoding") != "base64")
        throw Error(ErrorCode::UnsupportedContent, "Unknown repository file encoding");
      body = base64_decode(content);
    } else {
      auto raw = scalar(contents, "download_url");
      if (raw.empty())
        throw Error(ErrorCode::PlatformApiFailure,
                    "File requires authenticated or unavailable raw resource");
      body = normalize_encoding(get_response(raw));
    }
    WebResponse file;
    file.body = body;
    file.content_type = "text/plain";
    if (sniff(file) == Representation::Binary)
      throw Error(ErrorCode::UnsupportedContent, "Binary repository file");
    if (path.ends_with(".md") || path.ends_with(".markdown"))
      append_markdown(d, body, limits_);
    else
      append_text(d, body, BlockType::Code, path.substr(path.find_last_of('.') + 1));
    return d;
  }
  return {};
}
} // namespace saga::web
