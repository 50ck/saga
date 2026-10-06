#include "check.hpp"
#include <saga/runtime.hpp>
#include <saga/edit.hpp>
#include <saga/ipc.hpp>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <unistd.h>
using namespace saga;
namespace {
struct Fixture {
  fs::path root = fs::temp_directory_path() / ("saga-tests-" + uuid());
  Paths paths{root / "config",root / "data",root / "state",root / "runtime"};
  fs::path project = root / "project";
  Fixture() { paths.create(); private_dir(project); }
  ~Fixture() { fs::remove_all(root); }
  Config config() { Config c; c.endpoint = "http://127.0.0.1:9999"; c.model = "test-model"; c.context_length = 65536; return c; }
  std::unique_ptr<PersonaContext> persona(Json meta) { return std::make_unique<PersonaContext>(paths,meta,project); }
};
void nested_research_context() {
  Fixture fixture;Registry registry(fixture.paths);auto p=fixture.persona(registry.create("Context fixture",""));
  auto& db=*p->db;p->session=db.exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()});
  Json user={{"role","user"},{"content","Research modern rotation rules before implementing the terminal game."}};
  db.exec("INSERT INTO messages(session_id,ts,role,content_json) VALUES(?,?,'user',?)",{p->session,now(),user.dump()});
  Json args={{"query","SRS modern tetris rotation rules"}},calls=Json::array({{{"id","search-call"},{"type","function"},{"function",{{"name","web_search"},{"arguments",args.dump()}}}}});
  Json assistant={{"role","assistant"},{"content",nullptr},{"tool_calls",calls}};
  db.exec("INSERT INTO messages(session_id,ts,role,content_json) VALUES(?,?,'assistant',?)",{p->session,now(),assistant.dump()});
  Json observation={{"results",Json::array()},{"next_cursor",std::string(8192,'x')}};
  for(int i=0;i<10;++i)observation["results"].push_back({{"title",std::string(512,'t')},{"snippet",std::string(2048,'s')},{"url","https://docs.example.org/"+std::to_string(i)}});
  auto event=db.event("tool.completed",{{"tool","web_search"},{"result",observation}},p->session);observation["source_event_id"]=event;
  Json tool={{"role","tool"},{"tool_call_id","search-call"},{"content",observation.dump()}};
  db.exec("INSERT INTO messages(session_id,ts,role,content_json) VALUES(?,?,'tool',?)",{p->session,now(),tool.dump()});
  Memory memory(*p,fixture.config());ContextBuilder context(*p,memory,fixture.config());auto request=context.build();
  CHECK(estimate_tokens(request.messages.dump())+estimate_tokens(request.tools.dump())<fixture.config().input_budget());
  auto found=std::find_if(request.messages.begin(),request.messages.end(),[](auto& m){return m["role"]=="tool";});CHECK(found!=request.messages.end());
  auto projected=Json::parse((*found)["content"].get<std::string>());CHECK(projected["context_excerpt"]==true);CHECK(projected["source_event_id"]==event);CHECK(projected.dump().size()<17000);
  CHECK(Json::parse(db.query("SELECT content_json FROM messages WHERE session_id=? AND role='tool'",{p->session})[0]["content_json"].get<std::string>())["content"]==observation.dump());
  CHECK(estimate_tokens(std::string(16000,'a'))<16000);
}
void streaming() {
  std::vector<std::string> frames;
  SseParser parser([&](std::string_view s){ frames.emplace_back(s); });
  std::string source = ": heartbeat\r\ndata: one\r\ndata: two\r\n\r\ndata: [DONE]\n\n";
  for (char c : source) parser.feed(std::string_view(&c,1));
  parser.finish();
  CHECK(frames.size() == 2); CHECK(frames[0] == "one\ntwo"); CHECK(frames[1] == "[DONE]");
  std::string public_json=Json{{"text","**Public** 東京 \"quoted\" 😀\nNext"}}.dump();
  for(size_t end=1;end<=public_json.size();++end){auto text=json_string_prefix(std::string_view(public_json).substr(0,end),"text");CHECK(Json::accept(Json(text).dump()));CHECK(std::string("**Public** 東京 \"quoted\" 😀\nNext").starts_with(text));}
  CHECK(json_string_prefix("{\"text\":\"\\uD83D\\uDE00\"}","text")=="😀");
  CHECK(json_string_prefix("{\"nested\":{\"text\":\"private\"},\"text\":\"public\"}","text")=="public");
  GenerationState result; OpenAIStreamAdapter result_adapter([&](const ProviderEvent& event){result.accept(event);});
  result_adapter.feed(Json::parse(R"({"choices":[{"index":0,"delta":{"content":"hello","tool_calls":[{"index":0,"id":"call_1","function":{"name":"remember","arguments":"{\"que"}}]},"finish_reason":null}]})"));
  result_adapter.feed(Json::parse(R"({"choices":[{"index":0,"delta":{"content":" world","tool_calls":[{"index":0,"function":{"arguments":"ry\":\"widget\"}"}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":50}})"));
  auto message = result.message(); CHECK(result.content == "hello world");
  result_adapter.feed({{"choices",Json::array({{{"delta",{{"reasoning_content","thought "}}}}})}});
  result_adapter.feed({{"choices",Json::array({{{"delta",{{"reasoning_content","continued"}}}}})}});
  CHECK(result.message()["reasoning_content"]=="thought continued");
  CHECK(Json::parse(message["tool_calls"][0]["function"]["arguments"].get<std::string>())["query"] == "widget"); CHECK(result.usage["input_tokens"] == 50);
  GenerationState live; OpenAIStreamAdapter live_adapter([&](const ProviderEvent& event){live.accept(event);});
  live_adapter.feed({{"prompt_progress",{{"total",10000},{"processed",1500},{"cache",8500}}}});CHECK(live.input_tokens==10000 && live.cache_tokens==8500);
  live_adapter.feed({{"timings",{{"prompt_n",1500},{"cache_n",8500},{"predicted_n",12}}}});CHECK(live.input_tokens==10000 && live.output_tokens==12);
  live_adapter.feed({{"timings",{{"prompt_n",1500},{"cache_n",8500},{"predicted_n",19}}}});CHECK(live.input_tokens==10000 && live.output_tokens==19);
  live_adapter.feed({{"timings",{{"prompt_n",1500},{"cache_n",8500},{"predicted_n",20}}},{"usage",{{"prompt_tokens",10000},{"completion_tokens",20},{"prompt_tokens_details",{{"cached_tokens",8500}}}}}});CHECK(live.input_tokens==10000 && live.output_tokens==20);
  live_adapter.feed({{"timings",{{"prompt_n",-1},{"cache_n",8500},{"predicted_n",nullptr}}},{"usage",nullptr}});CHECK(live.input_tokens==10000 && live.output_tokens==20);
  live_adapter.feed({{"usage",{{"prompt_tokens_details",{{"cached_tokens",0}}}}}});CHECK(live.cache_tokens==0);
  CHECK(OpenAICompatibleBackend::normalize("https://example.test/v1/").second == "https://example.test/v1");
  CHECK(OpenAICompatibleBackend::normalize("http://localhost:8000").second == "http://localhost:8000/v1");
  rejects([]{ OpenAICompatibleBackend::normalize("file:///etc/passwd"); });
  rejects([]{ OpenAICompatibleBackend::normalize("https://user:secret@host/v1"); });
  rejects([]{ OpenAICompatibleBackend::normalize("https://host/v1/v1"); });
  CHECK(base64("f") == "Zg=="); CHECK(base64("fo") == "Zm8="); CHECK(base64("foo") == "Zm9v");
  CHECK(base64(std::string(1,static_cast<char>(255))) == "/w==");
  CHECK(utf8_text(std::string(1,static_cast<char>(255))) == "�");
  CHECK(utf8_excerpt("éé",3) == "é"); CHECK(utf8_excerpt("éé",3,true) == "é");
}
class ProbeEndpoint : public OpenAICompatibleBackend {
  bool llama_;
protected:
  Json request(std::string_view method,const std::string& url,const Json& body,StreamCallback callback) override {
    if (method == "GET") return {{"data",Json::array({{{"id","probe-model"},{"owned_by",llama_ ? "llamacpp" : "generic"},{"meta",{{"n_ctx",65536ULL}}}}})}};
    requests.push_back(body);
    if(url.ends_with("/control"))return {{"success",true}};
    if (callback) {
      if(reasoning_fixture)callback({{"id","fixture-completion"},{"choices",Json::array({{{"index",0},{"delta",{{"reasoning_content","hidden reasoning"}}},{"finish_reason",nullptr}}})}});
      callback({{"choices",Json::array({{{"index",0},{"delta",{{"content","OK"}}},{"finish_reason","stop"}}})}});
      return Json::object();
    }
    Json message = {{"role","assistant"},{"content","OK"}};
    if (body.contains("tools") && tool_support) message = {{"role","assistant"},{"content",nullptr},{"tool_calls",Json::array({{{"id","probe-call"},{"type","function"},{"function",{{"name","saga_probe"},{"arguments","{\"ok\":true}"}}}}})}};
    return {{"choices",Json::array({{{"index",0},{"message",message},{"finish_reason",message.contains("tool_calls") ? "tool_calls" : "stop"}}})}};
  }
public:
  Json requests = Json::array(); bool tool_support = true,reasoning_fixture=false;
  explicit ProbeEndpoint(bool llama) : OpenAICompatibleBackend([]{ Config c; c.endpoint="http://probe.test/v1"; c.model="probe-model"; return c; }()),llama_(llama) {}
};
void capability_probe() {
  ProbeEndpoint llama(true); Json progress = Json::array();
  auto report = llama.probe([&](const std::string& type,const Json& payload){ CHECK(type == "backend.progress"); progress.push_back(payload); });
  CHECK(report.completion && report.tool_calls && report.streaming); CHECK(progress.size() == 6);
  CHECK(progress[0]["stage"] == "completion" && progress[0]["status"] == "running");
  CHECK(progress[5]["stage"] == "streaming" && progress[5]["status"] == "passed");
  CHECK(llama.requests.size() == 3);
  CHECK(llama.requests[0]["max_tokens"] == 32); CHECK(llama.requests[1]["max_tokens"] == 64);
  CHECK(llama.requests[1]["tool_choice"] == "required");
  for (const auto& body : llama.requests) { CHECK(body["chat_template_kwargs"]["enable_thinking"] == false); CHECK(body["reasoning_effort"] == "none"); }
  ChatRequest chat; chat.messages = Json::array({{{"role","user"},{"content","Ordinary work"}}});
  llama.generate(chat,[](const ProviderEvent&){});
  CHECK(llama.requests.back()["chat_template_kwargs"]["preserve_thinking"]==true);CHECK(!llama.requests.back()["chat_template_kwargs"].contains("enable_thinking")); CHECK(!llama.requests.back().contains("reasoning_effort"));
  CHECK(llama.requests.back()["cache_prompt"]==true);
  CHECK(llama.requests.back()["timings_per_token"]==true && llama.requests.back()["return_progress"]==true);
  chat.forced_tool = "submit_review";
  chat.tools = Json::array({function_tool("submit_review","Review",Json::object()),function_tool("unused","Unused",Json::object())});
  llama.generate(chat,[](const ProviderEvent&){});
  CHECK(llama.requests.back()["tool_choice"] == "required"); CHECK(llama.requests.back()["tools"].size() == 1);
  CHECK(llama.requests.back()["tools"][0]["function"]["name"] == "submit_review");
  chat.forced_tool = "missing"; auto sent = llama.requests.size();
  rejects([&]{ llama.generate(chat,[](const ProviderEvent&){}); }); CHECK(llama.requests.size() == sent);
  ProbeEndpoint generic(false); CHECK(generic.probe().tool_calls);
  ChatRequest normal;normal.messages=Json::array({{{"role","user"},{"content","normal"}}});generic.generate(normal,[](const ProviderEvent&){});CHECK(!generic.requests.back().contains("timings_per_token"));
  for (const auto& body : generic.requests) { CHECK(!body.contains("chat_template_kwargs")); CHECK(!body.contains("reasoning_effort")); }
  ProbeEndpoint controlled(true);controlled.reasoning_fixture=true;
  normal.enable_reasoning_control=true;normal.reasoning_soft_tokens=1;normal.reasoning_hard_tokens=1;
  GenerationState state;int warnings=0;
  controlled.generate(normal,[&](const ProviderEvent& e){state.accept(e);if(e.kind==ProviderEventKind::Warning)++warnings;});
  CHECK(state.status==GenerationStatus::Completed && state.reasoning=="hidden reasoning" && warnings==2);
  CHECK(controlled.capabilities().reasoning_control);
  CHECK(std::count_if(controlled.requests.begin(),controlled.requests.end(),[](auto& body){return body.contains("action") && body["action"]=="reasoning_end";})==2);
  CHECK(controlled.requests[1]["stream"]==true && controlled.requests[1]["reasoning_control"]==true);
  ProbeEndpoint incompatible(true); incompatible.tool_support = false;
  rejects([&]{ incompatible.probe(); }); CHECK(incompatible.requests.size() == 2);
}
void persistence() {
  Fixture f; Registry registry(f.paths);
  auto a = registry.create("Name with spaces — 東京", "Identity A"), b = registry.create("Other identity", "Identity B");
  CHECK(registry.list().size() == 2); CHECK(valid_uuid(a["uuid"].get<std::string>()));
  auto p = f.persona(a); CHECK(p->soul == "Identity A");
  rejects([&]{ f.persona(a); });
  auto q = f.persona(b); CHECK(q->soul == "Identity B");
  auto db = p->db.get(); db->migrate(); CHECK(db->query("SELECT version FROM schema_version")[0]["version"] == 10);
  auto id = db->event("test.event",{{"text","immutable"}});
  Json single = id;
  CHECK(db->query("SELECT type FROM events WHERE id=?",{single})[0]["type"] == "test.event");
  rejects([&]{ db->exec("UPDATE events SET type='changed' WHERE id=?",{id}); });
  rejects([&]{ db->exec("DELETE FROM events WHERE id=?",{id}); });
  CHECK(q->db->query("SELECT * FROM events WHERE type='test.event'").empty());
  q->db->sql("ALTER TABLE facts DROP COLUMN project_id; UPDATE schema_version SET version=1");
  q->db->migrate();
  CHECK(q->db->query("SELECT version FROM schema_version")[0]["version"] == 10);
  bool scoped_column = false;
  for (const auto& column : q->db->query("PRAGMA table_info(facts)")) if (column["name"] == "project_id") scoped_column = true;
  CHECK(scoped_column);
  rejects([&]{ db->transaction([&]{ db->event("rollback",{}); throw std::runtime_error("abort"); }); });
  CHECK(db->query("SELECT * FROM events WHERE type='rollback'").empty());
  Memory memory(*p,f.config());
  Id source = db->event("user.message",{{"content","workstation uses Debian"}});
  auto old = memory.fact("workstation","operating_system","Debian",source);
  source = db->event("user.message",{{"content","No, workstation uses FreeBSD, not Debian"}});
  auto next = memory.fact("workstation","operating_system","FreeBSD",source,true);
  CHECK(old != next); CHECK(db->query("SELECT * FROM facts WHERE id=?",{old})[0]["superseded_by"] == next);
  CHECK(db->query("SELECT * FROM corrections").size() == 1);
  CHECK(memory.fact("workstation","operating_system","FreeBSD",source) == next);
  auto confidence = memory.confidence("fact",next);
  memory.fact("workstation","operating_system","FreeBSD",source);
  CHECK(memory.confidence("fact",next) == confidence);
  auto current = memory.search("know","workstation"); CHECK(current["results"].size() == 1); CHECK(current["results"][0]["object"] == "FreeBSD");
  CHECK(memory.search("know","workstation",true)["results"].size() == 2);
  CHECK(memory.search("know","\" OR 1=1 --")["results"].is_array());
  auto project_a = db->exec("INSERT INTO projects(name,root_path,created_at,last_seen_at) VALUES('A','/project-a',?,?)",{now(),now()});
  auto project_b = db->exec("INSERT INTO projects(name,root_path,created_at,last_seen_at) VALUES('B','/project-b',?,?)",{now(),now()});
  source = db->event("user.message",{{"content","Build A with CMake and B with Meson"}});
  auto scoped_a = memory.fact("build","system","CMake",source,false,project_a);
  auto scoped_b = memory.fact("build","system","Meson",source,false,project_b);
  CHECK(scoped_a != scoped_b); CHECK(db->query("SELECT valid_to FROM facts WHERE id=?",{scoped_a})[0]["valid_to"].is_null());
  p->project = project_a;
  auto scoped = memory.search("know","build")["results"]; CHECK(scoped.size() == 1); CHECK(scoped[0]["object"] == "CMake");
  p->project = project_b; scoped = memory.search("know","build")["results"]; CHECK(scoped.size() == 1); CHECK(scoped[0]["object"] == "Meson");
  CHECK(memory.search("know","build",true)["results"].size() == 2);
  memory.evidence("fact",next,"another interpretation of the same statement",true,source,1,0.75);
  auto once = memory.confidence("fact",next);
  memory.evidence("fact",next,"a redundant interpretation",true,source,1,0.75);
  CHECK(memory.confidence("fact",next) == once);
  Id session = db->exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()});
  auto episode = memory.episode({{"title","X11 widget scaling"},{"summary","Reproduced under Xvfb; DPI was the cause"},{"content","Captured and compared screenshots"}},session);
  memory.entity_links(episode,Json::array({{{"name","XLibre"},{"type","software"},{"aliases",Json::array({"the display server"})}}}));
  CHECK(!memory.search("remember","Xvfb")["results"].empty());
  CHECK(!memory.search("remember","the display server",true)["results"].empty());
  memory.maintain(); CHECK(db->query("SELECT accessibility FROM episodes")[0]["accessibility"].get<double>() > 0);
  p.reset(); p = f.persona(a); CHECK(p->db->query("SELECT status FROM sessions WHERE id=?",{session})[0]["status"] == "needs_consolidation");
  rejects([&]{ f.paths.persona("../../escape"); });
  struct stat st{}; stat((f.paths.data / "registry.db").c_str(),&st); CHECK((st.st_mode & 0777) == 0600);
}
void host_permissions() {
  Fixture f; Registry registry(f.paths); auto meta=registry.create(); auto p=f.persona(meta);
  p->project=p->db->exec("INSERT INTO projects(name,root_path,created_at,last_seen_at) VALUES('host test',?,?,?)",{f.project.string(),now(),now()});
  p->session=p->db->exec("INSERT INTO sessions(started_at,project_id,status) VALUES(?,?,'active')",{now(),p->project});
  Memory memory(*p,f.config()); int approvals=0; bool approve=false;
  Tools policy(*p,memory,[&](auto&,auto&){ ++approvals; return approve; });
  auto shell=[&](std::string command,std::string execution="host") {
    return policy.execute("shell_exec",{{"command",command},{"execution",execution}});
  };
  auto file=f.root/"host-created.html";
  auto create="printf '<h1>Host write</h1>' > '"+file.string()+"'";
  CHECK(policy.permissions()["mode"] == "ask_always");
  rejects([&]{ policy.permissions("unknown"); });
  CHECK(policy.permissions("host_ask")["host_restrictions"] == "none");
  CHECK(shell(create).contains("error")); CHECK(approvals == 1); CHECK(!fs::exists(file));
  approve=true;
  auto result=shell(create); CHECK(result["exit_code"] == 0); CHECK(approvals == 2);
  CHECK(result["host_restrictions"] == "none"); CHECK(read_file(file) == "<h1>Host write</h1>");
  CHECK(policy.permissions("host_always")["approval_required"] == false);
  CHECK(shell("printf automatic >> '"+file.string()+"'")["exit_code"] == 0); CHECK(approvals == 2);
  // Unrestricted execution inherits the host's setting; Saga must not add no_new_privs.
  auto nnp=prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0); CHECK(nnp >= 0);
  result=shell("cat /proc/self/status"); CHECK(result["exit_code"] == 0);
  CHECK(result["stdout"].get<std::string>().find("NoNewPrivs:\t"+std::to_string(nnp)) != std::string::npos);
  setenv("SAGA_HOST_FIXTURE","inherited-environment",1);
  result=shell("printf '%s' \"$SAGA_HOST_FIXTURE\""); unsetenv("SAGA_HOST_FIXTURE");
  CHECK(result["stdout"] == "inherited-environment"); CHECK(approvals == 2);
  // These are synthetic credentials: dangerous mode really removes private-storage guards.
  atomic_write(f.paths.runtime/"control.token","synthetic-private-token");
  result=shell("cat '"+(f.paths.runtime/"control.token").string()+"'"); CHECK(result["stdout"] == "synthetic-private-token");
  CHECK(p->db->query("SELECT value FROM runtime_settings WHERE key='host_permissions'")[0]["value"] == "host_always");
  if (shell_sandbox_available()) {
    CHECK(shell("cat '"+file.string()+"'","sandbox")["exit_code"] != 0); CHECK(approvals == 2);
    fs::remove(file); policy.permissions("always_approve");
    CHECK(shell(create)["exit_code"] != 0); CHECK(!fs::exists(file)); CHECK(approvals == 2);
  }
  auto other=f.persona(registry.create()); Memory other_memory(*other,f.config());
  Tools other_tools(*other,other_memory,[](auto&,auto&){ return false; });
  CHECK(other_tools.permissions()["mode"] == "ask_always");
}
void reviewed_edits() {
  Fixture f;Registry registry(f.paths);auto p=f.persona(registry.create());auto db=p->db.get();
  p->project=db->exec("INSERT INTO projects(name,root_path,created_at,last_seen_at) VALUES('initial',?,?,?)",{f.project.string(),now(),now()});
  p->session=db->exec("INSERT INTO sessions(started_at,project_id,status) VALUES(?,?,'active')",{now(),p->project});
  Memory memory(*p,f.config());int approvals=0;bool accept=false,stale=false;Json proposal,events=Json::array();
  Tools tools(*p,memory,[&](auto&,const Json& args){++approvals;proposal=args;if(stale)atomic_write(f.project/"review.txt","concurrent change\n");return accept;});
  auto emit=[&](const std::string& type,const Json& data){events.push_back({{"type",type},{"data",data}});};
  auto write=[&]{return tools.execute("file_write",{{"path","review.txt"},{"content","first\nsecond\n"},{"description","Synthetic reviewed edit"}},emit);};
  CHECK(write().contains("error"));CHECK(!fs::exists(f.project/"review.txt"));CHECK(db->query("SELECT * FROM artifacts").empty());
  CHECK(proposal["kind"]=="edit" && proposal["diff"].get<std::string>().find("+first")!=std::string::npos);
  accept=true;CHECK(!write().contains("error"));CHECK(approvals==2);CHECK(events.back()["type"]=="tool.completed");CHECK(db->query("SELECT * FROM artifact_versions").size()==1);
  Json edit={{"path","review.txt"},{"expected_hash",digest("first\nsecond\n")},{"description","Targeted edit"},{"edits",Json::array({{{"old_text","second"},{"new_text","next"}}})}};
  stale=true;CHECK(tools.execute("file_edit",edit).contains("error"));CHECK(read_file(f.project/"review.txt")=="concurrent change\n");CHECK(db->query("SELECT * FROM artifact_versions").size()==1);stale=false;
  CHECK(tools.execute("file_edit",edit).contains("error"));fs::permissions(f.project/"review.txt",fs::perms::owner_all);edit["expected_hash"]=digest("concurrent change\n");edit["edits"]=Json::array({{{"old_text","concurrent"},{"new_text","good"}}});
  CHECK(!tools.execute("file_edit",edit).contains("error"));CHECK(read_file(f.project/"review.txt")=="good change\n");CHECK((fs::status(f.project/"review.txt").permissions() & fs::perms::owner_exec)!=fs::perms::none);
  edit["expected_hash"]=digest("good change\n");edit["edits"]=Json::array({{{"old_text","good"},{"new_text","a"}},{{"old_text","good change"},{"new_text","b"}}});CHECK(tools.execute("file_edit",edit).contains("error"));
  for(auto mode:{"ask_always","always_approve","host_ask","host_always"}) {
    tools.permissions(mode);int previous=approvals;auto result=tools.execute("file_write",{{"path",std::string(mode)+".txt"},{"content","test"},{"description","Permission fixture"}});
    CHECK(!result.contains("error"));CHECK(approvals-previous==((std::string(mode)=="ask_always" || std::string(mode)=="host_ask")?1:0));
  }
  auto task=tools.execute("task_create",{{"title","Synthetic workspace task"},{"objective","Test scoped work"},{"risk","low"},{"checks",Json::array({"Verify workspace output"})}})["id"];
  auto target=f.root/"next";CHECK(tools.execute("project_open",{{"path",target.string()},{"create",true}}).contains("error"));
  db->event("user.message",{{"content","Work in "+target.string()}},p->session);
  CHECK(!tools.execute("project_open",{{"path",target.string()},{"create",true}}).contains("error"));CHECK(p->task==task && p->project_root==target);
  CHECK(!tools.execute("file_write",{{"path","new.txt"},{"content","new"},{"description","Scoped artifact"}}).contains("error"));
  auto third=f.root/"third";db->event("user.message",{{"content","Continue in "+third.string()}},p->session);
  CHECK(!tools.execute("project_open",{{"path",third.string()},{"create",true}}).contains("error"));CHECK(p->task!=task);CHECK(db->query("SELECT status FROM tasks WHERE id=?",{task})[0]["status"]=="blocked");
  CHECK(db->query("SELECT status,evidence_id FROM task_checks WHERE task_id=?",{p->task})[0]["status"]=="unresolved");
  CHECK(tools.execute("project_open",{{"path",f.paths.data.string()},{"create",true}}).contains("error"));
  CHECK(unified_diff("same\n","same\n","fixture").text.empty());
  auto diff=unified_diff("a\nb\nc","a\nB\nc\n","fixture");CHECK(diff.added==2 && diff.removed==2);CHECK(diff.text.find("\\ No newline at end of file")!=std::string::npos);
  // An independent patch consumer verifies the generated hunks, including EOF and UTF-8.
  for(auto& [before,after]:std::vector<std::pair<std::string,std::string>>{{"","new\n"},{"old\n",""},{"a\nb\nc","a\nB\nc\n"},{"a\nb\na\nb\n","b\na\nb\na\n"},{"東京\nfirst\n","東京\nlast"},{"one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n","ONE\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nTEN\n"}}) {
    atomic_write(f.project/"fixture",before);atomic_write(f.project/"patch",unified_diff(before,after,"fixture").text);
    auto applied=run_process(f.project,f.root/"patch-scratch",{"/usr/bin/git","apply","--unsafe-paths","patch"},5,true,{},Json::object(),true);
    CHECK(applied.exit_code==0);CHECK(read_file(f.project/"fixture")==after);
  }
}
void gates_and_praxis() {
  Fixture f; Registry registry(f.paths); auto p = f.persona(registry.create());
  auto db = p->db.get(); p->project = db->exec("INSERT INTO projects(name,root_path,created_at,last_seen_at) VALUES('test',?,?,?)",{f.project.string(),now(),now()});
  p->session = db->exec("INSERT INTO sessions(started_at,project_id,status) VALUES(?,?,'active')",{now(),p->project});
  Memory m(*p,f.config()); Tools tools(*p,m,[](auto&,auto&){ return true; });
  auto t = tools.execute("task_create",{{"title","write artifact"},{"objective","test"},{"risk","low"},{"checks",{"file exists"}}}); CHECK(t.contains("id"));
  CHECK(tools.execute("task_update",{{"id",p->task},{"status","completed"}}).contains("error"));
  auto write = tools.execute("file_write",{{"path","output.txt"},{"content","artifact"},{"description","test file"}}); CHECK(!write.contains("error")); CHECK(read_file(f.project / "output.txt") == "artifact");
  CHECK(db->query("SELECT * FROM artifacts").size() == 1); CHECK(db->query("SELECT * FROM artifact_versions").size() == 1);
  CHECK(tools.execute("file_read",{{"path",(f.paths.data / "registry.db").string()}}).contains("error"));
  fs::create_symlink(f.paths.data / "registry.db",f.project / "alias");
  CHECK(tools.execute("file_read",{{"path","alias"}}).contains("error")); fs::remove(f.project / "alias");
  fs::create_hard_link(f.paths.data / "registry.db",f.project / "hardalias");
  CHECK(tools.execute("file_read",{{"path","hardalias"}}).contains("error")); fs::remove(f.project / "hardalias");
  CHECK(tools.execute("file_write",{{"path","SOUL.md"},{"content","mutated"},{"description","bad"}}).contains("error"));
  CHECK(tools.execute("personas.list",{}).contains("error"));
  CHECK(tools.execute("task_create",{{"title",4},{"objective","test"},{"risk","low"},{"checks",{"check"}}}).contains("error"));
  CHECK(!tools.execute("check_resolve",{{"check_id",t["checks"][0]["id"]},{"source_event_id",write["source_event_id"]},{"passed",true},{"explanation","Write observed"}}).contains("error"));
  CHECK(!tools.execute("task_update",{{"id",p->task},{"status","completed"}}).contains("error"));
  auto belief = tools.execute("record_belief",{{"statement","The artifact contains the expected output"},{"domain","files"},{"source_event_id",write["source_event_id"]}});
  CHECK(belief.contains("id"));
  auto read = tools.execute("file_read",{{"path","output.txt"}});
  auto archived = tools.execute("recall_observation",{{"event_id",read["source_event_id"]}});
  if (archived.contains("error")) throw std::runtime_error(archived.dump());
  CHECK(Json::parse(archived["content"].get<std::string>())["result"]["content"] == "artifact");
  CHECK(tools.execute("recall_observation",{{"event_id",read["source_event_id"]},{"offset",-1}}).contains("error"));
  auto proof_args = Json{{"target_type","belief"},{"target_id",belief["id"]},{"source_event_id",read["source_event_id"]},{"support",false},{"description","A contrary interpretation of the observed file"}};
  auto counter = tools.execute("record_evidence",proof_args); CHECK(!counter.contains("error"));
  CHECK(counter["evidence_confidence"].get<double>() < belief["evidence_confidence"].get<double>());
  auto duplicate = tools.execute("record_evidence",proof_args); CHECK(duplicate["already_recorded"] == true); CHECK(duplicate["id"] == counter["id"]);
  auto praxis = m.candidate({{"name","write carefully"},{"trigger","write"},{"procedure","write then read"},{"rationale","verify"}},write["source_event_id"]);
  auto outcome_event = db->event("task.completed",{},p->session,p->task);
  m.praxis_outcome(praxis,p->task,true,outcome_event); m.praxis_outcome(praxis,p->task,true,outcome_event);
  CHECK(db->query("SELECT uses FROM praxis WHERE id=?",{praxis})[0]["uses"] == 1);
  for (int i = 1; i < 10; ++i) {
    auto task = db->exec("INSERT INTO tasks(session_id,project_id,title,objective,status,created_at) VALUES(?,?,'task','proof','completed',?)",{p->session,p->project,now()});
    auto source_event = db->event("task.completed",{},p->session,task);
    auto proof = m.evidence("task",task,"output observed",true,source_event);
    db->exec("INSERT INTO task_checks(task_id,description,status,evidence_id) VALUES(?,'Observed result','passed',?)",{task,proof});
    m.praxis_outcome(praxis,task,true,source_event);
    auto state = db->query("SELECT status FROM praxis WHERE id=?",{praxis})[0]["status"];
    if (i == 1) CHECK(state == "learned");
    if (i == 4) CHECK(state == "validated");
    if (i == 9) CHECK(state == "habitual");
  }
  CHECK(p->soul == default_soul);
  Tools declined(*p,m,[](auto&,auto&){ return false; });
  auto skill=declined.execute("compile_skill",{{"praxis_id",praxis},{"name","safe read"},{"steps",Json::array({{{"tool","file_read"},{"arguments",{{"path","output.txt"}}}}})}});CHECK(skill.contains("id"));
  CHECK(declined.execute("run_skill",{{"id",skill["id"]}})["results"][0]["content"]=="artifact");
  CHECK(declined.execute("compile_skill",{{"praxis_id",praxis},{"name","host read"},{"steps",Json::array({{{"tool","shell_exec"},{"arguments",{{"command","printf host"},{"execution","host"}}}}})}}).contains("error"));
  CHECK(declined.execute("file_write",{{"path","automatic.txt"},{"content","automatic"},{"description","reviewed write"}}).contains("error")); CHECK(!fs::exists(f.project / "automatic.txt"));
  CHECK(declined.execute("shell_exec",{{"command","touch denied-host.txt"},{"execution","host"}}).contains("error")); CHECK(!fs::exists(f.project/"denied-host.txt"));
  atomic_write(f.project / "binary.dat",std::string("\0\xff",2));
  auto binary = tools.execute("file_read",{{"path","binary.dat"}});
  CHECK(binary["encoding"] == "base64"); CHECK(binary["content"] == "AP8=");
  for (int i = 0; i < 3; ++i) {
    auto predicted = db->event("prediction.created",{},p->session,p->task);
    auto prediction = db->exec("INSERT INTO predictions(task_id,statement,expected_outcome,confidence,domain,source_event_id,created_at) VALUES(?,'expect success','success',0.9,'frontend',?,?)",{p->task,predicted,now()});
    auto observed = db->event("tool.failed",{{"tool","shell_exec"},{"result",{{"exit_code",1}}}},p->session,p->task);
    db->exec("INSERT INTO observations(task_id,prediction_id,statement,matched,source_event_id,created_at) VALUES(?,?,'observed failure',0,?,?)",{p->task,prediction,observed,now()});
  }
  m.maintain(); CHECK(ExecutiveController::needs_review({{"risk","low"},{"domain","frontend"}},m.self()));
  auto calibrated = db->query("SELECT id FROM events WHERE type='self.calibrated'").size();
  m.maintain(); CHECK(db->query("SELECT id FROM events WHERE type='self.calibrated'").size() == calibrated);
  ContextBuilder builder(*p,m,f.config());
  for (int i = 0; i < 80; ++i) {
    Json message = {{"role","user"},{"content",std::string(2500,'a')}};
    db->exec("INSERT INTO messages(session_id,ts,role,content_json) VALUES(?,?,'user',?)",{p->session,now(),message.dump()});
    message = {{"role","assistant"},{"content",std::string(2500,'b')}};
    db->exec("INSERT INTO messages(session_id,ts,role,content_json) VALUES(?,?,'assistant',?)",{p->session,now(),message.dump()});
  }
  auto request = builder.build(); CHECK(estimate_tokens(request.messages.dump()) + estimate_tokens(request.tools.dump()) <= f.config().input_budget());
  CHECK(request.messages[0]["role"] == "system");
  auto system = request.messages[0]["content"].get<std::string>();
  CHECK(system.find(p->soul) != std::string::npos);
  auto marker = system.find("Persistent working state (data):\n"); CHECK(marker != std::string::npos);
  CHECK(Json::parse(system.substr(marker+std::string("Persistent working state (data):\n").size())).contains("task_checks"));
  for (size_t i=1; i<request.messages.size(); ++i) CHECK(request.messages[i]["role"] != "system");
  CHECK(request.messages.size() < 160);
  Json long_turn = {{"role","user"},{"content","Continue the long diagnostic task"}};
  db->exec("INSERT INTO messages(session_id,ts,role,content_json) VALUES(?,?,'user',?)",{p->session,now(),long_turn.dump()});
  for (int i = 0; i < 45; ++i) {
    auto call = "long-call-" + std::to_string(i);
    Json assistant = {{"role","assistant"},{"content",nullptr},{"tool_calls",Json::array({{{"id",call},{"type","function"},{"function",{{"name","file_read"},{"arguments","{\"path\":\"output.txt\"}"}}}}})}};
    Json observation = {{"content",std::string(3000,'x')},{"source_event_id",write["source_event_id"]}};
    Json message = {{"role","tool"},{"tool_call_id",call},{"content",observation.dump()}};
    db->exec("INSERT INTO messages(session_id,ts,role,content_json) VALUES(?,?,'assistant',?)",{p->session,now(),assistant.dump()});
    db->exec("INSERT INTO messages(session_id,ts,role,content_json) VALUES(?,?,'tool',?)",{p->session,now(),message.dump()});
  }
  request = builder.build(); CHECK(estimate_tokens(request.messages.dump()) + estimate_tokens(request.tools.dump()) <= f.config().input_budget());
  auto last_tool=std::find_if(request.messages.rbegin(),request.messages.rend(),[](auto& message){return message["role"]=="tool";});
  CHECK(last_tool!=request.messages.rend() && (*last_tool)["tool_call_id"]=="long-call-44");
  CHECK(!db->query("SELECT id FROM events WHERE type='context.working_compacted'").empty());
  CHECK(db->query("SELECT count(*) AS n FROM messages WHERE session_id=? AND role!='system_internal'",{p->session})[0]["n"] == 251);
  auto handoff=m.handoff(); CHECK(handoff.contains("checkpoint_id")); CHECK(handoff["last_user_request"] == "Continue the long diagnostic task");
  auto checkpoints=db->query("SELECT count(*) AS n FROM context_checkpoints")[0]["n"];
  builder.build(); CHECK(db->query("SELECT count(*) AS n FROM context_checkpoints")[0]["n"] == checkpoints);
  if (shell_sandbox_available()) {
    int approvals=0; Tools policy(*p,m,[&](auto&,auto&){ ++approvals; return true; });
    auto r=policy.execute("shell_exec",{{"command","printf ok; file=$(mktemp); printf temp > \"$file\"; cat \"$file\""}});
    CHECK(r["exit_code"] == 0); CHECK(approvals == 0); CHECK(r["stdout"] == "oktemp");
    p->host_environment={{"DBUS_SESSION_BUS_ADDRESS","unix:path=/tmp/test-session-bus"}};
    r=policy.execute("shell_exec",{{"command","printf '%s' \"$DBUS_SESSION_BUS_ADDRESS\""},{"execution","host"}});
    if (r.value("exit_code",-1) != 0) throw std::runtime_error("Host runner failed: "+r.dump());
    CHECK(approvals == 1); CHECK(r["stdout"] == "unix:path=/tmp/test-session-bus");
    policy.permissions("always_approve");
    r=policy.execute("shell_exec",{{"command","printf host-ok"},{"execution","host"}}); CHECK(r["exit_code"] == 0); CHECK(approvals == 1);
    auto hidden=policy.execute("shell_exec",{{"command","cat '"+(f.paths.data/"registry.db").string()+"'"},{"execution","host"}}); CHECK(hidden["exit_code"] != 0);
    auto original_soul=read_file(p->directory/"SOUL.md");
    CHECK(policy.execute("shell_exec",{{"command","printf changed > '"+(p->directory/"SOUL.md").string()+"'"},{"execution","host"}})["exit_code"]!=0);
    CHECK(read_file(p->directory/"SOUL.md")==original_soul);
    atomic_write(f.paths.runtime/"control.token","test-private-control-token");
    CHECK(policy.execute("shell_exec",{{"command","cat '"+(f.paths.runtime/"control.token").string()+"'"},{"execution","host"}})["exit_code"]!=0);
    CHECK(policy.execute("shell_exec",{{"command","ls '"+(f.paths.data/"personas").string()+"'"},{"execution","host"}})["exit_code"]!=0);
    auto hostfile=f.root/"outside.txt"; atomic_write(hostfile,"outside");
    r=policy.execute("shell_exec",{{"command","cat '"+hostfile.string()+"'"},{"execution","host"}}); CHECK(r["stdout"] == "outside");
    CHECK(tools.execute("shell_exec",{{"command","cat '"+hostfile.string()+"'"}})["exit_code"] != 0);
    policy.permissions("ask_always");
    auto process = run_process(f.project,p->directory / "cache/scratch",{"/bin/sh","-c","printf hello; printf error >&2"}); CHECK(process.exit_code == 0); CHECK(process.out == "hello"); CHECK(process.err == "error");
    auto denied = run_process(f.project,p->directory / "cache/scratch",{"/bin/sh","-c","cat '" + (f.paths.data / "registry.db").string() + "'"}); CHECK(denied.exit_code != 0);
    auto env = run_process(f.project,p->directory / "cache/scratch",{"/usr/bin/env"}); CHECK(env.out.find("SAGA_API_KEY") == std::string::npos);
    auto shell = tools.execute("shell_exec",{{"command","printf shell > shell-created.txt"}});
    CHECK(shell["exit_code"] == 0); CHECK(shell["artifacts_recorded"] == 1);
    CHECK(!m.search("recall_artifact","shell-created")["results"].empty());
    auto byte = run_process(f.project,p->directory / "cache/scratch",{"/bin/sh","-c","printf '\\377'"});
    CHECK(byte.json()["stdout_size"] == 1); CHECK(byte.json()["stdout"] == "�");
    auto timeout = run_process(f.project,p->directory / "cache/scratch",{"/bin/sh","-c","sleep 5"},1); CHECK(timeout.timed_out);
  }
}
void config_and_ipc() {
  Fixture f; auto c = f.config(); c.api_key = "secret-with-quotes\""; c.save(f.paths);
  auto loaded = Config::load(f.paths); CHECK(loaded.api_key == c.api_key); CHECK(loaded.input_budget() == 55296);
  c.context_length = 32768; rejects([&]{ c.validate(); }); c.allow_small_context = true; c.validate();
  c.context_length = 1024; rejects([&]{ c.input_budget(); });
  int sockets[2]; CHECK(socketpair(AF_UNIX,SOCK_STREAM | SOCK_CLOEXEC,0,sockets) == 0);
  Channel a(sockets[0]),b(sockets[1]); a.send({{"type","test"},{"payload",{{"name","space — unicode"}}}}); CHECK(b.receive(100)->at("type") == "test"); CHECK(!b.receive(1));
  CHECK(ExecutiveController::route("Do you remember the page?") == CognitiveMode::Recall);
  CHECK(ExecutiveController::route("Fix the widget") == CognitiveMode::Plan);
  CHECK(ExecutiveController::route("hello") == CognitiveMode::Respond);
}
}
int main() {
  std::vector<std::pair<std::string,std::function<void()>>> tests = {{"nested research context",nested_research_context},{"streaming",streaming},{"capability probe and progress",capability_probe},{"persistence and isolation",persistence},{"guarded and unrestricted host permissions",host_permissions},{"reviewed edits and workspaces",reviewed_edits},{"tools, proof gates, praxis and context",gates_and_praxis},{"configuration and IPC",config_and_ipc}};
  int failed = 0;
  for (auto& [name,test] : tests) { try { test(); std::cout << "PASS " << name << '\n'; } catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; } }
  return failed ? 1 : 0;
}
