#pragma once
#include <saga/common.hpp>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace saga {
enum class DebugProfile { Off, Debug, Trace, Wire, Forensic };
std::string debug_profile_name(DebugProfile);
DebugProfile debug_profile(std::string_view);
struct DebugOptions {
  DebugProfile profile=DebugProfile::Off;
  fs::path directory;
  bool allow_secrets=false, specified=false;
  size_t rotate_bytes=64*1024*1024, keep=0, queue_bytes=8*1024*1024, ring_events=1000;
  int sample_interval_ms=5000;
  std::vector<std::string> components,exclude;
  Json json() const;
  static DebugOptions from_json(const Json&,const Paths&);
};
bool debug_argument(int argc,char** argv,int& index,DebugOptions&);
std::string debug_sha256(std::string_view);
std::string debug_filename(std::string_view persona,Id session,Id unix_seconds);
class RedactionEngine {
  bool allow_;
  mutable std::mutex mutex_;
  std::vector<std::string> secrets_;
public:
  explicit RedactionEngine(bool allow=false):allow_(allow){}
  void secret(std::string_view);
  Json apply(const Json&) const;
};
class DebugLogger {
  struct Pending {Json event;size_t bytes;bool flush;};
  DebugOptions options_;
  RedactionEngine redact_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Pending> queue_;
  std::deque<std::string> ring_;
  std::jthread writer_;
  Json context_=Json::object(),manifest_=Json::object(),previous_context_=Json::object();
  fs::path bundle_,log_,follow_;
  int fd_=-1,signal_fd_=-1,signal_slot_=-1;
  size_t reported_drops_=0;
  size_t queued_=0,part_bytes_=0,part_=0,written_=0,dropped_=0,bytes_=0,sidecar_bytes_=0,errors_=0;
  std::uint64_t sequence_=0,span_sequence_=0;
  bool stopping_=false,failed_=false,busy_=false;
  std::chrono::steady_clock::time_point started_,sampled_;
  void open_session(std::string_view,Id,const Json&);
  void run();
  void write_event(Json,bool);
  void write_manifest();
  void publish_follow(const fs::path& target);
  void fail() noexcept;
public:
  explicit DebugLogger(DebugOptions,fs::path follow_path={});
  ~DebugLogger();
  DebugLogger(const DebugLogger&)=delete;
  bool enabled(DebugProfile minimum=DebugProfile::Debug) const noexcept;
  void session(std::string_view persona,Id id,const Json& context=Json::object()) noexcept;
  void context(const Json&);
  Json context() const;
  void replace_context(const Json&);
  void secret(std::string_view value){redact_.secret(value);}
  void emit(std::string_view component,std::string_view event,Json fields=Json::object(),
            DebugProfile minimum=DebugProfile::Debug,std::string_view severity="INFO",bool critical=true) noexcept;
  void observe(std::string_view event,const Json&) noexcept;
  void flush() noexcept;
  void crash(std::string_view reason) noexcept;
  void sample() noexcept;
  void record_context(const Json& messages,const Json& tools,Json fields);
  fs::path path() const {std::lock_guard lock(mutex_);return failed_?fs::path():log_;}
  fs::path follow_path() const {std::lock_guard lock(mutex_);return failed_?fs::path():follow_;}
  const DebugOptions& options() const {return options_;}
  std::string next_span();
  static Json build_metadata();
};
DebugLogger* debug_logger() noexcept;
class TraceContext {
  DebugLogger* logger_;
  Json previous_;
public:
  explicit TraceContext(const Json& fields);
  ~TraceContext();
};
class DebugScope {
  DebugLogger* previous_;
public:
  explicit DebugScope(DebugLogger* logger=nullptr) noexcept;
  ~DebugScope();
  void bind(DebugLogger*) noexcept;
};
void trace(std::string_view component,std::string_view event,Json fields=Json::object(),
           DebugProfile minimum=DebugProfile::Debug,std::string_view severity="INFO",bool critical=true) noexcept;
class TraceSpan {
  DebugLogger* logger_;
  std::string id_,parent_,component_,name_;
  std::chrono::steady_clock::time_point started_;
  int exceptions_;
  DebugProfile minimum_;
public:
  TraceSpan(std::string_view component,std::string_view name,Json fields=Json::object(),DebugProfile minimum=DebugProfile::Trace);
  ~TraceSpan();
};
Json debug_context_accounting(const Json& messages,const Json& tools);
void install_debug_crash_handlers();
}
