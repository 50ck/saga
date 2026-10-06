#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace saga {
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Id = std::int64_t;
using Emit = std::function<void(const std::string&, const Json&)>;
inline constexpr int runtime_context_revision=15;
struct TurnCancelled : std::runtime_error { TurnCancelled() : std::runtime_error("Stopped by user") {} };
std::string json_string_prefix(std::string_view source,std::string_view key);
bool live_command_allowed(std::string_view name,const Json& arguments = Json::object());
Id now();
std::string uuid();
bool valid_uuid(std::string_view value);
std::string read_file(const fs::path& path, size_t max_bytes = 1024 * 1024);
void private_dir(const fs::path& path);
void atomic_write(const fs::path& path, std::string_view contents);
bool within(const fs::path& path, const fs::path& root);
std::string lower(std::string value);
std::string trim(std::string_view value);
size_t estimate_tokens(std::string_view value);
std::string digest(std::string_view value);
std::string display_text(std::string_view value);
std::string utf8_text(std::string_view value);
std::string utf8_excerpt(std::string_view value, size_t max_bytes, bool tail = false);
std::string base64(std::string_view value);

struct Paths {
  fs::path config, data, state, runtime;
  static Paths environment();
  void create() const;
  fs::path socket() const { return runtime / "sagad.sock"; }
  fs::path persona(std::string_view id) const;
};
struct Config {
  std::string endpoint, api_key, model;
  std::uint64_t context_length = 0, generation_reserve = 8192, safety_margin = 2048;
  // Zero default inherits generation_reserve for existing configurations.
  std::uint64_t default_max_output_tokens=0,hard_max_output_tokens=32768;
  int max_continuations=3,max_generation_retries=1;
  std::string reasoning_budget="auto";
  std::uint64_t reasoning_soft_budget=0,reasoning_hard_budget=0;
  bool reasoning_control=false,stream_assistant_text=true;
  bool allow_small_context = false, insecure_tls = false;
  int timeout_seconds = 600, max_tool_rounds = 24;
  std::string search_engine = "duckduckgo"; // Legacy first-choice setting.
  std::vector<std::string> search_engines;
  int duckduckgo_min_request_interval_ms = 1000, duckduckgo_challenge_backoff_ms = 60000;
  int search_cache_ttl_seconds = 600, search_total_timeout_seconds = 45, search_max_engine_attempts = 2;
  int fourget_directory_ttl_seconds = 21600, fourget_probe_ttl_seconds = 300;
  int fourget_failure_backoff_ms = 60000, fourget_max_instance_attempts = 6;
  int fourget_probe_timeout_seconds = 3, fourget_request_timeout_seconds = 10;
  std::vector<std::string> fourget_manual_instances;
  bool fourget_prefer_manual = true, fourget_allow_private_instances = false;
  int web_search_limit = 4, web_read_limit = 8;
  bool web_allow_private_network=false;
  size_t web_output_tokens=4096;
  double lexical_weight = 1, entity_weight = 0.6, project_weight = 1,
         goal_weight = 0.6, salience_weight = 0.5, recency_weight = 0.3,
         confidence_weight = 0.4, accessibility_weight = 0.3;
  static Config load(const Paths& paths);
  void save(const Paths& paths) const;
  void validate() const;
  size_t input_budget() const;
};
inline constexpr std::string_view default_soul =
  "I am a calm, capable, curious personal agent.\n\n"
  "I communicate naturally and directly.\n"
  "I prefer evidence over confident guessing.\n"
  "I learn from experience and correct myself when reality contradicts me.\n"
  "I value continuity, competence, clarity, and useful action.\n"
  "I adapt to the person I work with over time without becoming theatrical or sycophantic.\n"
  "I prefer demonstrating personality through behavior rather than constantly describing it.\n";
}
