#include <saga/ipc.hpp>
#include <saga/model.hpp>
#include <saga/chat_ui.hpp>
#include <clocale>
#include <iostream>
#include <map>
#include <mutex>
#include <termios.h>
#include <unistd.h>

using namespace saga;
namespace {
struct Client {
  std::unique_ptr<Channel> channel;
  bool json_output = false;
  std::mutex receive_mutex;
  std::mutex control_mutex;
  std::map<std::string,Emit> controls;
  Client(std::unique_ptr<Channel> connection,bool json) : channel(std::move(connection)),json_output(json) {}
  void send(const Json& message) { std::lock_guard lock(control_mutex); channel->send(message); }
  void live_request(Json payload,Emit callback) {
    auto id=uuid(); std::lock_guard lock(control_mutex);
    controls.emplace(id,std::move(callback));
    try { channel->send({{"type","command"},{"request_id",id},{"payload",std::move(payload)}}); }
    catch (...) { controls.erase(id); throw; }
  }
  bool dispatch_control(const Json& frame) {
    Emit callback;
    {
      std::lock_guard lock(control_mutex);
      auto it=controls.find(frame.value("request_id","")); if (it==controls.end()) return false;
      auto type=frame.value("type","");
      if(type=="result" || type=="error"){callback=std::move(it->second);controls.erase(it);}else callback=it->second;
    }
    auto data=frame.value("payload",Json::object());if(frame.at("type")=="approval.requested")data["_request_id"]=frame.value("request_id","");callback(frame.at("type"),data);return true;
  }
  Json request(std::string type,Json payload = Json::object(),Emit callback = {}) {
    std::unique_lock receive_lock(receive_mutex);
    std::string streamed;
    auto id = uuid(); send({{"type",std::move(type)},{"request_id",id},{"payload",std::move(payload)}});
    auto started = std::chrono::steady_clock::now(),last_wait_notice = started;
    while (true) {
      auto frame = channel->receive(callback ? 1000 : -1);
      if (!frame) {
        auto time = std::chrono::steady_clock::now();
        if (callback && time-last_wait_notice >= std::chrono::seconds(10)) {
          callback("request.waiting",{{"elapsed_seconds",std::chrono::duration_cast<std::chrono::seconds>(time-started).count()}}); last_wait_notice = time;
        }
        continue;
      }
      auto t = frame->value("type",""); auto p = frame->value("payload",Json::object());
      if (t == "approval.requested") p["_request_id"]=frame->value("request_id","");
      if (json_output) std::cout << frame->dump() << '\n' << std::flush;
      if (dispatch_control(*frame)) continue;
      if (frame->value("request_id","") != id) { if (callback) callback(t,p); continue; }
      if (t == "error") throw std::runtime_error(p.value("message","Runtime error"));
      if (t == "result") return p;
      if (t == "approval.requested") {
        p["_request_id"] = id;
        bool approved = false;
        if (callback) { callback(t,p); continue; }
        if (!json_output && isatty(STDIN_FILENO)) {
          std::cout << "\nApprove " << p["tool"].get<std::string>() << ": " << display_text(p["arguments"].dump(2)) << "\n[y/N] " << std::flush;
          std::string answer; std::getline(std::cin,answer); approved = lower(trim(answer)) == "y";
        }
        send({{"type","approval"},{"request_id",id},{"payload",{{"approval_id",p["approval_id"]},{"approved",approved}}}});
      } else if (callback) callback(t,p);
      else if (!json_output) {
        if (t == "assistant.delta") { streamed += p.value("content",""); std::cout << display_text(p.value("content","")) << std::flush; }
        else if (t == "assistant.completed") {
          auto final = p.value("content","");
          if (final.starts_with(streamed)) std::cout << display_text(final.substr(streamed.size()));
          else std::cout << '\n' << display_text(final);
          std::cout << '\n';
        }
        else if (t == "context.usage" && !p.value("streaming",false)) std::cout << "\n[" << (p.value("approximate",true) ? "~" : "") << p.value("used_tokens",p.value("input_tokens",0ULL)+p.value("output_tokens",0ULL)) << "/" << p.value("context_length",0ULL) << " tokens]\n";
        else if (t == "tool.started") std::cout << "\n[" << display_text(p.value("tool","")) << "]\n";
        else if (t == "tool.failed") std::cout << "[tool failed]\n";
        else if (t == "notification") std::cout << "\n" << display_text(p.value("description","")) << '\n';
      }
    }
  }
};
std::string prompt(const std::string& text,bool secret = false) {
  std::cout << text << std::flush;
  termios old{}; bool changed = false;
  if (secret && isatty(STDIN_FILENO) && tcgetattr(0,&old) == 0) { auto hidden = old; hidden.c_lflag &= static_cast<tcflag_t>(~ECHO); changed = tcsetattr(0,TCSANOW,&hidden) == 0; }
  std::string value; bool read = static_cast<bool>(std::getline(std::cin,value));
  if (changed) { tcsetattr(0,TCSANOW,&old); std::cout << '\n'; }
  if (!read) throw std::runtime_error("Input closed");
  return value;
}
int select_menu(const std::string& title,const std::vector<std::string>& entries,bool full_screen) {
  (void)full_screen;
  std::cout << title << "\n\n";
  for (size_t i = 0; i < entries.size(); ++i) std::cout << "  " << i+1 << ". " << display_text(entries[i]) << '\n';
  while (true) {
    auto input = prompt("\nSelect (q to quit): ");
    if (lower(trim(input)) == "q") return -1;
    try { size_t pos = 0; int n = std::stoi(input,&pos); if (pos == input.size() && n > 0 && n <= static_cast<int>(entries.size())) return n-1; } catch (const std::exception&) {}
    std::cout << "Select one of the listed entries.\n";
  }
}
Json create_persona(Client& client,bool first,bool full_screen) {
  int choice = select_menu(first ? "Create your first persona.\nAll settings can be changed later." : "Create persona.\nAll settings can be changed later.",{"Use defaults","Set name and SOUL.md"},full_screen);
  if (choice < 0) return Json();
  if (choice == 0) return client.request("personas.create");
  auto name = prompt("Name [Assistant]: "); if (trim(name).empty()) name = "Assistant";
  auto file = prompt("SOUL.md file (blank uses neutral defaults): ");
  return client.request("personas.create",{{"name",name},{"soul",file.empty() ? std::string(default_soul) : read_file(file,32768)}});
}
void setup(Client& client,bool full_screen) {
  std::cout << "Model setup. All settings can be changed later.\n";
  auto endpoint = prompt("Endpoint: "); auto key = prompt("API key (optional): ",true);
  std::string stage; auto stage_started = std::chrono::steady_clock::now();
  auto progress = [&](const std::string& type,const Json& p){
    if (type == "backend.progress") {
      stage = p.value("message",""); stage_started = std::chrono::steady_clock::now();
      std::cout << (p.value("status","") == "passed" ? "✓ " : "… ") << display_text(stage) << '\n' << std::flush;
    } else if (type == "request.waiting") {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-stage_started).count();
      std::cout << "  Waiting: " << display_text(stage) << " (" << elapsed << "s)\n" << std::flush;
    }
  };
  auto models = client.request("backend.models",{{"endpoint",endpoint},{"api_key",key}},progress);
  int selected = 0;
  if (models.size() > 1) {
    std::vector<std::string> names; for (auto& m : models) names.push_back(m.at("id"));
    selected = select_menu("Select the model.",names,full_screen); if (selected < 0) throw std::runtime_error("Model setup cancelled");
  }
  Json payload = {{"endpoint",endpoint},{"api_key",key},{"model",models.at(selected).at("id")}};
  auto result = client.request("backend.setup",payload,progress);
  if (result.value("needs_manual_context",false)) {
    std::cout << "The server did not expose its effective context window.\n";
    auto input = prompt("Effective serving context (tokens, at least 65536): "); size_t pos = 0;
    auto tokens = std::stoull(input,&pos); if (pos != input.size()) throw std::runtime_error("Invalid context length");
    payload["context_length"] = tokens; result = client.request("backend.setup",payload,progress);
  }
  std::cout << "✓ Connected\n✓ " << display_text(result["model"]["id"].get<std::string>()) << "\n✓ " << result["model"]["context_length"] << " context\n";
}
using Action = ChatAction;
Action local_command(Client& client,const std::string& input,const fs::path& cwd,Emit callback = {},bool live = false) {
  auto space = input.find(' '); auto name = input.substr(1,space == std::string::npos ? space : space-1);
  auto arg = space == std::string::npos ? "" : trim(input.substr(space+1));
  if (live && (name == "exit" || name == "quit" || name == "persona" || name == "model" || name == "new"))
    throw std::runtime_error("Wait for the current turn before changing or closing the session.");
  if (name == "exit" || name == "quit") return Action::Exit;
  if (name == "persona") return Action::Selector;
  if (name == "model") return Action::ModelSetup;
  if (name == "help") { auto help=command_help(arg); if (callback) callback("help",{{"content",help}}); else {for(auto& line:markdown_lines(help,100))std::cout<<chat_utf8(line.text)<<'\n';} return Action::Continue; }
  if (name == "new") { auto r=client.request("command",{{"name","new"}},callback); if (callback) {callback("session.reset",Json::object());if(r.contains("status"))callback("agent.status",r["status"]);} return Action::NewSession; }
  Json args = Json::object();
  if(name=="diff") {args["event_id"]=std::stoll(arg);}
  else if (name == "steer") {args["content"]=arg;}
  else if (name == "memory" || name == "know" || name == "praxis" || name == "artifacts") {
    args = {{"query",arg},{"kind",name == "know" ? "know" : name == "praxis" ? "know_how" : name == "artifacts" ? "recall_artifact" : "remember"},{"deep",true}}; name = "memory";
  } else if (name == "project" && !arg.empty()) args = {{"path",arg}};
  else if (name == "name" && !arg.empty()) args = {{"name",arg}};
  else if (name == "soul" && !arg.empty()) args = {{"content",read_file(fs::path(arg).is_absolute() ? fs::path(arg) : cwd / arg,32768)}};
  else if (name == "fact" || name == "correct") args = Json::parse(arg);
  else if (name == "permissions" && !arg.empty()) {
    auto value=lower(arg);
    if (value == "1" || value == "ask" || value == "ask always" || value == "ask_always") args["mode"]="ask_always";
    else if (value == "2" || value == "always" || value == "always approve" || value == "always_approve") args["mode"]="always_approve";
    else if (value == "3" || value == "host-ask" || value == "host_ask") args["mode"]="host_ask";
    else if (value == "4" || value == "host-always" || value == "host_always") args["mode"]="host_always";
    else throw std::runtime_error("Use /permissions 1, 2, 3 or 4 (ask, always, host-ask, host-always)");
  }
  if (live && !live_command_allowed(name,args)) throw std::runtime_error("This command requires the current turn to finish. Inspection commands and /permissions are available while the agent works.");
  auto show_result = [&,name,arg,callback](const Json& result) {
    if(name=="diff"){if(callback)callback("diff.loaded",result);}
    else if (name == "status" || name == "compact") {
      auto status=name == "status" ? result : result["status"];
      if (callback) { callback("agent.status",status); callback("result",format_agent_status(status)); }
      else if (!client.json_output) std::cout << display_text(format_agent_status(status)) << '\n';
    } else if (name == "permissions") {
      if (callback) { callback(arg.empty() ? "permissions.menu" : "permissions.changed",result); if (!arg.empty()) callback("result","Host permissions: "+result["label"].get<std::string>()); }
      else if (!client.json_output) std::cout << "Host permissions: " << result["label"].get<std::string>()
        << "\n1. Ask before guarded host operations (default).\n2. Automatically approve guarded host operations."
        << "\n3. Sandbox automatic; ask before unrestricted host operations.\n4. Allow unrestricted host operations without approval (DANGEROUS)."
        << "\nUse /permissions NUMBER. Sandbox actions stay automatic. Unrestricted host uses your OS user privileges; Saga storage is also accessible.\n";
    } else if (name == "steer") {if(callback && result.value("queued",false))callback("result","Steering queued for the next model call after the current command/approval finishes.");}
    else if (name == "stop") {if(callback)callback("result",result.value("stopping",false)?"Stopping…":"No active turn.");}
    else if (callback) callback("result",result);
    else if (!client.json_output) std::cout << display_text(result.dump(2)) << '\n';
  };
  Json payload={{"name",name},{"arguments",args}};
  if (live) client.live_request(std::move(payload),[show_result,callback](const std::string& type,const Json& result){
    if(type=="error")callback("command.error",result);else if(type=="result")show_result(result);else if(callback)callback(type,result);
  });
  else show_result(client.request("command",std::move(payload),callback));
  return Action::Continue;
}
Action line_chat(Client& client,const fs::path& cwd) {
  if (!client.json_output) std::cout << "Type /help for commands.\n";
  std::string line;
  while (true) {
    if (isatty(0) && !client.json_output) std::cout << "> " << std::flush;
    if (!std::getline(std::cin,line)) return Action::Exit;
    if (trim(line).empty()) continue;
    try {
      if (line.starts_with('/')) { auto a = local_command(client,line,cwd); if (a != Action::Continue && a != Action::NewSession) return a; }
      else client.request("chat",{{"content",line}});
    } catch (const std::exception& e) { std::cerr << "Saga: " << e.what() << '\n'; }
  }
}
Action tui_chat(Client& client,const fs::path& cwd,const std::string& name,const Json& backend,const Json& activation) {
  ChatView view; view.name=name; view.model=backend.value("model",""); view.context=backend.value("context_length",0ULL);
  view.event("agent.status",client.request("command",{{"name","status"}}));
  for (const auto& frame : activation) view.event(frame.at("type"),frame.at("payload"));
  return chat_ui(std::move(view),[&](const std::string& input,Emit callback){
    if (input.starts_with('/')) local_command(client,input,cwd,callback);
    else client.request("chat",{{"content",input}},callback);
  },[&]() -> std::optional<Json> {
    std::unique_lock lock(client.receive_mutex,std::try_to_lock);
    if (!lock.owns_lock()) return {};
    auto frame=client.channel->receive(100);
    if (frame && client.dispatch_control(*frame)) return {};
    return frame;
  },[&](const Json& message){ client.send(message); },[&](const std::string& input,Emit callback){
    local_command(client,input,cwd,std::move(callback),true);
  });
}
}
int main(int argc,char** argv) {
  try {
    if (!std::setlocale(LC_ALL,"")) std::setlocale(LC_ALL,"C.UTF-8");
    if (MB_CUR_MAX == 1) std::setlocale(LC_CTYPE,"C.UTF-8");
    bool full_screen = true, json_output = false; std::string batch;
    for (int i = 1; i < argc; ++i) {
      std::string arg(argv[i]);
      if (arg == "--help") { std::cout << "Saga — local-first persistent cognitive agents\nUsage: saga [--no-tui] [--json] [--batch UUID]\n       saga --request (one JSON control request from stdin)\n       saga --version\nSelect a persona at every interactive start. Use /help for commands.\n"; return 0; }
      if (arg == "--version") { std::cout << "Saga 0.1.0\n"; return 0; }
      if (arg == "--no-tui") full_screen = false;
      else if (arg == "--json") { json_output = true; full_screen = false; }
      else if (arg == "--batch" && i+1 < argc) { batch = argv[++i]; full_screen = false; }
      else if (arg != "--request") throw std::runtime_error("Unknown argument: " + arg);
    }
    auto paths = Paths::environment(); paths.create();
    auto exe = fs::canonical("/proc/self/exe").parent_path() / "sagad";
    start_daemon(paths,exe);
    Client client{Channel::connect(paths.socket()),json_output};
    if (argc == 2 && std::string(argv[1]) == "--request") {
      auto line = prompt(""); auto request = Json::parse(line); client.json_output = true;
      client.request(request.at("type"),request.value("payload",Json::object())); return 0;
    }
    if(client.request("ping").value("context_revision",0)<runtime_context_revision)
      throw std::runtime_error("The running sagad predates this client's runtime updates. Close existing Saga sessions, run 'pkill -TERM -x sagad', then reopen Saga to load the updated daemon.");
    auto cwd = fs::current_path();
    while (true) {
      Json metadata;
      if (!batch.empty()) {
        // Explicit script selection is deliberate; interactive startup never auto-selects.
        metadata = {{"uuid",batch},{"display_name",""}};
      } else {
        auto list = client.request("personas.list");
        if (list.empty()) metadata = create_persona(client,true,full_screen);
        else {
          std::vector<std::string> entries; for (auto& p : list) entries.push_back(p["display_name"]);
          entries.push_back("+ Create persona..."); auto selected = select_menu("Select the persona.",entries,full_screen);
          if (selected < 0) break;
          metadata = selected == static_cast<int>(list.size()) ? create_persona(client,false,full_screen) : list[selected];
        }
        if (metadata.is_null()) break;
      }
      auto backend = client.request("backend.status");
      if (!backend["configured"].get<bool>()) setup(client,full_screen);
      bool use_tui = full_screen && isatty(0) && isatty(1);
      Json activation=Json::array();
      Emit on_activate;
      if (use_tui) on_activate=[&](const std::string& type,const Json& payload){ activation.push_back({{"type",type},{"payload",payload}}); };
      Json host_environment=Json::object();
      for (auto* key : {"PATH","LANG","LC_ALL","HOME","USER","SHELL","DISPLAY","WAYLAND_DISPLAY","DBUS_SESSION_BUS_ADDRESS","XDG_RUNTIME_DIR","XAUTHORITY","SSH_AUTH_SOCK"}) if (const char* value=std::getenv(key)) host_environment[key]=value;
      client.request("persona.activate",{{"uuid",metadata["uuid"]},{"cwd",cwd.string()},{"host_environment",host_environment}},on_activate);
      Action action;
      if (use_tui) action = tui_chat(client,cwd,metadata.value("display_name","Assistant"),client.request("backend.status"),activation);
      else action = line_chat(client,cwd);
      client.request("session.close",{{"reason",action == Action::Selector ? "persona_switch" : "user_exit"}});
      if (action == Action::ModelSetup) setup(client,full_screen);
      if (action == Action::Exit || !batch.empty()) break;
    }
    return 0;
  } catch (const std::exception& e) { std::cerr << "Saga: " << e.what() << '\n'; return 1; }
}
