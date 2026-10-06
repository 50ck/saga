#include <saga/common.hpp>
#include <saga/web.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace saga {
bool live_command_allowed(std::string_view name,const Json& arguments) {
  if (!arguments.is_object()) return false;
  if (name == "name" || name == "soul" || name == "project") return arguments.empty();
  return name == "web" || name == "diff" || name == "stop" || name == "steer" || name == "status" || name == "permissions" || name == "memory" || name == "journal" ||
    name == "goals" || name == "tasks" || name == "self" || name == "project" || name == "state";
}
std::string json_string_prefix(std::string_view source,std::string_view key) {
  // Decode only complete JSON characters of a top-level string argument.
  size_t pos=0; int depth=0;
  while(pos<source.size()) {
    char c=source[pos++]; if(c=='{' || c=='['){++depth;continue;} if(c=='}' || c==']'){--depth;continue;}
    if(c!='"')continue;
    size_t start=pos-1;
    while(pos<source.size()){if(source[pos]=='\\'){pos=std::min(pos+2,source.size());continue;}if(source[pos++]=='"')break;}
    auto property=Json::parse(source.substr(start,pos-start),nullptr,false);
    if(depth!=1 || !property.is_string() || property.get<std::string>()!=key)continue;
    while(pos<source.size() && std::isspace(static_cast<unsigned char>(source[pos])))++pos;
    if(pos==source.size() || source[pos++]!=':')continue;
    while(pos<source.size() && std::isspace(static_cast<unsigned char>(source[pos])))++pos;
    if(pos==source.size() || source[pos++]!='"')return {};
    start=pos-1;size_t end=pos;
    while(pos<source.size() && source[pos]!='"') {
      auto ch=static_cast<unsigned char>(source[pos]);size_t length=1;
      if(ch=='\\') {
        if(pos+1>=source.size())break;
        length=source[pos+1]=='u'?6:2;
        if(pos+length>source.size())break;
        if(length==6) {auto hex=static_cast<char>(std::tolower(static_cast<unsigned char>(source[pos+3])));if(std::tolower(static_cast<unsigned char>(source[pos+2]))=='d' && (hex=='8' || hex=='9' || hex=='a' || hex=='b'))length=12;}
      } else if(ch>=0x80) length=(ch&0xe0)==0xc0?2:(ch&0xf0)==0xe0?3:(ch&0xf8)==0xf0?4:1;
      if(pos+length>source.size())break;
      pos+=length;end=pos;
    }
    auto decoded=Json::parse(std::string(source.substr(start,end-start))+"\"",nullptr,false);
    return decoded.is_string()?decoded.get<std::string>():std::string();
  }
  return {};
}
Id now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string uuid() {
  unsigned char bytes[16];
  size_t offset = 0;
  while (offset < sizeof bytes) {
    auto n = getrandom(bytes + offset, sizeof bytes - offset, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("Cannot obtain secure randomness");
    offset += static_cast<size_t>(n);
  }
  bytes[6] = (bytes[6] & 15) | 64;
  bytes[8] = (bytes[8] & 63) | 128;
  std::ostringstream s;
  for (size_t i = 0; i < sizeof bytes; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) s << '-';
    s << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
  }
  return s.str();
}
bool valid_uuid(std::string_view s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
    else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
  }
  return true;
}
std::string read_file(const fs::path& path, size_t max_bytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot read file: " + path.string());
  std::string result;
  char buffer[8192];
  while (in) {
    in.read(buffer, sizeof buffer);
    result.append(buffer, static_cast<size_t>(in.gcount()));
    if (result.size() > max_bytes) throw std::runtime_error("File exceeds size limit");
  }
  if (!in.eof()) throw std::runtime_error("File read failed");
  return result;
}
void private_dir(const fs::path& path) {
  fs::create_directories(path);
  struct stat st{};
  if (lstat(path.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid())
    throw std::runtime_error("Directory is not private and owned by current user: " + path.string());
  if (chmod(path.c_str(), 0700)) throw std::runtime_error("Cannot protect directory");
}
void atomic_write(const fs::path& path, std::string_view contents) {
  auto temp = path.string() + ".tmp-" + uuid();
  int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) throw std::runtime_error("Cannot create private file");
  try {
    size_t offset = 0;
    while (offset < contents.size()) {
      auto n = write(fd, contents.data() + offset, contents.size() - offset);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::runtime_error("File write failed");
      offset += static_cast<size_t>(n);
    }
    if (fsync(fd)) throw std::runtime_error("File flush failed");
    close(fd); fd = -1;
    fs::rename(temp, path);
    int dir = open(path.parent_path().c_str(), O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) { fsync(dir); close(dir); }
  } catch (...) { if (fd >= 0) close(fd); fs::remove(temp); throw; }
}
bool within(const fs::path& p, const fs::path& r) {
  auto path = fs::weakly_canonical(p), root = fs::weakly_canonical(r);
  auto a = path.begin(), b = root.begin();
  for (; b != root.end(); ++b, ++a) if (a == path.end() || *a != *b) return false;
  return true;
}
std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string trim(std::string_view s) {
  auto first = s.find_first_not_of(" \t\r\n"), last = s.find_last_not_of(" \t\r\n");
  return first == std::string_view::npos ? "" : std::string(s.substr(first, last - first + 1));
}
size_t estimate_tokens(std::string_view s) {
  // Conservative approximation, not a byte count disguised as a token count.
  // Non-ASCII and symbol-heavy text receive extra capacity; API usage wins.
  size_t symbols=0,non_ascii=0;
  for(unsigned char c:s) { symbols+=std::ispunct(c)!=0; non_ascii+=c>=128; }
  return (s.size()+1)/2 + symbols/4 + non_ascii/2 + 8;
}
std::string digest(std::string_view s) {
  // Non-cryptographic content fingerprint for metrics, never used for security.
  std::uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  std::ostringstream out; out << std::hex << h; return out.str();
}
std::string display_text(std::string_view s) {
  std::string out;
  for (unsigned char c : s) if (c >= 32 || c == '\n' || c == '\t') out += static_cast<char>(c);
  return out;
}
std::string utf8_text(std::string_view s) {
  return Json::parse(Json(std::string(s)).dump(-1,' ',false,Json::error_handler_t::replace)).get<std::string>();
}
std::string utf8_excerpt(std::string_view s, size_t max_bytes, bool tail) {
  if (s.size() <= max_bytes) return utf8_text(s);
  if (!tail) {
    size_t end = max_bytes;
    while (end && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80) --end;
    return utf8_text(s.substr(0,end));
  }
  size_t begin = s.size()-max_bytes;
  while (begin < s.size() && (static_cast<unsigned char>(s[begin]) & 0xc0) == 0x80) ++begin;
  return utf8_text(s.substr(begin));
}
std::string base64(std::string_view value) {
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output; output.reserve((value.size()+2)/3*4);
  for (size_t i = 0; i < value.size(); i += 3) {
    auto a = static_cast<unsigned char>(value[i]);
    auto b = i+1 < value.size() ? static_cast<unsigned char>(value[i+1]) : 0;
    auto c = i+2 < value.size() ? static_cast<unsigned char>(value[i+2]) : 0;
    unsigned n = (static_cast<unsigned>(a)<<16) | (static_cast<unsigned>(b)<<8) | c;
    output += alphabet[(n>>18)&63]; output += alphabet[(n>>12)&63];
    output += i+1 < value.size() ? alphabet[(n>>6)&63] : '=';
    output += i+2 < value.size() ? alphabet[n&63] : '=';
  }
  return output;
}
static fs::path xdg(const char* key, const fs::path& fallback) {
  const char* value = std::getenv(key);
  return value && *value && fs::path(value).is_absolute() ? fs::path(value) : fallback;
}
Paths Paths::environment() {
  const char* h = std::getenv("HOME");
  if (!h || !*h) throw std::runtime_error("HOME is not set");
  fs::path home(h);
  auto runtime_root = xdg("XDG_RUNTIME_DIR", fs::path("/tmp") / ("saga-" + std::to_string(getuid())));
  if (std::getenv("XDG_RUNTIME_DIR") && !fs::is_directory(runtime_root)) runtime_root = fs::path("/tmp") / ("saga-" + std::to_string(getuid()));
  return {xdg("XDG_CONFIG_HOME", home / ".config") / "saga",
          xdg("XDG_DATA_HOME", home / ".local/share") / "saga",
          xdg("XDG_STATE_HOME", home / ".local/state") / "saga",
          runtime_root / "saga"};
}
void Paths::create() const { for (auto& p : {config, data, state / "logs", runtime, data / "personas"}) private_dir(p); }
fs::path Paths::persona(std::string_view id) const {
  if (!valid_uuid(id)) throw std::runtime_error("Invalid persona UUID");
  auto p = data / "personas" / id;
  if (!within(p, data / "personas") || fs::is_symlink(p)) throw std::runtime_error("Invalid persona directory");
  return p;
}
Config Config::load(const Paths& paths) {
  Config c;
  if (fs::exists(paths.config / "config.toml")) {
    std::istringstream in(read_file(paths.config / "config.toml", 65536));
    std::string line;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty() || line[0] == '#' || line[0] == '[') continue;
      auto eq = line.find('=');
      if (eq == std::string::npos) throw std::runtime_error("Invalid configuration line");
      auto k = trim(line.substr(0,eq)), v = trim(line.substr(eq+1));
      // Written strings use the JSON/TOML basic-string common subset.
      if (k == "endpoint") c.endpoint = Json::parse(v).get<std::string>();
      else if (k == "api_key") c.api_key = Json::parse(v).get<std::string>();
      else if (k == "model") c.model = Json::parse(v).get<std::string>();
      else if (k == "context_length") c.context_length = Json::parse(v).get<std::uint64_t>();
      else if (k == "generation_reserve") c.generation_reserve = Json::parse(v).get<std::uint64_t>();
      else if (k == "default_max_output_tokens") c.default_max_output_tokens = Json::parse(v).get<std::uint64_t>();
      else if (k == "hard_max_output_tokens") c.hard_max_output_tokens = Json::parse(v).get<std::uint64_t>();
      else if (k == "max_continuations") c.max_continuations = Json::parse(v).get<int>();
      else if (k == "max_generation_retries") c.max_generation_retries = Json::parse(v).get<int>();
      else if (k == "reasoning_budget") c.reasoning_budget = Json::parse(v).get<std::string>();
      else if (k == "reasoning_soft_budget") c.reasoning_soft_budget = Json::parse(v).get<std::uint64_t>();
      else if (k == "reasoning_hard_budget") c.reasoning_hard_budget = Json::parse(v).get<std::uint64_t>();
      else if (k == "reasoning_control") c.reasoning_control = Json::parse(v).get<bool>();
      else if (k == "stream_assistant_text") c.stream_assistant_text = Json::parse(v).get<bool>();
      else if (k == "safety_margin") c.safety_margin = Json::parse(v).get<std::uint64_t>();
      else if (k == "allow_small_context") c.allow_small_context = Json::parse(v).get<bool>();
      else if (k == "insecure_tls") c.insecure_tls = Json::parse(v).get<bool>();
      else if (k == "timeout_seconds") c.timeout_seconds = Json::parse(v).get<int>();
      else if (k == "max_tool_rounds") c.max_tool_rounds = Json::parse(v).get<int>();
      else if (k == "search_engine") c.search_engine = Json::parse(v).get<std::string>();
      else if (k == "search_engines") c.search_engines = Json::parse(v).get<std::vector<std::string>>();
      else if (k == "fourget_manual_instances") c.fourget_manual_instances = Json::parse(v).get<std::vector<std::string>>();
      else if (k == "fourget_prefer_manual") c.fourget_prefer_manual = Json::parse(v).get<bool>();
      else if (k == "fourget_allow_private_instances") c.fourget_allow_private_instances = Json::parse(v).get<bool>();
      else if (k == "duckduckgo_min_request_interval_ms") c.duckduckgo_min_request_interval_ms = Json::parse(v).get<int>();
      else if (k == "duckduckgo_challenge_backoff_ms") c.duckduckgo_challenge_backoff_ms = Json::parse(v).get<int>();
      else if (k == "search_cache_ttl_seconds") c.search_cache_ttl_seconds = Json::parse(v).get<int>();
      else if (k == "search_total_timeout_seconds") c.search_total_timeout_seconds = Json::parse(v).get<int>();
      else if (k == "search_max_engine_attempts") c.search_max_engine_attempts = Json::parse(v).get<int>();
      else if (k == "fourget_directory_ttl_seconds") c.fourget_directory_ttl_seconds = Json::parse(v).get<int>();
      else if (k == "fourget_probe_ttl_seconds") c.fourget_probe_ttl_seconds = Json::parse(v).get<int>();
      else if (k == "fourget_failure_backoff_ms") c.fourget_failure_backoff_ms = Json::parse(v).get<int>();
      else if (k == "fourget_max_instance_attempts") c.fourget_max_instance_attempts = Json::parse(v).get<int>();
      else if (k == "fourget_probe_timeout_seconds") c.fourget_probe_timeout_seconds = Json::parse(v).get<int>();
      else if (k == "fourget_request_timeout_seconds") c.fourget_request_timeout_seconds = Json::parse(v).get<int>();
      else if (k == "web_search_limit") c.web_search_limit = Json::parse(v).get<int>();
      else if (k == "web_read_limit") c.web_read_limit = Json::parse(v).get<int>();
      else if (k == "web_allow_private_network") c.web_allow_private_network = Json::parse(v).get<bool>();
      else if (k == "web_output_tokens") c.web_output_tokens = Json::parse(v).get<size_t>();
      else if (k == "lexical_weight") c.lexical_weight = Json::parse(v).get<double>();
      else if (k == "entity_weight") c.entity_weight = Json::parse(v).get<double>();
      else if (k == "project_weight") c.project_weight = Json::parse(v).get<double>();
      else if (k == "goal_weight") c.goal_weight = Json::parse(v).get<double>();
      else if (k == "salience_weight") c.salience_weight = Json::parse(v).get<double>();
      else if (k == "recency_weight") c.recency_weight = Json::parse(v).get<double>();
      else if (k == "confidence_weight") c.confidence_weight = Json::parse(v).get<double>();
      else if (k == "accessibility_weight") c.accessibility_weight = Json::parse(v).get<double>();
    }
  }
  if (const char* key = std::getenv("SAGA_API_KEY")) c.api_key = key;
  return c;
}
void Config::save(const Paths& p) const {
  std::ostringstream s;
  s << "# Saga: global OpenAI-compatible backend\n";
  Json fields = {{"endpoint",endpoint},{"api_key",api_key},{"model",model},{"context_length",context_length},
    {"generation_reserve",generation_reserve},{"safety_margin",safety_margin},{"allow_small_context",allow_small_context},
    {"default_max_output_tokens",default_max_output_tokens},{"hard_max_output_tokens",hard_max_output_tokens},{"max_continuations",max_continuations},{"max_generation_retries",max_generation_retries},{"reasoning_budget",reasoning_budget},{"reasoning_soft_budget",reasoning_soft_budget},{"reasoning_hard_budget",reasoning_hard_budget},{"reasoning_control",reasoning_control},{"stream_assistant_text",stream_assistant_text},
    {"insecure_tls",insecure_tls},{"timeout_seconds",timeout_seconds},{"max_tool_rounds",max_tool_rounds},
    {"search_engine",search_engine},{"search_engines",search_engines},{"fourget_manual_instances",fourget_manual_instances},{"fourget_prefer_manual",fourget_prefer_manual},{"fourget_allow_private_instances",fourget_allow_private_instances},{"duckduckgo_min_request_interval_ms",duckduckgo_min_request_interval_ms},{"duckduckgo_challenge_backoff_ms",duckduckgo_challenge_backoff_ms},{"search_cache_ttl_seconds",search_cache_ttl_seconds},{"search_total_timeout_seconds",search_total_timeout_seconds},{"search_max_engine_attempts",search_max_engine_attempts},{"fourget_directory_ttl_seconds",fourget_directory_ttl_seconds},{"fourget_probe_ttl_seconds",fourget_probe_ttl_seconds},{"fourget_failure_backoff_ms",fourget_failure_backoff_ms},{"fourget_max_instance_attempts",fourget_max_instance_attempts},{"fourget_probe_timeout_seconds",fourget_probe_timeout_seconds},{"fourget_request_timeout_seconds",fourget_request_timeout_seconds},{"web_search_limit",web_search_limit},{"web_read_limit",web_read_limit},
    {"web_allow_private_network",web_allow_private_network},{"web_output_tokens",web_output_tokens},
    {"lexical_weight",lexical_weight},{"entity_weight",entity_weight},{"project_weight",project_weight},{"goal_weight",goal_weight},
    {"salience_weight",salience_weight},{"recency_weight",recency_weight},{"confidence_weight",confidence_weight},{"accessibility_weight",accessibility_weight}};
  for (auto it = fields.begin(); it != fields.end(); ++it) s << it.key() << " = " << it.value().dump() << '\n';
  atomic_write(p.config / "config.toml", s.str());
}
void Config::validate() const {
  if (endpoint.empty() || model.empty() || context_length == 0) throw std::runtime_error("Complete model setup first");
  if (!allow_small_context && context_length < 65536)
    throw std::runtime_error("Saga requires at least 65,536 tokens of effective context. Detected: " + std::to_string(context_length) + ".");
  if (context_length <= generation_reserve || context_length - generation_reserve <= safety_margin)
    throw std::runtime_error("Generation reserve and safety margin leave no input budget");
  if(hard_max_output_tokens<1 || hard_max_output_tokens>1048576 || default_max_output_tokens>hard_max_output_tokens || max_continuations<0 || max_continuations>16 || max_generation_retries<0 || max_generation_retries>3 || (reasoning_budget!="auto" && reasoning_budget!="fixed" && reasoning_budget!="disabled") || (reasoning_hard_budget && reasoning_soft_budget>reasoning_hard_budget))throw std::runtime_error("Invalid generation limits");
  for (auto &engine : search_engines) (void)make_search_engine(engine);
  if (search_engines.size()>8 || duckduckgo_min_request_interval_ms<1000 || duckduckgo_min_request_interval_ms>60000 || duckduckgo_challenge_backoff_ms<1000 || search_cache_ttl_seconds<1 || search_cache_ttl_seconds>86400 || search_total_timeout_seconds<1 || search_total_timeout_seconds>120 || search_max_engine_attempts<1 || search_max_engine_attempts>8 || fourget_directory_ttl_seconds<60 || fourget_directory_ttl_seconds>604800 || fourget_probe_ttl_seconds<1 || fourget_probe_ttl_seconds>86400 || fourget_failure_backoff_ms<1000 || fourget_failure_backoff_ms>3600000 || fourget_max_instance_attempts<1 || fourget_max_instance_attempts>16 || fourget_probe_timeout_seconds<1 || fourget_probe_timeout_seconds>15 || fourget_request_timeout_seconds<1 || fourget_request_timeout_seconds>60 || fourget_manual_instances.size()>16) throw std::runtime_error("Invalid search policy limits");
  (void)make_search_engine(search_engine); // The provider registry owns engine validation.
  if (web_search_limit < 1 || web_search_limit > 100 || web_read_limit < 1 || web_read_limit > 100 || web_output_tokens<128 || web_output_tokens>4096) throw std::runtime_error("Invalid web research limits");
  if (timeout_seconds < 1 || timeout_seconds > 1800 || max_tool_rounds < 1 || max_tool_rounds > 100)
    throw std::runtime_error("Invalid runtime limits");
}
size_t Config::input_budget() const { validate(); return static_cast<size_t>(context_length - generation_reserve - safety_margin); }
}
