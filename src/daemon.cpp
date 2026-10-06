#include <saga/debug.hpp>
#include <saga/ipc.hpp>
#include <saga/runtime.hpp>
#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace saga;
namespace {
volatile sig_atomic_t stopping = 0;
void stop(int signal) { stopping = signal; }
DebugOptions daemon_debug;
std::mutex config_mutex;
void serve_connected(int fd,Paths paths,bool authenticate = false) {
  Channel channel(fd);
  std::unique_ptr<DebugLogger> recorder;
  if(daemon_debug.profile!=DebugProfile::Off)recorder=std::make_unique<DebugLogger>(daemon_debug);
  DebugScope recording(recorder.get());
  std::unique_ptr<Runtime> runtime;
  std::string id;
  Emit emit = [&](const std::string& event,const Json& data){ channel.send({{"type",event},{"request_id",id},{"payload",data}}); };
  auto live_command = [&](const Json& message) {
    auto control_id=message.value("request_id","");
    try {
      auto payload=message.value("payload",Json::object());
      if (control_id.empty() || control_id.size()>128 || !payload.is_object()) throw std::runtime_error("Invalid control request");
      auto args=payload.value("arguments",Json::object());
      if (!runtime || message.value("type","") != "command" || !live_command_allowed(payload.value("name",""),args))
        throw std::runtime_error("This command requires the agent to finish its current turn. Use /help, /status, /permissions or an inspection command while it works.");
      auto result=runtime->command(payload.at("name"),args,[&](const std::string& type,const Json& data){channel.send({{"type",type},{"request_id",control_id},{"payload",data}});});
      channel.send({{"type","result"},{"request_id",control_id},{"payload",result}});
    } catch (const std::exception& e) {
      channel.send({{"type","error"},{"request_id",control_id},{"payload",{{"message",e.what()}}}});
    }
  };
  auto service = [&] {
    if(recorder)recorder->sample();
    if(stopping && runtime){trace("shutdown","signal.received",{{"signal",static_cast<int>(stopping)}},DebugProfile::Debug,"WARN");runtime->command("stop",Json::object());}
    // Keep cognition on one thread; handle only controls that cannot replace it.
    for (int n=0; n<16; ++n) {
      auto message=channel.receive(0); if (!message) break;
      live_command(*message);
    }
  };
  try {
    if (authenticate) {
      auto message=channel.receive(5000);
      if (!message || message->value("type","") != "authenticate" || message->value("payload",Json::object()).value("token","") != read_file(paths.runtime/"control.token",256)) return;
      channel.send({{"type","result"},{"request_id",message->value("request_id","")},{"payload",{{"authenticated",true}}}});
    }
    Registry registry(paths);
    auto last_tick = std::chrono::steady_clock::now();
    while (!stopping) {
      auto incoming = channel.receive(1000);
      if (!incoming) {
        if (runtime && std::chrono::steady_clock::now()-last_tick >= std::chrono::seconds(60)) {
          runtime->tick(emit,false); last_tick = std::chrono::steady_clock::now();
        }
        continue;
      }
      auto message = *incoming;
      id = message.value("request_id","");
      auto type = message.value("type",""); auto payload = message.value("payload",Json::object());
      if (id.empty() || id.size() > 128 || !payload.is_object()) throw std::runtime_error("Invalid control request");
      try {
        if(recorder)recorder->context({{"ipc_request_id",id}});
        if(type=="debug.configure") {
          if(runtime)throw std::runtime_error("Configure diagnostics before activating a persona");
          auto options=DebugOptions::from_json(payload,paths);recording.bind(nullptr);recorder.reset();
          if(options.profile!=DebugProfile::Off){recorder=std::make_unique<DebugLogger>(options);install_debug_crash_handlers();}
          recording.bind(recorder.get());
          emit("result",{{"profile",debug_profile_name(options.profile)},{"path",recorder?recorder->path().string():std::string()}});
        }
        else if (type == "ping") emit("result",{{"runtime","Saga"},{"version","0.1.0"},{"context_revision",runtime_context_revision}});
        else if (type == "personas.list") { if (runtime) throw std::runtime_error("Return to the persona selector first"); emit("result",registry.list()); }
        else if (type == "personas.create") { if (runtime) throw std::runtime_error("Return to the persona selector first"); emit("result",registry.create(payload.value("name","Assistant"),payload.value("soul",std::string(default_soul)))); }
        else if (type == "backend.status") {
          std::lock_guard lock(config_mutex); auto c = Config::load(paths);
          emit("result",{{"configured",!c.model.empty() && c.context_length > 0},{"endpoint",c.endpoint},{"model",c.model},{"context_length",c.context_length}});
        }
        else if (type == "backend.models" || type == "backend.setup") {
          if (runtime) throw std::runtime_error("Close the active session before model setup");
          Config c; { std::lock_guard lock(config_mutex); c = Config::load(paths); }
          c.endpoint = payload.at("endpoint"); c.api_key = payload.value("api_key","");
          if (const char* key = std::getenv("SAGA_API_KEY")) c.api_key = key;
          c.model = payload.value("model",""); c.context_length = payload.value("context_length",0ULL);
          c.allow_small_context = payload.value("allow_small_context",false);
          c.insecure_tls = payload.value("insecure_tls",false);
          c.timeout_seconds = payload.value("timeout_seconds",c.timeout_seconds);
          if (c.allow_small_context) { c.generation_reserve = payload.value("generation_reserve",512ULL); c.safety_margin = payload.value("safety_margin",256ULL); }
          OpenAICompatibleBackend backend(c);
          if (type == "backend.models") {
            emit("backend.progress",{{"stage","models"},{"message","Discovering models"},{"status","running"}});
            Json models = Json::array(); for (auto& m : backend.models()) models.push_back(m.json()); emit("result",models);
          }
          else {
            emit("backend.progress",{{"stage","context"},{"message","Detecting effective context"},{"status","running"}});
            auto info = backend.discover();
            if (info.context_length) c.context_length = *info.context_length;
            c.model = info.id;
            if (!c.context_length) { emit("result",{{"needs_manual_context",true},{"model",c.model}}); }
            else {
              c.validate();
              emit("backend.progress",{{"stage","context"},{"message",c.model + " — " + std::to_string(c.context_length) + " context"},{"status","passed"}});
              { std::lock_guard lock(config_mutex); c.save(paths); }
              info.context_length = c.context_length;
              emit("result",{{"model",info.json()},{"capabilities",{{"connected",true},{"probed",false},{"completion",nullptr},{"streaming",nullptr},{"tool_calls",nullptr}}}});
            }
          }
        }
        else if (type == "persona.activate") {
          if (runtime) { runtime->close("persona_switch",emit); runtime.reset(); }
          Config c; { std::lock_guard lock(config_mutex); c = Config::load(paths); }
          c.validate();
          auto context = std::make_unique<PersonaContext>(paths,registry.select(payload.at("uuid")),payload.at("cwd").get<std::string>());
          auto host_environment=payload.value("host_environment",Json::object());
          if (!host_environment.is_object() || host_environment.dump().size() > 32768) throw std::runtime_error("Invalid host environment");
          context->host_environment=std::move(host_environment);
          auto approve = [&](const std::string& tool,const Json& args) {
            auto approval_id = uuid();
            runtime->persona().db->event("approval.requested",{{"approval_id",approval_id},{"tool",tool},{"arguments",args}},runtime->persona().session,runtime->persona().task);
            emit("approval.requested",{{"approval_id",approval_id},{"tool",tool},{"arguments",args}});
            auto deadline = std::chrono::steady_clock::now()+std::chrono::minutes(5);
            while (!stopping && std::chrono::steady_clock::now() < deadline) {
              auto reply = channel.receive(1000); if (!reply) continue;
              if (reply->value("type","") == "approval" && reply->value("request_id","") == id && reply->contains("payload") && (*reply)["payload"].value("approval_id","") == approval_id)
              {
                bool granted=(*reply)["payload"].value("approved",false);
                runtime->persona().db->event("approval.resolved",{{"approval_id",approval_id},{"approved",granted}},runtime->persona().session,runtime->persona().task);
                emit("approval.resolved",{{"approval_id",approval_id},{"approved",granted}});
                return granted;
              }
              live_command(*reply);
              try { runtime->check_cancelled(); }
              catch(const TurnCancelled&) { runtime->persona().db->event("approval.resolved",{{"approval_id",approval_id},{"approved",false},{"reason","user_stop"}},runtime->persona().session,runtime->persona().task);emit("approval.resolved",{{"approval_id",approval_id},{"approved",false},{"reason","user_stop"}});throw; }
            }
            runtime->persona().db->event("approval.resolved",{{"approval_id",approval_id},{"approved",false},{"reason","expired"}},runtime->persona().session,runtime->persona().task);
            emit("approval.resolved",{{"approval_id",approval_id},{"approved",false},{"reason","expired"}});
            return false;
          };
          runtime = std::make_unique<Runtime>(std::move(context),c,std::make_unique<OpenAICompatibleBackend>(c),approve);
          runtime->service(service);
          runtime->start(emit); emit("result",{{"active",true}});
        }
        else if (type == "chat") { if (!runtime) throw std::runtime_error("Select a persona first"); runtime->chat(payload.at("content"),emit,payload.value("user_message_id",id)); emit("result",{{"ok",true}}); }
        else if (type == "command") {
          if (!runtime) throw std::runtime_error("Select a persona first");
          auto name = payload.at("name").get<std::string>(); auto args = payload.value("arguments",Json::object());
          if (name == "name" && args.contains("name")) registry.rename(runtime->persona().id,args.at("name"));
          emit("result",runtime->command(name,args,emit));
        }
        else if (type == "session.close") { if (runtime) { runtime->close(payload.value("reason","user_exit"),emit); runtime.reset(); } emit("result",{{"closed",true}}); }
        else throw std::runtime_error("Unknown protocol request");
      } catch (const std::exception& e) {trace("runtime","runtime.error",{{"message",e.what()},{"operation",type},{"recoverable",true}},DebugProfile::Debug,"ERROR");emit("error",{{"message",e.what()}}); }
    }
  } catch (const std::exception& error) {trace("network","connection.closed",{{"reason",error.what()}},DebugProfile::Debug,"WARN");}
  trace("shutdown","shutdown.started",{{"signal",static_cast<int>(stopping)}});
  if (runtime) { try { runtime->close(stopping ? "daemon_shutdown" : "client_disconnect"); } catch (const std::exception& error) {trace("shutdown","shutdown.error",{{"message",error.what()}},DebugProfile::Debug,"ERROR");} }
  trace("shutdown","shutdown.completed");
}
void serve(int fd,Paths paths) {
  ucred cred{}; socklen_t length = sizeof cred;
  if (getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&length) || cred.uid != getuid()) { close(fd); return; }
  serve_connected(fd,std::move(paths),true);
}
}
int main(int argc,char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--help") { std::cout << "sagad — Saga persistent cognitive runtime\nRuns in foreground on the private XDG Unix socket.\n"; return 0; }
  if (argc > 1 && std::string(argv[1]) == "--version") { std::cout << "Saga 0.1.0\n"; return 0; }
  try {
    for(int i=1;i<argc;++i)if(!debug_argument(argc,argv,i,daemon_debug))throw std::runtime_error("Unknown sagad argument");
    umask(0077); auto paths = Paths::environment(); paths.create();
    daemon_debug=DebugOptions::from_json(daemon_debug.json(),paths);
    if(daemon_debug.profile!=DebugProfile::Off)install_debug_crash_handlers();
    int lock_fd = open((paths.runtime / "sagad.lock").c_str(),O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,0600);
    if (lock_fd < 0 || flock(lock_fd,LOCK_EX | LOCK_NB)) throw std::runtime_error("sagad is already running");
    // Initialize WAL and the registry schema before connection workers race to open it.
    { Registry registry(paths); }
    atomic_write(paths.runtime/"control.token",uuid()+uuid());
    int listener = listen_socket(paths.socket());
    std::signal(SIGINT,stop); std::signal(SIGTERM,stop); std::signal(SIGPIPE,SIG_IGN);
    struct Worker { std::shared_ptr<std::atomic_bool> done; std::jthread thread; };
    std::vector<Worker> clients;
    // ponytail: one connection worker per attached identity; persona flock serializes writers.
    std::jthread maintenance([paths](std::stop_token token){
      auto next = std::chrono::steady_clock::now()+std::chrono::seconds(60);
      while (!token.stop_requested() && !stopping) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (std::chrono::steady_clock::now() < next) continue;
        next = std::chrono::steady_clock::now()+std::chrono::seconds(60);
        try {
          Registry registry(paths); Config config; { std::lock_guard lock(config_mutex); config = Config::load(paths); }
          config.validate(); config.timeout_seconds = std::min(config.timeout_seconds,10);
          auto work = fs::temp_directory_path() / ("saga-maintenance-" + std::to_string(getuid())); private_dir(work);
          for (auto& metadata : registry.list()) {
            if (token.stop_requested() || stopping) break;
            try {
              auto p = std::make_unique<PersonaContext>(paths,metadata,work);
              Runtime runtime(std::move(p),config,std::make_unique<OpenAICompatibleBackend>(config),[](auto&,auto&){ return false; });
              runtime.tick();
            } catch (const std::exception&) { /* attached identity or deferred work */ }
          }
        } catch (const std::exception&) { /* model may not be configured yet */ }
      }
    });
    while (!stopping) {
      std::erase_if(clients,[](const Worker& worker){ return worker.done->load(); });
      pollfd p{listener,POLLIN,0}; if (poll(&p,1,250) <= 0) continue;
      int client = accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);
      if (client >= 0) {
        if (clients.size() >= 32) { close(client); continue; }
        auto done = std::make_shared<std::atomic_bool>(false);
        clients.push_back({done,std::jthread([client,paths,done]{ serve(client,paths); done->store(true); })});
      }
    }
    maintenance.request_stop(); close(listener); clients.clear(); fs::remove(paths.socket()); close(lock_fd);
    return 0;
  } catch (const std::exception& e) { std::cerr << "sagad: " << e.what() << '\n'; return 1; }
}
