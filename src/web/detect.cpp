#include "internal.hpp"
#include <algorithm>

namespace saga::web {
namespace {
PlatformCapabilities capabilities(Platform platform) {
  bool lab = platform == Platform::GitLab;
  bool forge = lab || platform == Platform::GitHub || platform == Platform::GitHubEnterprise ||
               platform == Platform::Forgejo || platform == Platform::Gitea;
  return {platform != Platform::Unknown, forge, forge, forge && !lab, lab, forge, forge};
}
} // namespace
InstanceDescriptor passive_instance(std::string_view url, const Json &signals) {
  InstanceDescriptor result;
  result.origin = url_origin(url);
  auto host = web_host(url);
  auto generator = lower(signals.value("generator", std::string())),
       assets = lower(signals.value("assets", Json::array()).dump()),
       footer = lower(signals.value("footer", std::string())),
       classes = lower(signals.value("classes", std::string()));
  std::map<Platform, double> scores;
  if (host == "github.com")
    scores[Platform::GitHub] = 1;
  else if (host == "gitlab.com")
    scores[Platform::GitLab] = 1;
  else if (host == "codeberg.org")
    scores[Platform::Forgejo] = 1;
  auto evidence = [&](Platform p, bool match, double weight) {
    if (match)
      scores[p] += weight;
  };
  evidence(Platform::GitLab, generator.find("gitlab") != std::string::npos, 0.75);
  evidence(Platform::GitLab,
           assets.find("/assets/webpack/") != std::string::npos ||
               assets.find("/assets/gitlab-") != std::string::npos,
           0.3);
  evidence(Platform::GitLab,
           classes.find("gl-new-dropdown") != std::string::npos ||
               classes.find("gitlab-content") != std::string::npos,
           0.2);
  evidence(Platform::Forgejo, generator.find("forgejo") != std::string::npos, 0.75);
  evidence(Platform::Forgejo, footer.find("powered by forgejo") != std::string::npos, 0.5);
  evidence(Platform::Forgejo, assets.find("forgejo") != std::string::npos, 0.3);
  evidence(Platform::Gitea, generator.find("gitea") != std::string::npos, 0.75);
  evidence(Platform::Gitea, footer.find("powered by gitea") != std::string::npos, 0.5);
  evidence(Platform::Gitea,
           assets.find("/assets/js/index.js") != std::string::npos &&
               classes.find("repository") != std::string::npos,
           0.3);
  evidence(Platform::GitHubEnterprise,
           !signals.value("github_enterprise_version", std::string()).empty(), 0.8);
  evidence(Platform::GitHubEnterprise, assets.find("githubassets") != std::string::npos, 0.3);
  evidence(Platform::GitHubEnterprise,
           classes.find("logged-out") != std::string::npos &&
               classes.find("page-responsive") != std::string::npos,
           0.25);
  evidence(Platform::Discourse, generator.find("discourse") != std::string::npos, 0.75);
  evidence(Platform::Discourse,
           assets.find("/assets/discourse-") != std::string::npos ||
               assets.find("/assets/ember-") != std::string::npos,
           0.3);
  evidence(Platform::MediaWiki, generator.find("mediawiki") != std::string::npos, 0.75);
  evidence(Platform::MediaWiki, assets.find("/load.php") != std::string::npos, 0.3);
  evidence(Platform::MediaWiki, classes.find("mw-parser-output") != std::string::npos, 0.25);
  for (auto [platform, confidence] : scores)
    if (confidence > result.confidence) {
      result.platform = platform;
      result.confidence = std::min(1.0, confidence);
    }
  if (result.confidence < 0.4) {
    result.platform = Platform::Unknown;
    result.confidence = 0;
  }
  auto path = [&](std::string_view candidate) {
    try {
      auto resolved = web_url(candidate, result.origin, true);
      if (url_origin(resolved) != result.origin)
        return std::string();
      auto value = resolved.substr(result.origin.size());
      auto q = value.find('?');
      return value.substr(0, q);
    } catch (const std::exception &) {
      return std::string();
    }
  };
  for (auto &base : signals.value("bases", Json::array()))
    if (base.is_string()) {
      auto p = path(base.get<std::string>());
      if (!p.empty() && p.ends_with('/') && std::string(url).starts_with(result.origin + p))
        result.mount_path = p == "/" ? "" : p.substr(0, p.size() - 1);
    }
  if (result.mount_path.empty())
    for (auto &asset : signals.value("assets", Json::array()))
      if (asset.is_string()) {
        auto p = path(asset.get<std::string>());
        size_t marker = p.find("/assets/");
        if (result.platform == Platform::MediaWiki)
          marker = p.find("/load.php");
        if (result.platform == Platform::MediaWiki && p.find("/w/load.php") != std::string::npos)
          marker = p.find("/w/load.php");
        if (marker != std::string::npos && marker && marker < 128 &&
            std::string(url).starts_with(result.origin + p.substr(0, marker) + "/")) {
          result.mount_path = p.substr(0, marker);
          break;
        }
      }
  if (result.platform == Platform::GitHub)
    result.api_base = "https://api.github.com";
  else if (result.platform == Platform::GitHubEnterprise)
    result.api_base = result.origin + result.mount_path + "/api/v3";
  else if (result.platform == Platform::GitLab)
    result.api_base = result.origin + result.mount_path + "/api/v4";
  else if (result.platform == Platform::Forgejo || result.platform == Platform::Gitea)
    result.api_base = result.origin + result.mount_path + "/api/v1";
  else if (result.platform == Platform::MediaWiki) {
    result.api_base = result.origin + result.mount_path + "/api.php";
    for (auto &asset : signals.value("assets", Json::array())) {
      if (!asset.is_string())
        continue;
      auto p = path(asset.get<std::string>());
      auto loader = p.find("/load.php");
      if (loader != std::string::npos) {
        result.api_base = result.origin + p.substr(0, loader) + "/api.php";
        break;
      }
    }
  } else
    result.api_base = result.origin + result.mount_path;
  result.version = signals.value("github_enterprise_version", std::string());
  result.capabilities = capabilities(result.platform);
  return result;
}
InstanceDescriptor WebAcquisitionEngine::detect(const WebResponse &response,
                                                const CanonicalDocument &, const Json &raw_signals,
                                                bool cache, ExtractionInfo &info) {
  auto signals = raw_signals;
  auto header = response.headers.find("x-github-enterprise-version");
  if (header != response.headers.end())
    signals["github_enterprise_version"] = header->second;
  auto origin = url_origin(response.url);
  auto passive = passive_instance(response.url, signals);
  if (db_ && cache)
    for (auto &row : db_->query("SELECT mount_path,descriptor_json FROM web_instances WHERE "
                                "origin=? AND expires_at>? ORDER BY length(mount_path) DESC",
                                {origin, now()})) {
      auto mount = row["mount_path"].get<std::string>();
      auto prefix = origin + mount;
      if (response.url != prefix && !response.url.starts_with(prefix + "/") &&
          !response.url.starts_with(prefix + "?"))
        continue;
      auto j = Json::parse(row["descriptor_json"].get<std::string>());
      if (j.at("platform") == static_cast<int>(Platform::Unknown) &&
          passive.platform != Platform::Unknown)
        continue;
      InstanceDescriptor d;
      d.origin = origin;
      d.mount_path = mount;
      d.platform = static_cast<Platform>(j.at("platform").get<int>());
      d.api_base = j.value("api_base", "");
      d.version = j.value("version", "");
      d.confidence = j.value("confidence", 0.0);
      d.capabilities = capabilities(d.platform);
      info.instance_cache_hit = true;
      return d;
    }
  auto d = passive;
  if (d.platform != Platform::Unknown && (d.confidence < 1 || d.platform == Platform::Gitea)) {
    std::vector<std::pair<std::string, Platform>> probes;
    auto base = d.origin + d.mount_path;
    switch (d.platform) {
    case Platform::Forgejo:
    case Platform::Gitea:
      probes = {{base + "/api/forgejo/v1/version", Platform::Forgejo},
                {base + "/api/v1/version", Platform::Gitea}};
      break;
    case Platform::GitLab:
      probes = {{base + "/api/v4/version", Platform::GitLab},
                {base + "/api/v4/metadata", Platform::GitLab}};
      break;
    case Platform::GitHubEnterprise:
      probes = {{base + "/api/v3/meta", Platform::GitHubEnterprise}};
      break;
    case Platform::Discourse:
      probes = {{base + "/site.json", Platform::Discourse}};
      break;
    case Platform::MediaWiki: {
      auto api = d.api_base;
      auto assets = signals.value("assets", Json::array()).dump();
      if (assets.find("/w/load.php") != std::string::npos)
        api = d.origin + d.mount_path + "/w/api.php";
      d.api_base = api;
      probes = {{api + "?action=query&meta=siteinfo&format=json", Platform::MediaWiki}};
      break;
    }
    default:
      break;
    }
    for (auto &[url, platform] : probes)
      try {
        auto probe = fetch(url, cache);
        if (probe.status != 200)
          continue;
        auto j = Json::parse(probe.body, [](int depth, Json::parse_event_t, Json &) {
          if (depth > 64)
            throw Error(ErrorCode::PlatformApiFailure, "Platform probe nesting limit");
          return true;
        });
        bool valid = false;
        if (platform == Platform::Forgejo || platform == Platform::Gitea ||
            platform == Platform::GitLab)
          valid = j.is_object() && j.value("version", Json()).is_string();
        else if (platform == Platform::GitHubEnterprise)
          valid = j.is_object() && (j.contains("verifiable_password_authentication") ||
                                    j.contains("installed_version"));
        else if (platform == Platform::Discourse)
          valid = j.is_object() && j.value("categories", Json()).is_array() &&
                  (j.contains("post_types") || j.contains("notification_types"));
        else if (platform == Platform::MediaWiki)
          valid = j.value("query", Json::object()).value("general", Json()).is_object();
        if (valid) {
          d.platform = platform;
          d.confidence = 1;
          if (j.value("version", Json()).is_string())
            d.version = j["version"];
          break;
        }
      } catch (const Error &e) {
        if (e.code == ErrorCode::BlockedAddress || e.code == ErrorCode::ResponseTooLarge)
          throw;
      } catch (const Json::exception &) {
        // A 200 response containing a login page or invalid JSON is not evidence.
      }
  }
  if (d.platform != Platform::Unknown && d.confidence < 0.7) {
    d.platform = Platform::Unknown;
    d.api_base.clear();
  }
  d.capabilities = capabilities(d.platform);
  if (db_ && cache) {
    auto ttl = d.platform == Platform::Unknown ? 60 * 60 * 1000LL
               : d.confidence == 1             ? 7 * 24 * 60 * 60 * 1000LL
                                               : 24 * 60 * 60 * 1000LL;
    Json j = {{"platform", static_cast<int>(d.platform)},
              {"api_base", d.api_base},
              {"version", d.version},
              {"confidence", d.confidence},
              {"capabilities",
               {{"api_available", d.capabilities.api_available},
                {"raw_files", d.capabilities.raw_files},
                {"issues", d.capabilities.issues},
                {"pull_requests", d.capabilities.pull_requests},
                {"merge_requests", d.capabilities.merge_requests},
                {"repository_tree", d.capabilities.repository_tree},
                {"releases", d.capabilities.releases}}}};
    db_->exec("INSERT INTO web_instances(origin,mount_path,descriptor_json,expires_at) "
              "VALUES(?,?,?,?) ON CONFLICT(origin,mount_path) DO UPDATE SET "
              "descriptor_json=excluded.descriptor_json,expires_at=excluded.expires_at",
              {d.origin, d.mount_path, j.dump(), now() + ttl});
  }
  return d;
}
} // namespace saga::web
