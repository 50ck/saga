#include "check.hpp"
#include <saga/ipc.hpp>
#include <saga/runtime.hpp>
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#define main saga_client_binary_main
#include "../src/client.cpp"
#undef main
using namespace saga;
namespace {
class MockModel {
  int listener_ = -1;
  std::jthread worker_;
  std::mutex mutex_;
  Json requests_ = Json::array();
  static void send_all(int fd,std::string_view bytes) {
    while (!bytes.empty()) {
      auto n = send(fd,bytes.data(),std::min<size_t>(37,bytes.size()),MSG_NOSIGNAL);
      if (n <= 0) return;
      bytes.remove_prefix(static_cast<size_t>(n));
    }
  }
  static Json tool(std::string name,Json args) {
    return {{"role","assistant"},{"content",nullptr},{"tool_calls",Json::array({{{"id","call_"+uuid()},{"type","function"},{"function",{{"name",name},{"arguments",args.dump()}}}}})}};
  }
  Json reply(const Json& request) {
    std::string forced;
    if (request.contains("tool_choice") && request["tool_choice"].is_object()) forced = request["tool_choice"]["function"].value("name","");
    if (request.value("tool_choice",Json()) == "required" && request["tools"].size() == 1) forced = request["tools"][0]["function"]["name"];
    if (forced == "saga_probe" && !tool_support) return {{"role","assistant"},{"content","I cannot call tools"}};
    if (forced == "saga_probe") return tool(forced,{{"ok",true}});
    if (forced == "submit_session_summary") {
      bool artifact = request["messages"].dump().find("artifact.txt") != std::string::npos;
      return tool(forced,{{"title",artifact ? "Artifact work" : "Conversation"},
        {"summary",artifact ? "Created and checked a continuity artifact" : "Spoke with the user"},
        {"narrative",artifact ? "I worked on a continuity artifact and observed its checks." : "I spoke with the user and preserved our conversation."},
        {"outcome","recorded"},{"lesson",artifact ? "Verify before claiming completion" : ""},
        {"entities",artifact ? Json::array({{{"name","artifact.txt"},{"type","file"},{"aliases",{"the artifact"}}}}) : Json::array()}});
    }
    if (forced == "submit_review") return tool(forced,{{"approved",true},{"concerns","No unresolved obligations in supplied evidence"}});
    if (forced == "submit_memory_extraction") return tool(forced,{{"facts",Json::array()},{"praxis",Json::array()}});
    const auto& messages = request.at("messages"); size_t user_at = 0; std::string input;
    for (size_t i=0; i<messages.size(); ++i) if (messages[i]["role"] == "user" && !messages[i].value("content",std::string()).starts_with("Runtime attention update (data):\n")) { user_at=i; input=messages[i].value("content",""); }
    if (input.starts_with("Create artifact")) {
      Json state = Json::object();
      for (auto& m : messages) if (m["role"] == "system" && m["content"].is_string()) {
        auto text=m["content"].get<std::string>(); std::string prefix="Persistent working state (data):\n";
        auto at=text.find(prefix); if (at != std::string::npos) state=Json::parse(text.substr(at+prefix.size()));
      }
      for(auto& m:messages)if(m["role"]=="user") {
        auto text=m.value("content","");std::string marker="Runtime attention update (data):\n";
        if(text.starts_with(marker))state.update(Json::parse(text.substr(marker.size())));
      }
      std::string last; Json result;
      for (size_t i=user_at+1; i<messages.size(); ++i) {
        if (messages[i].contains("tool_calls")) last=messages[i]["tool_calls"][0]["function"]["name"];
        if (messages[i]["role"] == "tool") result=Json::parse(messages[i].value("content","{}"));
      }
      if (last.empty()) return tool("file_write",{{"path","artifact.txt"},{"content","persistent identity artifact\n"},{"description","Continuity proof artifact"}});
      if (last == "file_write") return tool("file_read",{{"path","artifact.txt"}});
      if (last == "file_read") return tool("check_resolve",{{"check_id",state["task_checks"][0]["id"]},{"source_event_id",result["source_event_id"]},{"passed",true},{"explanation","Read verified expected artifact contents"}});
      if (last == "check_resolve") return tool("task_update",{{"id",state["active_task"][0]["id"]},{"status","completed"}});
      return {{"role","assistant"},{"content","Artifact created and verified."}};
    }
    return {{"role","assistant"},{"content",lower(input).find("remember") != std::string::npos ? "I retrieved artifact work from autobiographical memory." : "Hello from the mock reasoning engine."}};
  }
  void handle(int fd) {
    std::string raw; char buffer[4096]; size_t header_end;
    while ((header_end=raw.find("\r\n\r\n")) == std::string::npos) { auto n=recv(fd,buffer,sizeof buffer,0); if (n<=0) return; raw.append(buffer,static_cast<size_t>(n)); if (raw.size()>1024*1024) return; }
    auto headers=raw.substr(0,header_end); size_t length=0; auto at=lower(headers).find("content-length:");
    if (at != std::string::npos) length=std::stoull(headers.substr(at+15));
    while (raw.size()-header_end-4<length) { auto n=recv(fd,buffer,sizeof buffer,0); if (n<=0) return; raw.append(buffer,static_cast<size_t>(n)); }
    auto first=headers.substr(0,headers.find("\r\n")); auto request=length ? Json::parse(raw.substr(header_end+4,length)) : Json::object();
    { std::lock_guard lock(mutex_); requests_.push_back({{"line",first},{"headers",headers},{"body",request}}); }
    auto response=[&](std::string body,std::string type="application/json",int code=200) { send_all(fd,"HTTP/1.1 "+std::to_string(code)+" OK\r\nContent-Type: "+type+"\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n"+body); };
    if (first.starts_with("GET ") && first.find("/models ") != std::string::npos) { Json m={{"id","mock-model"}}; if (metadata) m["meta"]={{"n_ctx",context.load()}}; response(Json{{"data",Json::array({m})}}.dump()); return; }
    if (first.starts_with("GET ") && first.find("/props ") != std::string::npos) { if (props) response(Json{{"default_generation_settings",{{"n_ctx",context.load()}}}}.dump()); else response("{}","application/json",404); return; }
    if (first.find("/chat/completions ") == std::string::npos) { response("{}","application/json",404); return; }
    if (reject_usage && request.contains("stream_options")) { response("{}","application/json",400); return; }
    if (pause.exchange(false)) {
      waiting=true; auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
      while (!resume && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      waiting=false;
    }
    auto message=reply(request); std::string reason=message.contains("tool_calls") ? "tool_calls" : "stop";
    if (!request.value("stream",false)) { response(Json{{"choices",Json::array({{{"index",0},{"message",message},{"finish_reason",reason}}})}}.dump()); return; }
    std::string body;
    auto chunk=[&](Json delta,Json finish=Json()) { body+="data: "+Json{{"choices",Json::array({{{"index",0},{"delta",delta},{"finish_reason",finish}}})}}.dump()+"\r\n\r\n"; };
    if (message.contains("tool_calls")) {
      auto call=message["tool_calls"][0]; auto args=call["function"]["arguments"].get<std::string>(); auto first_call=call;
      first_call["index"]=0; first_call["function"]["arguments"]=args.substr(0,args.size()/2); chunk({{"tool_calls",Json::array({first_call})}});
      chunk({{"tool_calls",Json::array({{{"index",0},{"function",{{"arguments",args.substr(args.size()/2)}}}}})}},reason);
    } else { auto content=message.value("content",""); chunk({{"content",content.substr(0,content.size()/2)}}); chunk({{"content",content.substr(content.size()/2)}},reason); }
    body+="data: "+Json{{"choices",Json::array()},{"usage",{{"prompt_tokens",123},{"completion_tokens",21}}}}.dump()+"\n\n";
    if (!truncated) body+="data: [DONE]\n\n";
    response(body,"text/event-stream");
  }
public:
  std::atomic<std::uint64_t> context=65536;
  std::atomic_bool metadata=true,props=true,tool_support=true,reject_usage=false,truncated=false;
  std::atomic_bool pause=false,waiting=false,resume=false;
  std::string endpoint;
  MockModel() {
    listener_=socket(AF_INET,SOCK_STREAM | SOCK_CLOEXEC,0); CHECK(listener_>=0);
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(bind(listener_,reinterpret_cast<sockaddr*>(&addr),sizeof addr)==0); CHECK(listen(listener_,16)==0);
    socklen_t size=sizeof addr; CHECK(getsockname(listener_,reinterpret_cast<sockaddr*>(&addr),&size)==0);
    endpoint="http://127.0.0.1:"+std::to_string(ntohs(addr.sin_port));
    worker_=std::jthread([this](std::stop_token stop) { while (!stop.stop_requested()) {
      pollfd p{listener_,POLLIN,0}; if (poll(&p,1,50)<=0) continue;
      int fd=accept4(listener_,nullptr,nullptr,SOCK_CLOEXEC); if (fd<0) continue;
      timeval timeout{3,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
      try { handle(fd); } catch (const std::exception&) {} close(fd);
    }});
  }
  ~MockModel() { worker_.request_stop(); worker_.join(); close(listener_); }
  Json requests() { std::lock_guard lock(mutex_); return requests_; }
};
struct Environment {
  fs::path root=fs::temp_directory_path()/("saga-integration-"+uuid());
  Paths paths{root/"config/saga",root/"data/saga",root/"state/saga",root/"run/saga"};
  fs::path project=root/"project";
  pid_t daemon=-1;
  Environment() {
    paths.create(); private_dir(project);
    setenv("XDG_CONFIG_HOME",(root/"config").c_str(),1); setenv("XDG_DATA_HOME",(root/"data").c_str(),1);
    setenv("XDG_STATE_HOME",(root/"state").c_str(),1); setenv("XDG_RUNTIME_DIR",(root/"run").c_str(),1); unsetenv("SAGA_API_KEY");
  }
  void start(const std::string& executable) {
    daemon=fork(); CHECK(daemon>=0);
    if (!daemon) { chdir(project.c_str()); setpgid(0,0); int log=open((root/"daemon.log").c_str(),O_CREAT | O_WRONLY | O_TRUNC,0600); dup2(log,1); dup2(log,2); close(log); execl(executable.c_str(),executable.c_str(),static_cast<char*>(nullptr)); _exit(127); }
    for (int i=0; i<200; ++i) { try { auto c=Channel::connect(paths.socket()); return; } catch (const std::exception&) {} std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    throw std::runtime_error("Daemon start failed: "+read_file(root/"daemon.log"));
  }
  void stop() {
    if (daemon<=0) return;
    kill(daemon,SIGTERM); int status;
    for (int i=0; i<500; ++i) { if (waitpid(daemon,&status,WNOHANG)==daemon) { daemon=-1; return; } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    kill(daemon,SIGKILL); waitpid(daemon,&status,0); daemon=-1;
  }
  ~Environment() { stop(); fs::remove_all(root); }
};
struct Peer {
  std::unique_ptr<Channel> channel;
  Json frames=Json::array();
  explicit Peer(const fs::path& path) {
    try { channel=Channel::connect(path); }
    catch (const std::exception& e) { throw std::runtime_error("Peer connection: "+std::string(e.what())); }
  }
  Json request(std::string type,Json payload=Json::object(),bool approve=true) {
    auto id=uuid(); channel->send({{"type",type},{"request_id",id},{"payload",payload}});
    for (int i=0; i<500; ++i) {
      std::optional<Json> f;
      try { f=channel->receive(10000); }
      catch (const std::exception& e) { throw std::runtime_error("Request "+type+": "+e.what()); }
      CHECK(f.has_value()); frames.push_back(*f); auto t=f->at("type").get<std::string>(); auto p=f->at("payload");
      if (t=="error") throw std::runtime_error(p.value("message","Protocol error"));
      if (t=="result") return p;
      if (t=="approval.requested") channel->send({{"type","approval"},{"request_id",id},{"payload",{{"approval_id",p["approval_id"]},{"approved",approve}}}});
    }
    throw std::runtime_error("Too many frames");
  }
  Json command(std::string name,Json args=Json::object()) { return request("command",{{"name",name},{"arguments",args}}); }
};
void model_contracts(MockModel& mock) {
  Config c; c.endpoint=mock.endpoint+"/v1/"; OpenAICompatibleBackend backend(c);
  auto m=backend.discover(); CHECK(m.id=="mock-model"); CHECK(m.context_length==65536); CHECK(backend.probe().tool_calls);
  mock.metadata=false; CHECK(OpenAICompatibleBackend(c).discover().context_length==65536);
  mock.props=false; CHECK(!OpenAICompatibleBackend(c).discover().context_length); mock.metadata=true; mock.props=true;
  mock.tool_support=false; rejects([&]{ backend.probe(); }); mock.tool_support=true;
  mock.reject_usage=true; CHECK(backend.probe().streaming); mock.reject_usage=false;
  mock.truncated=true; ChatRequest r; r.messages=Json::array({{{"role","user"},{"content","hello"}}}); rejects([&]{ backend.chat(r,[](auto&){}); }); mock.truncated=false;
  c.api_key="test-key"; OpenAICompatibleBackend(c).discover();
  CHECK(mock.requests().back()["headers"].get<std::string>().find("Authorization: Bearer test-key")!=std::string::npos);
  for (auto& request:mock.requests()) CHECK(request["line"].get<std::string>().find("/v1/v1")==std::string::npos);
}
void end_to_end(MockModel& mock,const std::string& daemon,const std::string& client) {
  Environment env; env.start(daemon); Peer peer(env.paths.socket());
  CHECK(peer.request("ping")["runtime"]=="Saga"); CHECK(peer.request("personas.list").empty());
  auto a=peer.request("personas.create",{{"name","Identity A — 東京"},{"soul","My core identity A."}});
  auto b=peer.request("personas.create",{{"name","Identity B"},{"soul","My separate identity B."}});
  mock.context=32768; rejects([&]{ peer.request("backend.setup",{{"endpoint",mock.endpoint}}); });
  CHECK(!peer.request("backend.status")["configured"].get<bool>()); mock.context=65536;
  auto before_setup=mock.requests().size();
  auto setup=peer.request("backend.setup",{{"endpoint",mock.endpoint}});
  CHECK(setup["capabilities"]["probed"] == false); CHECK(setup["capabilities"]["tool_calls"].is_null());
  auto setup_requests=mock.requests();
  for (size_t i=before_setup; i<setup_requests.size(); ++i) CHECK(setup_requests[i]["line"].get<std::string>().starts_with("GET "));
  peer.request("persona.activate",{{"uuid",a["uuid"]},{"cwd",env.project.string()}}); rejects([&]{ peer.request("personas.list"); });
  Peer concurrent(env.paths.socket()); rejects([&]{ concurrent.request("persona.activate",{{"uuid",a["uuid"]},{"cwd",env.project.string()}}); });
  peer.request("chat",{{"content","Create artifact for continuity"}}); CHECK(read_file(env.project/"artifact.txt")=="persistent identity artifact\n");
  CHECK(peer.command("tasks")[0]["status"]=="completed");
  CHECK(!peer.command("memory",{{"kind","recall_artifact"},{"query","continuity"}})["results"].empty());
  peer.command("fact",{{"subject","machine"},{"predicate","OS"},{"object","FreeBSD"}});
  peer.request("session.close",{{"reason","persona_switch"}});
  { Database db(env.paths.persona(a["uuid"].get<std::string>())/"agent.db"); CHECK(db.query("SELECT * FROM journal_entries").size()==1); CHECK(db.query("SELECT * FROM tool_runs").size()>=5); CHECK(db.query("SELECT * FROM model_calls WHERE approximate=0").size()>=5); CHECK(db.query("SELECT end_reason FROM sessions")[0]["end_reason"]=="persona_switch"); }
  peer.request("persona.activate",{{"uuid",b["uuid"]},{"cwd",env.project.string()}});
  CHECK(peer.command("memory",{{"kind","know"},{"query","machine"}})["results"].empty()); CHECK(peer.command("journal").empty()); CHECK(peer.command("soul")["content"]=="My separate identity B.");
  auto other=(env.paths.persona(a["uuid"].get<std::string>())/"SOUL.md").string();
  CHECK(peer.command("tool",{{"name","file_read"},{"arguments",{{"path",other}}}}).contains("error")); peer.request("chat",{{"content","hello"}});
  for (auto& request:mock.requests()) if (request["body"].contains("messages") && request["body"]["messages"].dump().find("Identity name: Identity B")!=std::string::npos) { auto context=request["body"]["messages"].dump(); CHECK(context.find("My core identity A.")==std::string::npos); CHECK(context.find("Identity A —")==std::string::npos); }
  peer.request("session.close");
  {
    // Use the production client while HTTP inference sends no bytes at all.
    Client live{Channel::connect(env.paths.socket()),false};
    live.request("persona.activate",{{"uuid",b["uuid"]},{"cwd",env.project.string()}});
    mock.resume=false; mock.pause=true;
    std::atomic_bool done=false; std::atomic_int replies=0; std::exception_ptr error;
    std::jthread inference([&]{try {live.request("chat",{{"content","hello live controls"}},[](auto&,auto&){});} catch (...) {error=std::current_exception();} done=true;});
    struct Resume { MockModel& mock; ~Resume(){mock.resume=true;} } resume{mock};
    auto wait=[&](const std::function<bool()>& ready){auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(!ready() && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(10));CHECK(ready());};
    wait([&]{return mock.waiting.load();});
    live.live_request({{"name","permissions"},{"arguments",{{"mode","host_ask"}}}},[&](const std::string& type,const Json& p){CHECK(type=="result" && p["mode"]=="host_ask");++replies;});
    live.live_request({{"name","status"}},[&](const std::string& type,const Json& p){CHECK(type=="result" && p["permissions"]["mode"]=="host_ask");CHECK(p["usage"]["output_tokens"]==0);++replies;});
    live.live_request({{"name","tasks"}},[&](const std::string& type,const Json& p){CHECK(type=="result" && p.is_array());++replies;});
    wait([&]{return replies==3;}); CHECK(!done && mock.waiting);
    rejects([&]{local_command(live,"/new",env.project,[](auto&,auto&){},true);});
    local_command(live,"/permissions 4",env.project,[&](const std::string& type,const Json& p){if(type=="permissions.changed"){CHECK(p["mode"]=="host_always");++replies;}},true);
    wait([&]{return replies==4;}); CHECK(!done);
    mock.resume=true; inference.join(); if(error)std::rethrow_exception(error);
    CHECK(live.request("command",{{"name","permissions"}})["mode"]=="host_always");
    // Cancel during prompt preparation, while the endpoint sends no SSE bytes.
    done=false;error=nullptr;mock.resume=false;mock.pause=true;std::atomic_bool stop_ack=false,cancel_event=false;
    std::jthread waiting_turn([&]{try{live.request("chat",{{"content","hello cancellation"}},[&](const std::string& type,const Json&){if(type=="turn.cancelled")cancel_event=true;});}catch(...){error=std::current_exception();}done=true;});
    wait([&]{return mock.waiting.load();});
    live.live_request({{"name","stop"}},[&](const std::string& type,const Json& data){if(type=="result"){CHECK(data["stopping"]==true);stop_ack=true;}});
    wait([&]{return done.load();});waiting_turn.join();if(error)std::rethrow_exception(error);CHECK(stop_ack && cancel_event);mock.resume=true;
    wait([&]{return !mock.waiting.load();});
    // A live steering request arriving after completion still forwards its turn events.
    std::atomic_bool steer_reply=false,steer_completed=false;
    local_command(live,"/steer Continue the same conversation",env.project,[&](const std::string& type,const Json&){if(type=="assistant.completed")steer_completed=true;},true);
    auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!steer_completed && std::chrono::steady_clock::now()<until){auto frame=live.channel->receive(100);if(frame){if((*frame)["type"]=="result")steer_reply=true;live.dispatch_control(*frame);}}
    CHECK(steer_completed);while(!steer_reply && std::chrono::steady_clock::now()<until){auto frame=live.channel->receive(100);if(frame){if((*frame)["type"]=="result")steer_reply=true;live.dispatch_control(*frame);}}CHECK(steer_reply);
    live.request("session.close");
  }
  env.stop(); env.start(daemon); Peer again(env.paths.socket());
  again.request("persona.activate",{{"uuid",a["uuid"]},{"cwd",env.project.string()}}); again.request("chat",{{"content","Do you remember the artifact we made?"}});
  CHECK(!again.command("memory",{{"query","artifact"},{"deep",true}})["results"].empty()); CHECK(again.command("memory",{{"kind","know"},{"query","machine"}})["results"][0]["object"]=="FreeBSD");
  again.command("new"); CHECK(again.command("soul")["content"]=="My core identity A."); again.request("session.close");
  int pipe_fd[2]; CHECK(pipe(pipe_fd)==0); pid_t cli=fork(); CHECK(cli>=0);
  if (!cli) { chdir(env.project.c_str()); dup2(pipe_fd[0],0); close(pipe_fd[0]); close(pipe_fd[1]); int out=open((env.root/"cli.out").c_str(),O_CREAT | O_WRONLY | O_TRUNC,0600); dup2(out,1); dup2(out,2); close(out); auto id=a["uuid"].get<std::string>(); execl(client.c_str(),client.c_str(),"--batch",id.c_str(),"--no-tui",static_cast<char*>(nullptr)); _exit(127); }
  close(pipe_fd[0]); const char* input="/permissions 3\n/status\n/permissions 4\n/status\n/permissions ask\nhello\n/journal\n/exit\n"; CHECK(write(pipe_fd[1],input,strlen(input))>0); close(pipe_fd[1]); int status; CHECK(waitpid(cli,&status,0)==cli); CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
  auto cli_output=read_file(env.root/"cli.out"); CHECK(cli_output.find("Hello from the mock")!=std::string::npos);
  CHECK(cli_output.find("Ask for unrestricted host operations")!=std::string::npos);
  CHECK(cli_output.find("Unrestricted host without approval (DANGEROUS)")!=std::string::npos);
  bool saw_no_key=false; for (auto& request:mock.requests()) if (request["headers"].get<std::string>().find("Authorization:")==std::string::npos) saw_no_key=true; CHECK(saw_no_key);
}
}
int main(int argc,char** argv) {
  int probe = socket(AF_INET,SOCK_STREAM | SOCK_CLOEXEC,0);
  if (probe < 0 && (errno == EPERM || errno == EACCES)) { std::cout << "SKIP HTTP/daemon integration: environment denies local TCP sockets\n"; return 77; }
  if (probe >= 0) close(probe);
  try { CHECK(argc==3); MockModel mock; model_contracts(mock); std::cout << "PASS HTTP probes, context detection, SSE fragments, API keys, usage fallback and interrupted streams\n"; end_to_end(mock,fs::canonical(argv[1]).string(),fs::canonical(argv[2]).string()); std::cout << "PASS daemon and CLI: creation, setup, tools, approvals, proof gates, journals, switching, isolation and restart recall\n"; return 0; }
  catch (const std::exception& e) { std::cerr << "FAIL integration: " << e.what() << '\n'; return 1; }
}
