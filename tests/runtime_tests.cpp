#include "check.hpp"
#include <saga/runtime.hpp>
#include <algorithm>
#include <thread>
using namespace saga;
namespace {
class TestBackend : public ModelBackend {
  std::shared_ptr<Json> log_;
public:
  bool fail = false;
  bool approve_review = true;
  explicit TestBackend(std::shared_ptr<Json> log) : log_(std::move(log)) {}
  ModelInfo discover() override { return {"test-engine",65536,true,true,false}; }
  CapabilityReport probe() override { return {true,true,true,true}; }
  void chat(const ChatRequest& request,StreamCallback cb) override {
    CHECK(!request.messages.empty() && request.messages[0]["role"] == "system");
    for (size_t i=1; i<request.messages.size(); ++i) CHECK(request.messages[i]["role"] != "system");
    log_->push_back(request.messages);
    if (fail) throw std::runtime_error("Simulated offline backend");
    auto function = [&](std::string name,Json args){
      auto text = args.dump(); auto middle = text.size()/2;
      Json first = {{"index",0},{"id","call_"+uuid()},{"type","function"},{"function",{{"name",name},{"arguments",text.substr(0,middle)}}}};
      cb({{"choices",Json::array({{{"index",0},{"delta",{{"tool_calls",Json::array({first})}}},{"finish_reason",nullptr}}})}});
      cb({{"choices",Json::array({{{"index",0},{"delta",{{"tool_calls",Json::array({{{"index",0},{"function",{{"arguments",text.substr(middle)}}}}})}}},{"finish_reason","tool_calls"}}})},{"usage",{{"prompt_tokens",180},{"completion_tokens",30}}}});
    };
    if (request.forced_tool == "submit_session_summary") {
      bool artifact = request.messages.dump().find("artifact.txt") != std::string::npos;
      function(*request.forced_tool,{{"title",artifact ? "Artifact continuity" : "Conversation"},
        {"summary",artifact ? "Worked on artifact.txt with observed verification" : "Spoke with the user"},
        {"narrative",artifact ? "I worked on the artifact and checked its contents." : "I spoke with the user and preserved our conversation."},
        {"outcome","recorded"},{"lesson",artifact ? "Verify before claiming completion" : ""},
        {"entities",artifact ? Json::array({{{"name","artifact.txt"},{"type","file"},{"aliases",{"the artifact"}}}}) : Json::array()}}); return;
    }
    if (request.forced_tool == "submit_review") { function(*request.forced_tool,{{"approved",approve_review},{"concerns",approve_review ? "" : "Important assumption needs another observation"}}); return; }
    if (request.forced_tool == "submit_memory_extraction") { function(*request.forced_tool,{{"facts",Json::array()},{"praxis",Json::array()}}); return; }
    size_t user_index=0; std::string input;
    Json state=Json::object();
    for (size_t i=0; i<request.messages.size(); ++i) {
      auto& m=request.messages[i];
      if(m["role"]=="user") {
        auto text=m.value("content","");std::string marker="Runtime attention update (data):\n";
        if(text.starts_with(marker))state.update(Json::parse(text.substr(marker.size())));
        else {input=text;user_index=i;}
      }
      if (m["role"] == "system") { auto s=m.value("content",""); std::string prefix="Persistent working state (data):\n"; auto at=s.find(prefix); if (at != std::string::npos) state=Json::parse(s.substr(at+prefix.size())); }
    }
    if(input=="Empty always" || (input=="Empty once" && !state.value("attention",Json::object()).contains("empty_response_recovery"))) {
      cb({{"choices",Json::array({{{"index",0},{"delta",{{"reasoning_content","Internal reasoning only"}}},{"finish_reason","stop"}}})}});return;
    }
    if(input=="Public progress" && request.messages.back()["role"]!="tool" && request.messages.dump().find("published")==std::string::npos) {
      function("report_progress",{{"text","I am checking **the project** before changing code."}});return;
    }
    if(input=="Exercise controls" || input=="Exercise stop") {
      auto args=Json{{"command",input=="Exercise stop"?"printf started; sleep 10; printf forbidden":"sleep 1; printf finished"},{"execution","host"}};
      Json calls=Json::array({{{"index",0},{"id","control-first"},{"type","function"},{"function",{{"name","shell_exec"},{"arguments",args.dump()}}}},
        {{"index",1},{"id","control-second"},{"type","function"},{"function",{{"name","file_write"},{"arguments",Json{{"path","must-not-exist"},{"content","bad"},{"description","superseded action"}}.dump()}}}}});
      cb({{"choices",Json::array({{{"index",0},{"delta",{{"tool_calls",calls}}},{"finish_reason","tool_calls"}}})}});return;
    }
    if(input=="Show live progress") {
      for(int i=0;i<5;++i){std::this_thread::sleep_for(std::chrono::milliseconds(45));cb({{"choices",Json::array({{{"index",0},{"delta",{{"reasoning_content",std::string(60,'r')}}},{"finish_reason",nullptr}}})}});}
    }
    if(input=="Show exact context") {
      cb({{"choices",Json::array({{{"index",0},{"delta",{{"role","assistant"},{"content",nullptr}}},{"finish_reason",nullptr}}})},{"prompt_progress",{{"total",10000},{"cache",8500},{"processed",10000}}},{"timings",{{"prompt_n",1500},{"cache_n",8500},{"predicted_n",0}}}});
      for(int i=1;i<=5;++i){std::this_thread::sleep_for(std::chrono::milliseconds(60));cb({{"choices",Json::array({{{"index",0},{"delta",{{"content","x"}}},{"finish_reason",nullptr}}})},{"timings",{{"prompt_n",1500},{"cache_n",8500},{"predicted_n",i}}}});}
      cb({{"choices",Json::array({{{"index",0},{"delta",Json::object()},{"finish_reason","stop"}}})},{"usage",{{"prompt_tokens",10000},{"completion_tokens",5}}}});return;
    }
    if (input.starts_with("Create artifact")) {
      std::string last; Json result;
      for (size_t i=user_index+1; i<request.messages.size(); ++i) {
        auto& m=request.messages[i]; if (m.contains("tool_calls")) last=m["tool_calls"][0]["function"]["name"];
        if (m["role"] == "tool") result=Json::parse(m.value("content","{}"));
      }
      if (last.empty()) { function("file_write",{{"path","artifact.txt"},{"content","persistent identity artifact\n"},{"description","Continuity artifact"}}); return; }
      if (last == "file_write") { function("file_read",{{"path","artifact.txt"}}); return; }
      if (last == "file_read") { function("check_resolve",{{"check_id",state["task_checks"][0]["id"]},{"source_event_id",result["source_event_id"]},{"passed",true},{"explanation","Observed artifact contents"}}); return; }
      if (last == "check_resolve") { function("task_update",{{"id",state["active_task"][0]["id"]},{"status","completed"}}); return; }
    }
    std::string text = input.find("remember") != std::string::npos ? "I recalled the artifact from persistent memory." : "Hello from the interchangeable reasoning engine.";
    cb({{"choices",Json::array({{{"index",0},{"delta",{{"content",text.substr(0,10)}}},{"finish_reason",nullptr}}})}});
    cb({{"choices",Json::array({{{"index",0},{"delta",{{"content",text.substr(10)}}},{"finish_reason","stop"}}})},{"usage",{{"prompt_tokens",180},{"completion_tokens",30}}}});
  }
};
}
int main() {
  fs::path root=fs::temp_directory_path()/("saga-lifecycle-"+uuid());
  try {
    Paths paths{root/"config",root/"data",root/"state",root/"run"}; paths.create(); auto project=root/"project"; private_dir(project);
    Registry registry(paths); auto a=registry.create("Identity A","Private soul A"),b=registry.create("Identity B","Private soul B");
    Config config; config.endpoint="http://localhost:9999"; config.model="test-engine"; config.context_length=65536;
    auto log=std::make_shared<Json>(Json::array()); Json events=Json::array(); int approvals=0;
    auto emit=[&](const std::string& type,const Json& payload){ events.push_back({{"type",type},{"payload",payload}}); };
    auto activate=[&](const Json& metadata){ return std::make_unique<Runtime>(std::make_unique<PersonaContext>(paths,metadata,project),config,std::make_unique<TestBackend>(log),[&](auto&,auto&){ ++approvals; return true; }); };
    auto runtime=activate(a); runtime->start(emit); CHECK(log->empty()); runtime->chat("Create artifact for continuity",emit);
    for(size_t i=1;i<log->size();++i) {
      auto& previous=(*log)[i-1];auto& next=(*log)[i];CHECK(next.size()>previous.size());
      for(size_t j=0;j<previous.size();++j)CHECK(next[j]==previous[j]);
    }
    auto applied=runtime->persona().db->query("SELECT id FROM events WHERE type='edit.applied' ORDER BY id DESC LIMIT 1")[0]["id"];
    CHECK(runtime->command("diff",{{"event_id",applied}})["diff"].get<std::string>().find("+persistent identity artifact")!=std::string::npos);
    rejects([&]{runtime->command("diff",{{"event_id",-1}});});
    CHECK(approvals == 1); CHECK(read_file(project/"artifact.txt") == "persistent identity artifact\n"); CHECK(runtime->command("tasks")[0]["status"] == "completed");
    CHECK(std::any_of(events.begin(),events.end(),[](const Json& e){ return e["type"] == "context.usage" && e["payload"].value("approximate",false); }));
    CHECK(std::any_of(events.begin(),events.end(),[](const Json& e){ return e["type"] == "context.usage" && !e["payload"].value("approximate",true) && e["payload"].value("used_tokens",0) == 210; }));
    runtime->command("fact",{{"subject","machine"},{"predicate","OS"},{"object","FreeBSD"}});
    runtime->close("persona_switch",emit); runtime.reset();
    { Database db(paths.persona(a["uuid"].get<std::string>())/"agent.db"); CHECK(db.query("SELECT * FROM journal_entries").size()==1); CHECK(db.query("SELECT * FROM episodes").size()==1); CHECK(db.query("SELECT * FROM tool_runs").size()>=5); CHECK(db.query("SELECT * FROM model_calls WHERE approximate=0").size()>=5); }
    runtime=activate(b); runtime->start(emit); runtime->chat("hello",emit); CHECK(runtime->command("journal").empty()); CHECK(runtime->command("memory",{{"kind","know"},{"query","machine"}})["results"].empty());
    auto request=log->back().dump(); CHECK(request.find("Private soul A")==std::string::npos); CHECK(request.find("Identity A")==std::string::npos);
    runtime->close("persona_switch",emit); runtime.reset();
    config.model="replacement-engine"; runtime=activate(a); runtime->start(emit); runtime->chat("Do you remember the artifact?",emit);
    CHECK(!runtime->command("memory",{{"query","artifact"},{"deep",true}})["results"].empty()); CHECK(runtime->command("memory",{{"kind","know"},{"query","machine"}})["results"][0]["object"]=="FreeBSD");
    CHECK(runtime->command("soul")["content"]=="Private soul A"); runtime->command("new"); runtime->close("user_exit",emit); runtime.reset();
    // Abrupt context loss creates a local diary without inference during wake.
    Id crash_session;
    { Database db(paths.persona(a["uuid"].get<std::string>())/"agent.db"); crash_session=db.exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()}); db.event("user.message",{{"content","Crash continuity"}},crash_session); db.exec("INSERT INTO messages(session_id,ts,role,content_json) VALUES(?,?,'user',?)",{crash_session,now(),Json{{"role","user"},{"content","Crash continuity"}}.dump()}); }
    auto before_wake=log->size();
    runtime=activate(a); runtime->start(emit); CHECK(log->size()==before_wake); runtime->tick({},false); CHECK(log->size()==before_wake);
    CHECK(runtime->persona().db->query("SELECT end_reason,status FROM sessions WHERE id=?",{crash_session})[0]["end_reason"]=="crash_recovery"); CHECK(runtime->persona().db->query("SELECT * FROM journal_entries WHERE session_id=?",{crash_session}).size()==1); runtime->close("user_exit",emit); CHECK(log->size()==before_wake); runtime.reset();
    // An offline close writes a provisional diary, and a later wake upgrades
    // derived memory without duplicating the session's episode or journal.
    auto failing = std::make_unique<TestBackend>(log); auto* failing_ptr = failing.get();
    runtime=std::make_unique<Runtime>(std::make_unique<PersonaContext>(paths,a,project),config,std::move(failing),[](auto&,auto&){ return true; });
    runtime->start(); runtime->chat("hello before going offline",emit); Id offline_session=runtime->persona().session;
    failing_ptr->fail=true; runtime->close("user_exit");
    CHECK(runtime->persona().db->query("SELECT * FROM journal_entries WHERE session_id=?",{offline_session}).size()==1);
    CHECK(runtime->persona().db->query("SELECT status FROM sessions WHERE id=?",{offline_session})[0]["status"]=="needs_consolidation"); runtime.reset();
    before_wake=log->size(); runtime=activate(a); runtime->start(); CHECK(log->size()==before_wake);
    CHECK(runtime->persona().db->query("SELECT status FROM sessions WHERE id=?",{offline_session})[0]["status"]=="needs_consolidation");
    for (int i=0; i<4; ++i) runtime->tick();
    CHECK(runtime->persona().db->query("SELECT status FROM sessions WHERE id=?",{offline_session})[0]["status"]=="completed");
    CHECK(runtime->persona().db->query("SELECT * FROM episodes WHERE session_id=?",{offline_session}).size()==1); runtime->close("user_exit"); runtime.reset();
    auto reviewing=std::make_unique<TestBackend>(log); reviewing->approve_review=false;
    runtime=std::make_unique<Runtime>(std::make_unique<PersonaContext>(paths,a,project),config,std::move(reviewing),[](auto&,auto&){ return true; });
    runtime->start(); runtime->chat("Create artifact migration",emit);
    CHECK(runtime->command("tasks")[0]["status"]=="verifying");
    CHECK(runtime->persona().db->query("SELECT * FROM events WHERE type='assistant.completion_gated'").size()==1);
    CHECK(events[events.size()-2]["type"]=="assistant.completed" && events.back()["type"]=="turn.finished"); CHECK(events[events.size()-2]["payload"]["content"].get<std::string>().starts_with("The task is not complete"));
    bool fresh_review=false; for (auto& messages:*log) if (messages.size()==2 && messages[1].value("content","").starts_with("Independently review")) fresh_review=true; CHECK(fresh_review);
    runtime->close("user_exit"); runtime.reset();
    runtime=activate(b); runtime->start();
    events=Json::array();runtime->chat("Show live progress",emit);
    CHECK(std::any_of(events.begin(),events.end(),[](auto& e){return e["type"]=="context.usage" && e["payload"].value("approximate",false) && e["payload"].value("output_tokens",0)>0;}));
    CHECK(!runtime->persona().db->query("SELECT id FROM events WHERE type='model.reasoning_observed'").empty());
    auto same_session=runtime->persona().session;auto previous_turn=log->back();
    events=Json::array();runtime->chat("Show exact context",emit);int exact_updates=0,last_exact=0;
    CHECK(runtime->persona().session==same_session);CHECK(runtime->persona().db->query("SELECT id FROM sessions WHERE status='active'").size()==1);
    for(size_t i=0;i<previous_turn.size();++i)CHECK(log->back()[i]==previous_turn[i]);
    CHECK(std::any_of(log->back().begin(),log->back().end(),[](auto& message){return message.value("reasoning_content",std::string()).size()==300;}));
    for(auto& event:events)if(event["type"]=="context.usage" && !event["payload"].value("approximate",true)){auto used=event["payload"]["used_tokens"].get<int>();CHECK(used>=last_exact);last_exact=used;++exact_updates;}
    CHECK(exact_updates>=3 && last_exact==10005);CHECK(runtime->command("status")["usage"]["used_tokens"]==10005);
    CHECK(runtime->command("status")["usage"]["cached_input_tokens"]==8500);
    CHECK(!runtime->persona().db->query("SELECT id FROM events WHERE session_id=? AND type='model.cache_observed'",{same_session}).empty());
    events=Json::array();runtime->chat("Empty once",emit);CHECK(events[events.size()-2]["type"]=="assistant.completed" && events.back()["type"]=="turn.finished");CHECK(!events[events.size()-2]["payload"]["content"].get<std::string>().empty());
    auto calls_before=log->size();rejects([&]{runtime->chat("Empty always",emit);});CHECK(log->size()==calls_before+2);
    events=Json::array();runtime->chat("Public progress",emit);
    CHECK(std::any_of(events.begin(),events.end(),[](auto& e){return e["type"]=="progress.updated" && !e["payload"].value("complete",true);}));
    if(shell_sandbox_available()) {
      bool running=false,queued=false;
      auto controlled_emit=[&](const std::string& type,const Json& p){emit(type,p);if(type=="tool.started" && p["tool"]=="shell_exec")running=true;};
      runtime->service([&]{if(running && !queued){queued=true;runtime->command("steer",{{"content","Continue with the new instructions"}});}});
      runtime->chat("Exercise controls",controlled_emit);CHECK(queued && !fs::exists(project/"must-not-exist"));
      CHECK(runtime->persona().db->query("SELECT status FROM steering_messages ORDER BY id DESC LIMIT 1")[0]["status"]=="delivered");
      bool output_seen=false,stopped=false;
      runtime->service([&]{if(output_seen && !stopped){stopped=true;runtime->command("steer",{{"content","This queued instruction must be cancelled"}});runtime->command("stop");}});
      auto stop_emit=[&](const std::string& type,const Json& p){emit(type,p);if(type=="tool.output")output_seen=true;};
      runtime->chat("Exercise stop",stop_emit);CHECK(stopped && events[events.size()-2]["type"]=="turn.cancelled" && events.back()["type"]=="turn.finished");
      CHECK(runtime->persona().db->query("SELECT status FROM steering_messages ORDER BY id DESC LIMIT 1")[0]["status"]=="cancelled");
      CHECK(!fs::exists(project/"must-not-exist"));runtime->service({});
    }
    std::string unicode_request = "fix "; for (int i=0; i<60; ++i) unicode_request += "é";
    runtime->chat(unicode_request,emit);
    auto title = runtime->persona().db->query("SELECT title FROM tasks ORDER BY id DESC LIMIT 1")[0]["title"].get<std::string>();
    CHECK(title.size() <= 100); CHECK(Json(title).dump().find("é") != std::string::npos);
    auto& db=*runtime->persona().db; auto session=runtime->persona().session;
    auto original_messages=db.query("SELECT * FROM messages WHERE session_id=?",{session});
    auto soul=runtime->persona().soul;
    auto compact=runtime->command("compact",Json::object(),emit);CHECK(compact["status"]["compactions"]==1);
    CHECK(db.query("SELECT * FROM messages WHERE session_id=? AND id<=?",{session,original_messages.back()["id"]})==original_messages);
    CHECK(db.query("SELECT id FROM messages WHERE session_id=? AND id>? AND role!='system_internal'",{session,original_messages.back()["id"]}).empty());
    CHECK(runtime->persona().soul==soul);
    CHECK(!db.query("SELECT id FROM events WHERE session_id=? AND type='memory.extraction_partial'",{session}).empty());
    CHECK(db.query("SELECT id FROM context_checkpoints WHERE session_id=?",{session}).size()==1);
    runtime->command("compact");CHECK(runtime->command("status")["compactions"]==1);
    runtime->chat("Continue from the saved handoff",emit);
    CHECK(log->back().dump().find("checkpoint_id")!=std::string::npos);
    CHECK(log->back().dump().find("Continue from the saved handoff")!=std::string::npos);
    runtime->close("user_exit"); runtime.reset();
    CHECK(std::any_of(events.begin(),events.end(),[](auto& e){ return e["type"]=="assistant.completed"; }));
    fs::remove_all(root); std::cout << "PASS complete runtime lifecycle: streamed tool execution, approval, proofs, persistence, reflection, isolation, model replacement, new session and crash recovery\n"; return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL lifecycle: " << e.what() << '\n'; fs::remove_all(root); return 1; }
}
