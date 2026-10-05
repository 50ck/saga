#include <saga/tools.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <map>
#include <poll.h>
#include <signal.h>
#include <set>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace saga {
Json ProcessResult::json() const { return {{"exit_code",exit_code},{"timed_out",timed_out},{"stdout",utf8_text(out)},{"stderr",utf8_text(err)},
  {"stdout_size",stdout_size},{"stderr_size",stderr_size},{"stdout_truncated",stdout_truncated},{"stderr_truncated",stderr_truncated}}; }
Json Tools::permissions(const std::optional<std::string>& mode) {
  if (mode) {
    if (*mode != "ask_always" && *mode != "always_approve" && *mode != "host_ask" && *mode != "host_always")
      throw std::runtime_error("Choose guarded host (ask/always) or unrestricted host (host-ask/host-always)");
    p_.db->transaction([&]{
      p_.db->exec("UPDATE runtime_settings SET value=?,updated_at=? WHERE key='host_permissions'",{*mode,now()});
      p_.db->event("permissions.user_changed",{{"host_execution",*mode}},p_.session);
    });
  }
  auto value=p_.db->query("SELECT value FROM runtime_settings WHERE key='host_permissions'")[0]["value"].get<std::string>();
  bool unrestricted=value == "host_ask" || value == "host_always";
  bool automatic=value == "always_approve" || value == "host_always";
  std::string label=value == "host_always" ? "Unrestricted host without approval (DANGEROUS)" : value == "host_ask" ? "Ask for unrestricted host operations" : automatic ? "Always approve guarded host" : "Ask for guarded host operations";
  return {{"mode",value},{"label",label},{"scope","persona"},{"host_restrictions",unrestricted ? "none" : "private_storage"},{"approval_required",!automatic}};
}
bool shell_sandbox_available() {
#ifdef __linux__
  return syscall(SYS_landlock_create_ruleset,nullptr,0,LANDLOCK_CREATE_RULESET_VERSION) >= 3;
#else
  return false;
#endif
}
static bool restrict_child(const fs::path& cwd,const fs::path& scratch,bool host,const std::vector<fs::path>& private_roots) {
  int abi = static_cast<int>(syscall(SYS_landlock_create_ruleset,nullptr,0,LANDLOCK_CREATE_RULESET_VERSION));
  if (abi < 3) return false;
  std::uint64_t read = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
  std::uint64_t write = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
    LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE;
  // Handle every supported filesystem right; device/FIFO/socket creation denied.
  std::uint64_t handled = (1ULL << 15)-1;
  if (abi >= 5) handled |= (1ULL << 15); // IOCTL_DEV
  landlock_ruleset_attr rules{}; rules.handled_access_fs = handled;
  int fd = static_cast<int>(syscall(SYS_landlock_create_ruleset,&rules,sizeof rules,0));
  if (fd < 0) return false;
  auto allow = [&](const fs::path& path,std::uint64_t rights) {
    int parent = open(path.c_str(),O_PATH | O_CLOEXEC);
    if (parent < 0) return !fs::exists(path);
    struct stat st{}; fstat(parent,&st);
    if (!S_ISDIR(st.st_mode)) rights &= LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_TRUNCATE;
    landlock_path_beneath_attr attr{}; attr.allowed_access = rights; attr.parent_fd = parent;
    bool ok = syscall(SYS_landlock_add_rule,fd,LANDLOCK_RULE_PATH_BENEATH,&attr,0) == 0;
    close(parent); return ok;
  };
  bool ok = true;
  if (host) {
    if (private_roots.empty()) { close(fd); return false; }
    std::vector<fs::path> blocked{fs::path("/proc")};
    for (auto& path : private_roots) blocked.push_back(fs::weakly_canonical(path));
    std::function<void(const fs::path&)> permit = [&](const fs::path& path){
      auto resolved=fs::weakly_canonical(path);
      for (auto& secret : blocked) if (within(resolved,secret)) return;
      bool ancestor=std::any_of(blocked.begin(),blocked.end(),[&](auto& secret){ return within(secret,resolved); });
      if (!ancestor) { ok=allow(path,handled) && ok; return; }
      if (!fs::is_directory(path) || fs::is_symlink(path)) return;
      for (auto& child : fs::directory_iterator(path,fs::directory_options::skip_permission_denied)) permit(child.path());
    };
    permit("/");
  } else {
    for (const auto* path : {"/usr","/bin","/sbin","/lib","/lib64","/etc/ld.so.cache","/etc/localtime","/etc/machine-id","/var/lib/dbus/machine-id","/dev/urandom"}) ok = allow(path,read) && ok;
    ok = allow("/dev/null",LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE) && ok;
    ok = allow(cwd,read | write) && allow(scratch,read | write) && ok;
  }
  if (prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) != 0 || !ok || syscall(SYS_landlock_restrict_self,fd,0) != 0) { close(fd); return false; }
  close(fd);
  // Shell children cannot contact the control socket/model, escape through
  // process handles, create namespaces, mount filesystems, or bypass Landlock.
  std::vector<sock_filter> filter;
  auto stmt = [&](unsigned short code,unsigned value){ filter.push_back(BPF_STMT(code,value)); };
  auto jump = [&](unsigned short code,unsigned value,unsigned char yes,unsigned char no){ filter.push_back(BPF_JUMP(code,value,yes,no)); };
  stmt(BPF_LD | BPF_W | BPF_ABS,offsetof(seccomp_data,arch));
#if defined(__x86_64__)
  jump(BPF_JMP | BPF_JEQ | BPF_K,AUDIT_ARCH_X86_64,1,0);
#elif defined(__aarch64__)
  jump(BPF_JMP | BPF_JEQ | BPF_K,AUDIT_ARCH_AARCH64,1,0);
#else
  return false;
#endif
  stmt(BPF_RET | BPF_K,SECCOMP_RET_KILL_PROCESS);
  stmt(BPF_LD | BPF_W | BPF_ABS,offsetof(seccomp_data,nr));
#if defined(__x86_64__)
  // Deny x32 ABI, whose syscall numbers would evade the native deny list.
  jump(BPF_JMP | BPF_JGE | BPF_K,0x40000000,0,1);
  stmt(BPF_RET | BPF_K,SECCOMP_RET_ERRNO | EPERM);
#endif
  for (int number : {SYS_socket,SYS_connect,SYS_bind,SYS_ptrace,SYS_process_vm_readv,SYS_process_vm_writev,
                     SYS_mount,SYS_umount2,SYS_unshare,SYS_setns,SYS_open_by_handle_at,SYS_bpf,
                     SYS_io_uring_setup,SYS_keyctl,SYS_add_key,SYS_request_key,SYS_pidfd_getfd,
                     SYS_kill,SYS_tkill,SYS_tgkill,SYS_pidfd_send_signal}) {
    if (host && (number == SYS_socket || number == SYS_connect || number == SYS_bind)) continue;
    jump(BPF_JMP | BPF_JEQ | BPF_K,static_cast<unsigned>(number),0,1);
    stmt(BPF_RET | BPF_K,SECCOMP_RET_ERRNO | EPERM);
  }
  stmt(BPF_RET | BPF_K,SECCOMP_RET_ALLOW);
  sock_fprog program{static_cast<unsigned short>(filter.size()),filter.data()};
  return prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program) == 0;
}
ProcessResult run_process(const fs::path& cwd,const fs::path& scratch,const std::vector<std::string>& argv,int timeout,bool host,const std::vector<fs::path>& private_roots,const Json& environment,bool unrestricted,const std::function<void()>& service) {
  if (unrestricted && !host) throw std::runtime_error("Unrestricted execution requires host mode");
  if (argv.empty()) throw std::runtime_error("A process requires a command");
  if (!unrestricted && !shell_sandbox_available()) throw std::runtime_error("Shell execution requires Linux Landlock ABI >= 3; unrestricted host execution remains available");
  private_dir(scratch);
  int out[2],err[2];
  if (pipe2(out,O_CLOEXEC) != 0) throw std::runtime_error("Cannot create process pipe");
  if (pipe2(err,O_CLOEXEC) != 0) { close(out[0]); close(out[1]); throw std::runtime_error("Cannot create process pipe"); }
  pid_t pid = fork();
  if (pid == -1) { for (int fd : {out[0],out[1],err[0],err[1]}) close(fd); throw std::runtime_error("Cannot start process"); }
  if (pid == 0) {
    setpgid(0,0);
    dup2(out[1],STDOUT_FILENO); dup2(err[1],STDERR_FILENO);
    int null = open("/dev/null",O_RDONLY); if (null >= 0) { dup2(null,STDIN_FILENO); close(null); }
    syscall(SYS_close_range,3,~0U,0);
    if (!unrestricted) {
      rlimit cpu{static_cast<rlim_t>(timeout+1),static_cast<rlim_t>(timeout+1)}; setrlimit(RLIMIT_CPU,&cpu);
      rlimit files{128,128}; setrlimit(RLIMIT_NOFILE,&files);
      rlimit size{64*1024*1024,64*1024*1024}; setrlimit(RLIMIT_FSIZE,&size);
      rlimit core{0,0}; setrlimit(RLIMIT_CORE,&core);
    }
    try {
      if (chdir(cwd.c_str()) || (!unrestricted && !restrict_child(cwd,scratch,host,private_roots))) { const char* msg = "OS isolation unavailable; execution refused\n"; write(2,msg,strlen(msg)); _exit(126); }
    } catch (...) { const char* msg="Cannot establish private storage isolation\n"; write(2,msg,strlen(msg)); _exit(126); }
    std::vector<char*> args;
    for (auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    std::map<std::string,std::string> values{{"PATH","/usr/bin:/bin:/usr/sbin:/sbin"},{"LANG","C.UTF-8"},{"HOME",scratch.string()},{"TMPDIR",scratch.string()}};
    if (unrestricted) {
      values.clear();
      for (char** item=::environ; *item; ++item) {
        std::string variable(*item); auto equal=variable.find('=');
        if (equal != std::string::npos) values[variable.substr(0,equal)]=variable.substr(equal+1);
      }
    }
    if (host) for (auto* key : {"PATH","LANG","LC_ALL","HOME","USER","SHELL","DISPLAY","WAYLAND_DISPLAY","DBUS_SESSION_BUS_ADDRESS","XDG_RUNTIME_DIR","XAUTHORITY","SSH_AUTH_SOCK"}) {
      if (const char* v=std::getenv(key)) values[key]=v;
      if (environment.contains(key) && environment[key].is_string()) values[key]=environment[key].get<std::string>();
    }
    std::vector<std::string> strings; for (auto& [key,value] : values) strings.push_back(key+"="+value);
    std::vector<char*> env; for (auto& value : strings) env.push_back(value.data()); env.push_back(nullptr);
    execve(argv[0].c_str(),args.data(),env.data()); _exit(127);
  }
  setpgid(pid,pid);
  close(out[1]); close(err[1]);
  fcntl(out[0],F_SETFL,O_NONBLOCK); fcntl(err[0],F_SETFL,O_NONBLOCK);
  ProcessResult result; auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(timeout);
  std::array<pollfd,2> fds{{{out[0],POLLIN,0},{err[0],POLLIN,0}}};
  bool exited = false; int status = 0;
  while (!exited || fds[0].fd >= 0 || fds[1].fd >= 0) {
    try { if (service) service(); }
    catch (...) {
      kill(-pid,SIGKILL);
      if (!exited) { while (waitpid(pid,&status,0) < 0 && errno == EINTR) {} }
      for (auto& pipe : fds) if (pipe.fd >= 0) close(pipe.fd);
      throw;
    }
    if (std::chrono::steady_clock::now() >= deadline) { result.timed_out = true; kill(-pid,SIGKILL); }
    poll(fds.data(),fds.size(),25);
    for (size_t i = 0; i < fds.size(); ++i) {
      if (fds[i].fd < 0) continue;
      char buffer[8192]; ssize_t n;
      while ((n = read(fds[i].fd,buffer,sizeof buffer)) > 0) {
        auto& target = i == 0 ? result.out : result.err;
        auto& total = i == 0 ? result.stdout_size : result.stderr_size;
        auto& truncated = i == 0 ? result.stdout_truncated : result.stderr_truncated;
        total += static_cast<size_t>(n); truncated = truncated || total > 256*1024;
        if (target.size() < 256*1024) target.append(buffer,std::min<size_t>(static_cast<size_t>(n),256*1024-target.size()));
      }
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { close(fds[i].fd); fds[i].fd = -1; }
    }
    if (!exited) {
      auto w = waitpid(pid,&status,WNOHANG);
      exited = w == pid;
      if (exited) kill(-pid,SIGKILL); // no detached persistent automation
    }
    if (result.timed_out && !exited) { while (waitpid(pid,&status,0) < 0 && errno == EINTR) {} exited = true; }
  }
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  return result;
}
static Json str() { return {{"type","string"}}; }
static Json integer() { return {{"type","integer"}}; }
static Json boolean() { return {{"type","boolean"}}; }
Json Tools::definitions() {
  Json tools = Json::array();
  auto add = [&](std::string name,std::string desc,Json props,Json req){ tools.push_back(function_tool(std::move(name),std::move(desc),std::move(props),std::move(req))); };
  for (auto* name : {"remember","know","know_how","recall_artifact","inspect_self"})
    add(name,"Retrieve persistent " + std::string(name) + " evidence. One deep recall escalation is available.",{{"query",str()},{"depth",{{"type","string"},{"enum",{"normal","deep"}}}}},{"query"});
  add("inspect_open_loops","Retrieve unresolved work",Json::object(),{});
  add("recall_observation","Retrieve an archived observation by its provenance event ID; offset and limit page the original JSON text",{{"event_id",integer()},{"offset",integer()},{"limit",integer()}},{"event_id"});
  add("observe_environment","Safely inspect the current project, git state, clock and machine",Json::object(),{});
  add("file_read","Read a file in the active project or this identity's artifacts",{{"path",str()}},{"path"});
  add("file_write","Write a project/artifact file without approval and track its history",{{"path",str()},{"content",str()},{"description",str()}},{"path","content","description"});
  add("shell_exec","Run a shell command. execution=sandbox (default) is automatic, project/scratch only, no network. execution=host follows /permissions: 1/2 use private-storage guards; 3/4 run as the daemon's OS user without Saga isolation, asking approval only in 3. Use host for desktop DBus/notify-send, tmux, network or files outside the project. Unrestricted host can access all files and processes accessible to that user. timeout <=120 seconds; no detached processes.",{{"command",str()},{"timeout_seconds",integer()},{"execution",{{"type","string"},{"enum",{"sandbox","host"}}}}},{"command"});
  add("task_create","Create an explicit task and required proof obligations",{{"title",str()},{"objective",str()},{"risk",{{"type","string"},{"enum",{"low","medium","high"}}}},{"domain",str()},{"checks",{{"type","array"},{"items",str()}}}},{"title","objective","risk","checks"});
  add("task_update","Select or update a task; completion is gated on evidence and required checks",{{"id",integer()},{"status",{{"type","string"},{"enum",{"active","blocked","verifying","completed","abandoned"}}}}},{"id","status"});
  add("task_add_check","Refine the current task's definition of done with a specific proof obligation",{{"description",str()}},{"description"});
  add("check_resolve","Resolve a proof obligation using observed tool output or an explicit user confirmation",{{"check_id",integer()},{"source_event_id",integer()},{"passed",boolean()},{"explanation",str()}},{"check_id","source_event_id","passed","explanation"});
  add("record_assumption","Record an unverified assumption",{{"statement",str()},{"impact",{{"type","string"},{"enum",{"low","high"}}}},{"verification_method",str()}},{"statement","impact","verification_method"});
  add("resolve_assumption","Resolve an assumption using observed evidence",{{"id",integer()},{"source_event_id",integer()},{"confirmed",boolean()}},{"id","source_event_id","confirmed"});
  add("record_hypothesis","Store a hypothesis separately from facts",{{"statement",str()}},{"statement"});
  add("resolve_hypothesis","Confirm or reject a hypothesis using evidence",{{"id",integer()},{"source_event_id",integer()},{"confirmed",boolean()}},{"id","source_event_id","confirmed"});
  add("record_prediction","Predict a diagnostic outcome before acting; model confidence is weak metadata",{{"statement",str()},{"expected_outcome",str()},{"confidence",{{"type","number"}}},{"domain",str()}},{"statement","expected_outcome","confidence","domain"});
  add("record_observation","Compare a prediction with actual tool output",{{"prediction_id",integer()},{"source_event_id",integer()},{"statement",str()},{"matched",boolean()}},{"prediction_id","source_event_id","statement","matched"});
  add("learn_fact","Learn a semantic fact with provenance; use correction=true for explicit user correction",{{"subject",str()},{"predicate",str()},{"object",str()},{"source_event_id",integer()},{"correction",boolean()},{"scope",{{"type","string"},{"enum",{"global","project"}}}}},{"subject","predicate","object","source_event_id"});
  add("record_belief","Store an uncertain belief separately from verified facts",{{"statement",str()},{"domain",str()},{"source_event_id",integer()},{"reasoning_confidence",{{"type","number"}}}},{"statement","domain","source_event_id"});
  add("record_evidence","Attach observed support or contradiction to an existing epistemic target; confidence is recomputed by the runtime",{{"target_type",{{"type","string"},{"enum",{"fact","belief","task","hypothesis","assumption"}}}},{"target_id",integer()},{"source_event_id",integer()},{"support",boolean()},{"description",str()}},{"target_type","target_id","source_event_id","support","description"});
  add("record_goal","Persist a goal",{{"scope",{{"type","string"},{"enum",{"session","project","long_term","self_improvement"}}}},{"description",str()}},{"scope","description"});
  add("record_decision","Preserve a project decision and its rationale",{{"decision",str()},{"rationale",str()}},{"decision","rationale"});
  add("complete_goal","Complete a goal only when its associated work is finished",{{"id",integer()}},{"id"});
  add("record_commitment","Persist an explicit promise",{{"description",str()},{"due_condition",str()}},{"description","due_condition"});
  add("complete_commitment","Record fulfillment of an existing commitment",{{"id",integer()},{"source_event_id",integer()}},{"id","source_event_id"});
  add("record_open_loop","Persist unfinished work or a prospective reminder",{{"kind",str()},{"description",str()},{"priority",{{"type","number"}}}},{"kind","description","priority"});
  add("close_open_loop","Close an existing loop with evidence",{{"id",integer()},{"source_event_id",integer()}},{"id","source_event_id"});
  add("record_intention","Persist WHEN/THEN prospective memory",{{"trigger_type",{{"type","string"},{"enum",{"project","task","time"}}}},{"trigger",str()},{"reminder",str()},{"reason",str()}},{"trigger_type","trigger","reminder","reason"});
  add("record_curiosity","Store a question without interrupting the user",{{"question",str()}},{"question"});
  add("praxis_candidate","Store a procedure as an untrusted candidate",{{"name",str()},{"scope",{{"type","string"},{"enum",{"global","project","domain"}}}},{"domain",str()},{"trigger",str()},{"procedure",str()},{"rationale",str()},{"limitations",str()}},{"name","scope","trigger","procedure","rationale"});
  add("praxis_outcome","Record a procedure's outcome using task evidence",{{"id",integer()},{"task_id",integer()},{"success",boolean()}},{"id","task_id","success"});
  add("record_failed_strategy","Remember the conditions of a failed approach",{{"context",str()},{"strategy",str()},{"failure",str()},{"lesson",str()}},{"context","strategy","failure","lesson"});
  add("record_relationship","Record an explicit user preference or repeated interaction pattern with provenance",{{"belief",str()},{"kind",{{"type","string"},{"enum",{"explicit","inferred"}}}},{"source_event_ids",{{"type","array"},{"items",integer()}}}},{"belief","kind","source_event_ids"});
  add("develop_trait","Record a tendency supported by at least three completed task events",{{"trait",str()},{"source_event_ids",{{"type","array"},{"items",integer()}}}},{"trait","source_event_ids"});
  add("compile_skill","Compile habitual praxis into a tool macro. Host workflows follow host permissions; sandbox workflows are automatic.",{{"praxis_id",integer()},{"name",str()},{"steps",{{"type","array"},{"items",{{"type","object"},{"properties",{{"tool",str()},{"arguments",{{"type","object"}}}}},{"required",{"tool","arguments"}},{"additionalProperties",false}}}}}},{"praxis_id","name","steps"});
  add("run_skill","Run an approved compiled macro; each action passes its own gate",{{"id",integer()}},{"id"});
  return tools;
}
void Tools::validate(const Json& args,const Json& schema) {
  auto type = schema.value("type","");
  bool valid = type.empty() || (type == "object" && args.is_object()) || (type == "string" && args.is_string()) ||
    (type == "integer" && args.is_number_integer()) || (type == "number" && args.is_number()) ||
    (type == "boolean" && args.is_boolean()) || (type == "array" && args.is_array());
  if (!valid) throw std::runtime_error("Tool argument has wrong type");
  if (schema.contains("enum") && std::find(schema["enum"].begin(),schema["enum"].end(),args) == schema["enum"].end()) throw std::runtime_error("Tool argument is outside allowed values");
  if (args.is_string() && args.get_ref<const std::string&>().size() > 512*1024) throw std::runtime_error("Tool argument too large");
  if (args.is_array()) {
    if (args.size() > 64) throw std::runtime_error("Too many tool arguments");
    if (schema.contains("items")) for (auto& arg : args) validate(arg,schema["items"]);
  }
  if (args.is_object()) {
    for (auto& key : schema.value("required",Json::array())) if (!args.contains(key.get<std::string>())) throw std::runtime_error("Missing required tool argument: " + key.get<std::string>());
    auto props = schema.value("properties",Json::object());
    for (auto it = args.begin(); it != args.end(); ++it) {
      if (props.contains(it.key())) validate(it.value(),props[it.key()]);
      else if (schema.contains("additionalProperties") && schema["additionalProperties"] == false) throw std::runtime_error("Unknown tool argument: " + it.key());
    }
  }
}
fs::path Tools::safe_path(const std::string& value,bool writing) const {
  if (value.empty() || value.find('\0') != std::string::npos) throw std::runtime_error("Invalid file path");
  fs::path path(value);
  if (!path.is_absolute()) path = p_.project_root / path;
  auto normalized = path.lexically_normal();
  bool allowed = within(normalized,p_.project_root) || within(normalized,p_.directory / "artifacts");
  if (!allowed) throw std::runtime_error("Path lies outside the active project and artifact area");
  fs::path component;
  for (auto& part : normalized) { component /= part; if (fs::is_symlink(component)) throw std::runtime_error("Symlink paths are not permitted by file tools"); }
  struct stat st{};
  if (!lstat(normalized.c_str(),&st) && (!S_ISREG(st.st_mode) || st.st_nlink > 1)) throw std::runtime_error("Only regular files without hard links are permitted");
  if (writing && normalized.filename() == "SOUL.md") throw std::runtime_error("SOUL.md requires an explicit user editor command");
  return normalized;
}
Json Tools::environment() {
  Json result = {{"cwd",p_.project_root.string()},{"clock_ms",now()},{"shell_sandbox",shell_sandbox_available()}};
  char host[256]{}; if (gethostname(host,sizeof host-1) == 0) result["machine"] = host;
  result["processes"] = Json::array();
  for (auto& entry : fs::directory_iterator("/proc",fs::directory_options::skip_permission_denied)) {
    auto id = entry.path().filename().string();
    if (id.empty() || !std::all_of(id.begin(),id.end(),[](unsigned char c){ return std::isdigit(c); })) continue;
    std::error_code ec;
    auto process_cwd = fs::read_symlink(entry.path() / "cwd",ec); if (ec || !within(process_cwd,p_.project_root)) continue;
    auto executable = fs::read_symlink(entry.path() / "exe",ec); if (ec) continue;
    if (executable.filename() == "saga" || executable.filename() == "sagad") continue;
    result["processes"].push_back({{"pid",id},{"executable",executable.filename().string()},{"cwd",process_cwd.string()}});
    if (result["processes"].size() >= 32) break;
  }
  auto latest = p_.db->query("SELECT status FROM model_calls ORDER BY id DESC LIMIT 1");
  result["model_connectivity"] = latest.empty() ? "not_observed" : latest[0]["status"] == "completed" ? "last_call_succeeded" : "last_call_failed";
  auto scratch = p_.directory / "cache/scratch";
  if (fs::exists(p_.project_root / ".git") && shell_sandbox_available()) {
    auto git = [&](std::vector<std::string> args) {
      args.insert(args.begin(),{"/usr/bin/git","--no-optional-locks","-c","core.hooksPath=/dev/null","-c","core.fsmonitor=false"});
      auto r = run_process(p_.project_root,scratch,args,5); return r.exit_code == 0 ? trim(r.out) : std::string();
    };
    result["git_root"] = git({"rev-parse","--show-toplevel"});
    result["branch"] = git({"branch","--show-current"});
    result["status"] = git({"status","--porcelain"});
    auto remote = git({"config","--get","remote.origin.url"});
    auto at = remote.find('@'), scheme = remote.find("://");
    if (at != std::string::npos && scheme != std::string::npos && at > scheme) remote.erase(scheme+3,at-(scheme+3)+1);
    result["remote"] = remote;
    // No author/committer emails in project observations.
    result["commits"] = git({"log","-5","--format=%h %s","--no-show-signature"});
  }
  return result;
}
Json Tools::execute(const std::string& name,const Json& args,Emit emit) {
  if (service_) service_(); // Apply queued permissions before the action gate.
  auto definitions_list = definitions();
  auto it = std::find_if(definitions_list.begin(),definitions_list.end(),[&](auto& d){ return d["function"]["name"] == name; });
  if (it == definitions_list.end()) return {{"error","Unknown tool"}};
  try { validate(args,(*it)["function"]["parameters"]); } catch (const std::exception& e) { return {{"error",e.what()}}; }
  Id started = now();
  Id run = p_.db->exec("INSERT INTO tool_runs(session_id,task_id,tool,arguments_json,started_at,status) VALUES(?,?,?,?,?,'running')",{p_.session,p_.task ? Json(p_.task) : Json(),name,args.dump(),started});
  p_.db->event("tool.started",{{"run_id",run},{"tool",name},{"arguments",args}},p_.session,p_.task);
  if (emit) emit("tool.started",{{"run_id",run},{"tool",name},{"arguments",args}});
  Json result;
  try { result = dispatch(name,args); } catch (const std::exception& e) { result = {{"error",e.what()}}; }
  bool failed = result.contains("error") || (result.contains("exit_code") && result["exit_code"] != 0);
  p_.db->exec("UPDATE internal_state SET frustration=max(0,min(1,frustration+?)),confidence=max(0.05,min(0.95,confidence+?)),engagement=min(1,engagement+0.02),satisfaction=max(0,min(1,satisfaction+?)),updated_at=? WHERE id=1",{failed ? 0.15 : -0.03,failed ? -0.08 : 0.02,failed ? -0.1 : 0.03,now()});
  auto type = failed ? "tool.failed" : "tool.completed";
  p_.db->exec("UPDATE tool_runs SET duration_ms=?,status=?,exit_code=?,stdout_size=?,stderr_size=?,error_type=?,result_hash=? WHERE id=?",
    {now()-started,failed ? "failed" : "completed",result.value("exit_code",Json()),result.value("stdout_size",result.value("stdout",std::string()).size()),result.value("stderr_size",result.value("stderr",std::string()).size()),failed ? Json(result.value("error",std::string("nonzero_exit"))) : Json(),digest(result.dump()),run});
  Id ev = p_.db->event(type,{{"run_id",run},{"tool",name},{"result",result}},p_.session,p_.task);
  result["source_event_id"] = ev;
  if (p_.task) memory_.evidence("task",p_.task,"tool output",!failed,ev,0.2,0.5);
  if (emit) emit(type,{{"run_id",run},{"tool",name},{"result",result}});
  return result;
}
Json Tools::dispatch(const std::string& name,const Json& a) {
  auto db = p_.db.get();
  auto source = [&](Id event,bool user_only = false) {
    auto rows = db->query("SELECT * FROM events WHERE id=? AND session_id=?",{event,p_.session});
    if (rows.empty() || (user_only ? rows[0]["type"] != "user.message" : rows[0]["type"] != "user.message" && rows[0]["type"] != "tool.completed" && rows[0]["type"] != "tool.failed"))
      throw std::runtime_error("An observed tool result or user message in this session is required");
    if (!user_only && rows[0]["type"] != "user.message") {
      auto payload = Json::parse(rows[0]["payload_json"].get<std::string>());
      auto tool = payload.value("tool","");
      if (tool != "shell_exec" && tool != "file_read" && tool != "file_write" && tool != "observe_environment")
        throw std::runtime_error("Cognitive tool acknowledgements are not external observations");
    }
    return rows[0];
  };
  auto active_task = [&]{ if (!p_.task) throw std::runtime_error("Create or select a task first"); };
  auto event = [&](std::string type,Json data){ return db->event(type,data,p_.session,p_.task); };
  if (name == "remember" || name == "know" || name == "know_how" || name == "recall_artifact" || name == "inspect_self" || name == "inspect_open_loops") return memory_.search(name,a.value("query",""),a.value("depth","normal") == "deep");
  if (name == "recall_observation") {
    auto rows = db->query("SELECT id,ts,type,payload_json FROM events WHERE id=? AND type IN ('tool.completed','tool.failed','observation.recorded')",{a["event_id"]});
    if (rows.empty()) throw std::runtime_error("No archived observation at this provenance ID");
    Id offset = a.value("offset",Id(0)),limit = a.value("limit",Id(12000));
    if (offset < 0 || limit < 1 || limit > 12000) throw std::runtime_error("Invalid observation page; limit must be between 1 and 12000 bytes");
    auto text = rows[0]["payload_json"].get<std::string>();
    size_t begin = std::min(static_cast<size_t>(offset),text.size()),end = std::min(begin+static_cast<size_t>(limit),text.size());
    // Align excerpts to UTF-8 boundaries without modifying archived bytes.
    while (begin > 0 && begin < text.size() && (static_cast<unsigned char>(text[begin]) & 0xc0) == 0x80) --begin;
    while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) ++end;
    return {{"event_id",rows[0]["id"]},{"type",rows[0]["type"]},{"ts",rows[0]["ts"]},{"content",text.substr(begin,end-begin)},
      {"offset",begin},{"next_offset",end},{"total_bytes",text.size()},{"complete",end == text.size()},{"hash",digest(text)}};
  }
  if (name == "observe_environment") return environment();
  if (name == "file_read") {
    auto path = safe_path(a["path"],false); auto contents = read_file(path,512*1024);
    bool binary = contents.find('\0') != std::string::npos;
    if (!binary) { try { Json(contents).dump(); } catch (const Json::exception&) { binary = true; } }
    return {{"path",path.string()},{"content",binary ? base64(contents) : contents},{"encoding",binary ? "base64" : "utf-8"},{"bytes",contents.size()},{"hash",digest(contents)}};
  }
  if (name == "file_write") {
    auto path = safe_path(a["path"],true);
    path = safe_path(a["path"],true);
    fs::create_directories(path.parent_path());
    auto content = a["content"].get<std::string>(); atomic_write(path,content);
    memory_.artifact(path,a["description"],content); return {{"path",path.string()},{"bytes",content.size()},{"hash",digest(content)}};
  }
  if (name == "shell_exec") {
    bool host=a.value("execution","sandbox") == "host";
    auto policy=permissions(); bool unrestricted=host && policy["host_restrictions"] == "none";
    if (host && policy["approval_required"].get<bool>() && !approve_(name,a)) return {{"error","User declined host execution"}};
    if (host) db->event("host.action_authorized",{{"tool",name},{"arguments",a},{"mode",policy["mode"]},{"unrestricted",unrestricted}},p_.session,p_.task);
    using Snapshot = std::map<fs::path,std::pair<fs::file_time_type,std::uintmax_t>>;
    auto snapshot = [&](bool security_check) {
      Snapshot files;
      for (auto it = fs::recursive_directory_iterator(p_.project_root,fs::directory_options::skip_permission_denied); it != fs::recursive_directory_iterator(); ++it) {
        auto& item = *it;
        if (security_check && item.is_symlink() && within(item.path(),p_.directory.parent_path().parent_path())) throw std::runtime_error("Workspace aliases private Saga storage");
        if (security_check && item.is_regular_file() && fs::hard_link_count(item.path()) > 1) throw std::runtime_error("Shell workspace contains hard-linked files");
        bool excluded = false;
        for (auto& part : item.path().lexically_relative(p_.project_root)) if (part == ".git" || part == "build" || part == "node_modules" || part == ".cache") excluded = true;
        if (excluded) { if (!security_check && item.is_directory()) it.disable_recursion_pending(); continue; }
        if (item.is_regular_file() && !item.is_symlink() && files.size() < 10000) files[item.path()] = {item.last_write_time(),item.file_size()};
        // ponytail: bounded artifact observation; explicit file tools track large workspaces precisely.
        if (files.size() >= 10000 && !security_check) break;
      }
      return files;
    };
    auto before = snapshot(!unrestricted);
    auto scratch=host ? fs::temp_directory_path()/("saga-host-"+uuid()) : p_.directory/"cache/scratch";
    struct Cleanup { fs::path path; ~Cleanup() { if (!path.empty()) { std::error_code ec; fs::remove_all(path,ec); } } } cleanup{host ? scratch : fs::path()};
    auto r = run_process(p_.project_root,scratch,{"/bin/sh","-c",a["command"]},std::clamp(a.value("timeout_seconds",30),1,120),host,p_.private_roots,p_.host_environment,unrestricted,service_);
    auto after = snapshot(false); size_t tracked = 0;
    auto task = db->query("SELECT title FROM tasks WHERE id=?",{p_.task});
    auto description = task.empty() ? "File observed after shell action" : task[0]["title"].get<std::string>();
    for (auto& [path,state] : after) if (!before.contains(path) || before[path] != state) {
      if (tracked >= 256) break;
      try {
        auto existing = db->query("SELECT description FROM artifacts WHERE project_id=? AND path=?",{p_.project,path.string()});
        auto label = existing.empty() ? description : existing[0]["description"].get<std::string>();
        if (state.second <= 8*1024*1024) memory_.artifact(path,label,read_file(safe_path(path.string(),false),8*1024*1024));
        else memory_.artifact(path,label,std::to_string(state.second)+":"+std::to_string(state.first.time_since_epoch().count()),false);
        ++tracked;
      } catch (const std::exception&) { /* disappeared, or not a readable regular artifact */ }
    }
    for (auto& [path,state] : before) if (!after.contains(path) && !fs::exists(path)) {
      auto existing = db->query("SELECT id FROM artifacts WHERE project_id=? AND path=?",{p_.project,path.string()});
      if (!existing.empty()) event("artifact.removed",{{"id",existing[0]["id"]},{"path",path.string()}});
      (void)state;
    }
    auto result = r.json(); result["execution"] = host ? "host" : "sandbox"; result["host_restrictions"] = host ? policy["host_restrictions"] : Json("sandbox"); result["artifacts_recorded"] = tracked; result["artifact_tracking_limited"] = before.size() >= 10000 || after.size() >= 10000 || tracked >= 256;
    return result;
  }
  if (name == "task_create") {
    if (a["checks"].empty()) throw std::runtime_error("A task requires at least one proof obligation");
    p_.task = db->exec("INSERT INTO tasks(session_id,project_id,title,objective,status,risk,domain,created_at) VALUES(?,?,?,?,'active',?,?,?)",{p_.session,p_.project,a["title"],a["objective"],a["risk"],a.value("domain","general"),now()});
    for (auto& check : a["checks"]) db->exec("INSERT INTO task_checks(task_id,description) VALUES(?,?)",{p_.task,check});
    event("task.created",{{"id",p_.task},{"definition",a}});
    return {{"id",p_.task},{"checks",db->query("SELECT * FROM task_checks WHERE task_id=?",{p_.task})}};
  }
  if (name == "task_update") {
    Id id = a["id"]; auto rows = db->query("SELECT * FROM tasks WHERE id=? AND project_id=?",{id,p_.project});
    if (rows.empty()) throw std::runtime_error("Unknown task in current project");
    if (a["status"] == "completed") {
      auto checks = db->query("SELECT * FROM task_checks WHERE task_id=? AND required=1 AND status!='passed'",{id});
      auto assumptions = db->query("SELECT * FROM assumptions WHERE task_id=? AND status='unresolved' AND impact_if_wrong='high'",{id});
      if (!checks.empty() || !assumptions.empty()) return {{"error","Required proof obligations or high-impact assumptions remain unresolved"},{"checks",checks},{"assumptions",assumptions}};
    }
    p_.task = id;
    db->exec("UPDATE tasks SET status=?,completed_at=? WHERE id=?",{a["status"],a["status"] == "completed" ? Json(now()) : Json(),id});
    event(a["status"] == "completed" ? "task.completed" : "task.updated",a);
    return {{"id",id},{"status",a["status"]},{"operational_confidence",memory_.confidence("task",id)}};
  }
  if (name == "task_add_check") {
    active_task();
    auto id = db->exec("INSERT INTO task_checks(task_id,description) VALUES(?,?)",{p_.task,a["description"]});
    db->exec("UPDATE tasks SET status='verifying',completed_at=NULL WHERE id=? AND status='completed'",{p_.task});
    event("task.check_added",{{"check_id",id},{"description",a["description"]}}); return {{"check_id",id}};
  }
  if (name == "check_resolve") {
    active_task(); Id sid = a["source_event_id"]; auto origin = source(sid);
    auto rows = db->query("SELECT * FROM task_checks WHERE id=? AND task_id=?",{a["check_id"],p_.task});
    if (rows.empty()) throw std::runtime_error("Unknown check on active task");
    if (origin["task_id"] != p_.task && origin["type"] != "user.message") throw std::runtime_error("Proof belongs to a different task");
    auto payload = Json::parse(origin["payload_json"].get<std::string>());
    bool passed = a["passed"];
    if (origin["type"] == "user.message") {
      auto text = lower(payload.value("content","")); bool confirmation = false;
      for (auto* word : {"confirmed","verified","passed","looks good","i checked","yes","correct","funciona","confirmado"})
        if (text.find(word) != std::string::npos) confirmation = true;
      if (!confirmation) throw std::runtime_error("Proof requires explicit user confirmation, not an initial request");
    }
    if (passed && origin["type"] == "tool.failed") throw std::runtime_error("Failed tool output cannot pass a check");
    if (origin["type"] == "tool.completed") {
      auto tool = payload.value("tool","");
      if (tool != "shell_exec" && tool != "file_read" && tool != "file_write" && tool != "observe_environment") throw std::runtime_error("Proof must be externally observed, not a cognitive tool acknowledgement");
    }
    Id evidence_id = memory_.evidence("check",a["check_id"],origin["type"] == "user.message" ? "user confirmed" : "output observed",passed,sid,1,0.9);
    db->exec("UPDATE task_checks SET status=?,evidence_id=? WHERE id=?",{passed ? "passed" : "failed",evidence_id,a["check_id"]});
    event("task.check_resolved",a); return {{"evidence_id",evidence_id}};
  }
  if (name == "record_assumption") {
    active_task(); auto ev = event("assumption.created",a);
    auto id = db->exec("INSERT INTO assumptions(task_id,statement,impact_if_wrong,verification_method,source_event_id,created_at) VALUES(?,?,?,?,?,?)",{p_.task,a["statement"],a["impact"],a["verification_method"],ev,now()}); return {{"id",id}};
  }
  if (name == "record_hypothesis") {
    active_task(); auto ev = event("hypothesis.created",a);
    auto id = db->exec("INSERT INTO hypotheses(task_id,statement,source_event_id,created_at) VALUES(?,?,?,?)",{p_.task,a["statement"],ev,now()}); return {{"id",id}};
  }
  if (name == "resolve_assumption" || name == "resolve_hypothesis") {
    active_task(); source(a["source_event_id"]);
    std::string table = name == "resolve_assumption" ? "assumptions" : "hypotheses";
    auto id = a["id"].get<Id>();
    if (db->query("SELECT id FROM " + table + " WHERE id=? AND task_id=?",{id,p_.task}).empty()) throw std::runtime_error("Unknown epistemic state on current task");
    db->exec("UPDATE " + table + " SET status=? WHERE id=?",{a["confirmed"].get<bool>() ? "confirmed" : "rejected",id});
    memory_.evidence(table,id,"observation",a["confirmed"],a["source_event_id"]); event(table + ".resolved",a); return {{"id",id}};
  }
  if (name == "record_prediction") {
    active_task(); auto ev = event("prediction.created",a);
    auto id = db->exec("INSERT INTO predictions(task_id,statement,expected_outcome,confidence,domain,source_event_id,created_at) VALUES(?,?,?,?,?,?,?)",{p_.task,a["statement"],a["expected_outcome"],std::clamp(a["confidence"].get<double>(),0.05,0.95),a["domain"],ev,now()}); return {{"id",id}};
  }
  if (name == "record_observation") {
    active_task(); auto origin = source(a["source_event_id"]);
    auto prediction = db->query("SELECT * FROM predictions WHERE id=? AND task_id=?",{a["prediction_id"],p_.task});
    if (prediction.empty() || prediction[0]["created_at"].get<Id>() > origin["ts"].get<Id>()) throw std::runtime_error("Prediction must precede observation and belong to current task");
    auto id = db->exec("INSERT INTO observations(task_id,prediction_id,statement,matched,source_event_id,created_at) VALUES(?,?,?,?,?,?)",{p_.task,a["prediction_id"],a["statement"],a["matched"],a["source_event_id"],now()});
    event("observation.recorded",a); return {{"id",id}};
  }
  if (name == "learn_fact") { source(a["source_event_id"],a.value("correction",false)); return {{"id",memory_.fact(a["subject"],a["predicate"],a["object"],a["source_event_id"],a.value("correction",false),a.value("scope","global") == "project" ? p_.project : 0)}}; }
  if (name == "record_belief") {
    auto origin = source(a["source_event_id"]);
    auto id = db->exec("INSERT INTO beliefs(statement,domain,reasoning_confidence,source_event_id,created_at,updated_at) VALUES(?,?,?,?,?,?)",{a["statement"],a["domain"],std::clamp(a.value("reasoning_confidence",0.5),0.05,0.95),a["source_event_id"],now(),now()});
    memory_.evidence("belief",id,"initial supporting observation",true,a["source_event_id"],0.5,origin["type"] == "tool.completed" ? 0.8 : 0.5);
    auto confidence = memory_.confidence("belief",id);
    db->exec("UPDATE beliefs SET evidence_confidence=?,operational_confidence=? WHERE id=?",{confidence,confidence,id});
    event("belief.created",{{"id",id},{"belief",a}}); return {{"id",id},{"evidence_confidence",confidence}};
  }
  if (name == "record_evidence") {
    auto origin = source(a["source_event_id"]);
    std::string type = a["target_type"],table = type == "hypothesis" ? "hypotheses" : type == "belief" ? "beliefs" : type + "s";
    Id id = a["target_id"]; bool support = a["support"];
    if (db->query("SELECT id FROM " + table + " WHERE id=?",{id}).empty()) throw std::runtime_error("Unknown epistemic target");
    std::string kind = origin["type"] == "user.message" ? "user statement" : "output observed";
    auto prior = db->query("SELECT id FROM evidence WHERE target_type=? AND target_id=? AND kind=? AND direction=? AND source_event_id=?",{type,id,kind,support ? "support" : "against",a["source_event_id"]});
    if (!prior.empty()) return {{"id",prior[0]["id"]},{"already_recorded",true},{"evidence_confidence",memory_.confidence(type,id)}};
    auto proof = memory_.evidence(type,id,kind,support,a["source_event_id"],1,0.75);
    auto confidence = memory_.confidence(type,id);
    if (type == "fact") db->exec("UPDATE facts SET confidence=? WHERE id=?",{confidence,id});
    if (type == "belief") db->exec("UPDATE beliefs SET evidence_confidence=?,operational_confidence=?,status=?,updated_at=? WHERE id=?",{confidence,confidence,support ? (confidence >= 0.65 ? "supported" : "unresolved") : "contested",now(),id});
    if (!support && type == "task") {
      db->exec("UPDATE tasks SET status='verifying',completed_at=NULL WHERE id=?",{id});
      db->exec("INSERT INTO task_checks(task_id,description) VALUES(?,?)",{id,"Contradictory observation: " + a["description"].get<std::string>()});
    }
    if (!support && (type == "hypothesis" || type == "assumption")) db->exec("UPDATE " + table + " SET status='unresolved' WHERE id=?",{id});
    event("evidence.recorded",{{"id",proof},{"target",a},{"confidence",confidence}}); return {{"id",proof},{"evidence_confidence",confidence}};
  }
  if (name == "record_goal") {
    auto id = db->exec("INSERT INTO goals(scope,project_id,description,created_at) VALUES(?,?,?,?)",{a["scope"],a["scope"] == "project" ? Json(p_.project) : Json(),a["description"],now()}); event("goal.created",{{"id",id},{"goal",a}}); return {{"id",id}};
  }
  if (name == "record_decision") { auto id = event("decision.made",a); return {{"source_event_id",id}}; }
  if (name == "complete_goal") {
    auto id = a["id"].get<Id>();
    if (db->query("SELECT id FROM goals WHERE id=?",{id}).empty()) throw std::runtime_error("Unknown goal");
    active_task(); if (db->query("SELECT id FROM tasks WHERE id=? AND status='completed'",{p_.task}).empty()) throw std::runtime_error("Complete and verify the active task first");
    db->exec("UPDATE goals SET status='completed',completed_at=? WHERE id=?",{now(),id}); event("goal.completed",a); return {{"id",id}};
  }
  if (name == "record_commitment") {
    auto ev = event("commitment.created",a); auto id = db->exec("INSERT INTO commitments(description,origin_event,due_condition,created_at) VALUES(?,?,?,?)",{a["description"],ev,a["due_condition"],now()}); return {{"id",id}};
  }
  if (name == "record_open_loop") {
    auto id = db->exec("INSERT INTO open_loops(kind,description,project_id,priority,created_at,updated_at) VALUES(?,?,?,?,?,?)",{a["kind"],a["description"],p_.project,std::clamp(a["priority"].get<double>(),0.0,1.0),now(),now()}); event("open_loop.created",{{"id",id},{"loop",a}}); return {{"id",id}};
  }
  if (name == "complete_commitment" || name == "close_open_loop") {
    source(a["source_event_id"]);
    bool promise = name == "complete_commitment";
    std::string table = promise ? "commitments" : "open_loops";
    if (db->query("SELECT id FROM " + table + " WHERE id=?",{a["id"]}).empty()) throw std::runtime_error("Unknown prospective memory");
    db->exec("UPDATE " + table + (promise ? " SET status='completed',completed_at=? WHERE id=?" : " SET state='closed',updated_at=? WHERE id=?"),{now(),a["id"]});
    event(promise ? "commitment.completed" : "open_loop.closed",a); return {{"id",a["id"]}};
  }
  if (name == "record_intention") {
    auto id = db->exec("INSERT INTO intentions(trigger_type,trigger_json,action_or_reminder,reason,created_at) VALUES(?,?,?,?,?)",{a["trigger_type"],Json{{"match",a["trigger"]}}.dump(),a["reminder"],a["reason"],now()}); event("intention.created",{{"id",id},{"intention",a}}); return {{"id",id}};
  }
  if (name == "record_curiosity") { auto id = db->exec("INSERT INTO curiosities(question,project_id,created_at) VALUES(?,?,?)",{a["question"],p_.project,now()}); event("curiosity.created",a); return {{"id",id}}; }
  if (name == "praxis_candidate") return {{"id",memory_.candidate(a,event("lesson.recorded",a))}};
  if (name == "praxis_outcome") {
    auto rows = db->query("SELECT id FROM events WHERE task_id=? AND type=? ORDER BY id DESC LIMIT 1",{a["task_id"],a["success"].get<bool>() ? "task.completed" : "tool.failed"});
    if (rows.empty()) throw std::runtime_error("Praxis requires a real task outcome");
    memory_.praxis_outcome(a["id"],a["task_id"],a["success"],rows[0]["id"]); return {{"id",a["id"]}};
  }
  if (name == "record_failed_strategy") {
    active_task(); auto failure = db->query("SELECT id FROM events WHERE task_id=? AND type='tool.failed' ORDER BY id DESC LIMIT 1",{p_.task});
    if (failure.empty()) throw std::runtime_error("A failed strategy requires observed failure");
    auto id = db->exec("INSERT INTO failed_strategies(task_id,context,strategy,failure,lesson,source_event_id,created_at) VALUES(?,?,?,?,?,?,?)",{p_.task,a["context"],a["strategy"],a["failure"],a["lesson"],failure[0]["id"],now()}); return {{"id",id}};
  }
  if (name == "record_relationship" || name == "develop_trait") {
    std::set<Id> seen; std::set<Id> sessions;
    for (auto& id : a["source_event_ids"]) {
      auto rows = db->query("SELECT * FROM events WHERE id=? AND type=?",{id,name == "develop_trait" ? "task.completed" : "user.message"});
      if (rows.empty()) throw std::runtime_error("Invalid behavior evidence");
      seen.insert(id.get<Id>()); if (!rows[0]["session_id"].is_null()) sessions.insert(rows[0]["session_id"].get<Id>());
    }
    bool explicit_pref = name == "record_relationship" && a["kind"] == "explicit";
    if (seen.size() < (explicit_pref ? 1U : 3U) || (!explicit_pref && sessions.size() < 2)) throw std::runtime_error("Insufficient repeated evidence");
    auto ev = event(name == "develop_trait" ? "trait.developed" : "relationship.learned",a);
    auto id = name == "develop_trait" ? db->exec("INSERT INTO developed_traits(trait,confidence,evidence_count,source_event_id,created_at) VALUES(?,?,?,?,?)",{a["trait"],0.6,seen.size(),ev,now()}) : db->exec("INSERT INTO relationship_beliefs(belief,kind,confidence,evidence_count,source_event_id,created_at) VALUES(?,?,?,?,?,?)",{a["belief"],a["kind"],explicit_pref ? 0.9 : 0.6,seen.size(),ev,now()}); return {{"id",id}};
  }
  if (name == "compile_skill") {
    auto praxis = db->query("SELECT * FROM praxis WHERE id=? AND status='habitual'",{a["praxis_id"]});
    if (praxis.empty()) throw std::runtime_error("Only habitual praxis may be compiled");
    if (a["steps"].empty() || a["steps"].size() > 16) throw std::runtime_error("Skill needs 1–16 steps");
    for (auto& step : a["steps"]) if (step["tool"] != "shell_exec" && step["tool"] != "file_read" && step["tool"] != "file_write") throw std::runtime_error("Invalid skill action");
    bool host=std::any_of(a["steps"].begin(),a["steps"].end(),[](const Json& step){return step["tool"]=="shell_exec" && step["arguments"].value("execution","sandbox")=="host";});
    if (host && permissions()["approval_required"].get<bool>() && !approve_(name,a)) return {{"error","User declined persistent host skill"}};
    auto ev = event("skill.compiled",a);
    auto id = db->exec("INSERT INTO skills(praxis_id,name,workflow_json,source_event_id,created_at) VALUES(?,?,?,?,?)",{a["praxis_id"],a["name"],a["steps"].dump(),ev,now()}); return {{"id",id}};
  }
  if (name == "run_skill") {
    auto skill = db->query("SELECT s.* FROM skills s JOIN praxis p ON p.id=s.praxis_id WHERE s.id=? AND p.status!='deprecated' AND (p.project_id IS NULL OR p.project_id=?)",{a["id"],p_.project}); if (skill.empty()) throw std::runtime_error("Unknown, deprecated, or out-of-scope skill");
    Json results = Json::array();
    for (auto& step : Json::parse(skill[0]["workflow_json"].get<std::string>())) { auto r = execute(step["tool"],step["arguments"]); results.push_back(r); if (r.contains("error") || (r.contains("exit_code") && r["exit_code"] != 0)) break; }
    return {{"results",results}};
  }
  throw std::runtime_error("Tool is not implemented");
}
}
