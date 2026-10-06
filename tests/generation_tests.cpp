#include "check.hpp"
#include <saga/model.hpp>
#include <saga/runtime.hpp>
#include <algorithm>
using namespace saga;
namespace {
Json delta(Json body,Json finish=nullptr) {return {{"choices",Json::array({{{"index",0},{"delta",std::move(body)},{"finish_reason",std::move(finish)}}})}};}
GenerationState decode(const std::vector<Json>& chunks) {
  GenerationState result;OpenAIStreamAdapter adapter([&](auto& event){result.accept(event);});
  for(auto& chunk:chunks)adapter.feed(chunk);
  adapter.finish();return result;
}
void adapter_tests() {
  auto thought=delta({{"reasoning_content","private reasoning"}});
  auto stop=delta(Json::object(),"stop");
  CHECK(decode({thought,stop}).status==GenerationStatus::ReasoningOnly);
  CHECK(decode({thought,delta(Json::object(),"length")}).status==GenerationStatus::OutputLimit);
  CHECK(decode({delta({{"content","answer"}},"stop")}).content=="answer");
  auto mixed=decode({thought,delta({{"content","answer"}},"stop")});CHECK(mixed.reasoning=="private reasoning" && mixed.content=="answer");
  CHECK(decode({stop}).status==GenerationStatus::Empty);
  auto call=[](int index,Json function,std::string id=""){Json part={{"index",index},{"function",std::move(function)}};if(!id.empty())part["id"]=id;return delta({{"tool_calls",Json::array({part})}});};
  std::vector<Json> fragments={thought,call(0,{{"name","file_"}},"call-0")};
  fragments.push_back(call(0,{{"name","read"}},"call-0"));
  std::string args="{\"path\":\"existing.txt\"}";
  for(char c:args)fragments.push_back(call(0,{{"arguments",std::string(1,c)}}));
  fragments.push_back(call(1,{{"name","remember"},{"arguments","{\"query\":\"past\"}"}},"call-1"));
  fragments.push_back(delta(Json::object(),"tool_calls"));
  auto tools=decode(fragments);CHECK(tools.calls.size()==2 && tools.calls[0]["id"]=="call-0" && tools.calls[0]["function"]["name"]=="file_read");
  CHECK(Json::parse(tools.calls[0]["function"]["arguments"].get<std::string>())["path"]=="existing.txt");
  auto id_only=delta({{"tool_calls",Json::array({{{"id","id-only"},{"function",{{"name","remember"},{"arguments","{"}}}}})}});
  auto id_end=delta({{"tool_calls",Json::array({{{"id","id-only"},{"function",{{"name",nullptr},{"arguments","}"}}}}})}},"tool_calls");
  CHECK(decode({id_only,id_end}).calls[0]["function"]["arguments"]=="{}");
  rejects([&]{decode({call(0,{{"name","file_read"},{"arguments","{"}},"incomplete"),stop});});
  auto partial=decode({call(0,{{"name","file_read"},{"arguments","{"}},"incomplete"),delta(Json::object(),"length")});CHECK(partial.status==GenerationStatus::OutputLimit && partial.calls.size()==1);
  rejects([&]{decode({{{"choices","bad"}}});});
  rejects([&]{decode({delta({{"content",42}})});});
  rejects([&]{decode({delta({{"tool_calls",Json::array({{{"index","bad"}}})}})});});
  rejects([&]{decode({delta(Json::object(),"tool_calls")});});
  rejects([&]{decode({call(0,{{"name","remember"},{"arguments","{}"}},"same"),call(1,{{"name","remember"},{"arguments","{}"}},"same"),delta(Json::object(),"tool_calls")});});
  std::string deeply_nested(130,'[');deeply_nested+="0";deeply_nested+=std::string(130,']');
  rejects([&]{decode({call(0,{{"name","remember"},{"arguments",deeply_nested}},"deep"),delta(Json::object(),"tool_calls")});});
  rejects([&]{decode({thought});});
  GenerationState cancelled;cancelled.accept({ProviderEventKind::Reasoning,"kept"});cancelled.interrupt(GenerationStatus::Cancelled);cancelled.interrupt(GenerationStatus::TransportError);CHECK(cancelled.status==GenerationStatus::Cancelled && cancelled.reasoning=="kept");
  rejects([&]{cancelled.accept({ProviderEventKind::Text,"late"});});
  auto usage=delta({{"reasoning_content",std::string(24576,'r')}},"length");usage["usage"]={{"prompt_tokens",1000},{"completion_tokens",8192},{"completion_tokens_details",{{"reasoning_tokens",8192}}},{"prompt_tokens_details",{{"cached_tokens",800}}}};
  auto regression=decode({usage});CHECK(regression.status==GenerationStatus::OutputLimit && regression.reasoning.size()==24576 && regression.output_tokens==8192 && regression.reasoning_tokens==8192 && regression.cache_tokens==800);
  auto repeated=delta({{"content","once"}});Json final={{"choices",Json::array({{{"message",{{"content","once"}}},{"finish_reason","stop"}}})}};CHECK(decode({repeated,final}).content=="once");
  CHECK(decode({thought,final}).content=="once");
  final["choices"][0]["message"]["content"]="once more";CHECK(decode({repeated,final}).content=="once more");
  final["choices"][0]["message"]["content"]="different";rejects([&]{decode({repeated,final});});
  auto complete_call=Json{{"choices",Json::array({{{"message",{{"tool_calls",Json::array({{{"id","snap"},{"type","function"},{"function",{{"name","file_read"},{"arguments",args}}}}})}}},{"finish_reason","tool_calls"}}})}};
  CHECK(decode({call(0,{{"name","file_"},{"arguments","{\"path\":"}},"snap"),complete_call}).calls[0]["function"]["arguments"]==args);
  complete_call["choices"][0]["message"]["tool_calls"].push_back({{"id","snap-next"},{"type","function"},{"function",{{"name","remember"},{"arguments","{}"}}}});
  CHECK(decode({call(0,{{"name","file_"}},"snap"),complete_call}).calls.size()==2);
  GenerationState ordered;OpenAIStreamAdapter adapter([&](auto& e){ordered.accept(e);});adapter.feed(stop);adapter.finish();rejects([&]{adapter.finish();});
}
struct Segment {
  std::vector<ProviderEvent> events;
  bool fail=false;
};
Segment answer(std::string text="finished") {return {{{ProviderEventKind::Text,std::move(text)},{ProviderEventKind::Finished,"",Json::object(),FinishReason::Stop}},false};}
Segment tool(std::string id,std::string name,Json args) {return {{{ProviderEventKind::ToolDelta,"",{{"index",0},{"id",id},{"name",name},{"arguments",args.dump()}}},{ProviderEventKind::Finished,"",Json::object(),FinishReason::ToolCall}},false};}
Segment limited(bool stop=false) {return {{{ProviderEventKind::Reasoning,std::string(24576,'r')},{ProviderEventKind::Usage,"",{{"input_tokens",1000ULL},{"output_tokens",8192ULL},{"reasoning_tokens",8192ULL}}},{ProviderEventKind::Finished,"",Json::object(),stop?FinishReason::Stop:FinishReason::Length}},false};}
class ScriptBackend : public ModelBackend {
public:
  std::vector<Segment> segments;std::vector<ChatRequest> requests;size_t index=0;
  explicit ScriptBackend(std::vector<Segment> sequence):segments(std::move(sequence)){}
  ModelInfo discover() override {return {"script",65536,true,true,false};}
  CapabilityReport probe() override {return {true,true,true,true};}
  void generate(const ChatRequest& request,ProviderCallback emit) override {
    requests.push_back(request);CHECK(index<segments.size());auto segment=segments[index++];
    for(auto& event:segment.events){if(control_)control_();emit(event);}
    if(segment.fail)throw ProviderError(ProviderErrorKind::Connection,"Synthetic connection reset",true);
  }
};
struct Fixture {
  fs::path root=fs::temp_directory_path()/("saga-generation-"+uuid());
  Paths paths{root/"config",root/"data",root/"state",root/"run"};
  Config config;Json metadata,events=Json::array();
  Fixture(){paths.create();private_dir(root/"project");atomic_write(root/"project/existing.txt","fixture\n");Registry r(paths);metadata=r.create("Synthetic generation fixture","");config.endpoint="http://unused.invalid";config.model="script";config.context_length=65536;}
  ~Fixture(){fs::remove_all(root);}
  std::unique_ptr<Runtime> runtime(std::unique_ptr<ModelBackend> backend){auto r=std::make_unique<Runtime>(std::make_unique<PersonaContext>(paths,metadata,root/"project"),config,std::move(backend),[](auto&,auto&){return true;});r->start();return r;}
  Emit emit(){return [&](const std::string& type,const Json& data){events.push_back({{"type",type},{"payload",data}});};}
};
void run_scenario(std::vector<Segment> segments,int expected_tools,int continuations=0,int retries=0) {
  Fixture f;auto backend=std::make_unique<ScriptBackend>(std::move(segments));auto* script=backend.get();auto r=f.runtime(std::move(backend));r->chat("hello",f.emit());auto& db=*r->persona().db;
  auto turn=db.query("SELECT * FROM turns");CHECK(turn.size()==1 && turn[0]["status"]=="completed");
  auto generations=db.query("SELECT * FROM generations ORDER BY id");CHECK(generations.size()==script->requests.size());for(auto& g:generations)CHECK(g["turn_id"]==turn[0]["id"]);
  CHECK(db.query("SELECT * FROM tool_runs").size()==static_cast<size_t>(expected_tools));
  auto count=[&](std::string type){return std::count_if(f.events.begin(),f.events.end(),[&](auto& e){return e["type"]==type;});};
  CHECK(count("turn.started")==1 && count("turn.completed")==1 && count("turn.finished")==1);
  CHECK(count("generation.continuation")==continuations && count("generation.retry")==retries);
  CHECK(count("generation.started")==static_cast<int>(script->requests.size()));
  std::uint64_t sequence=0;
  for(auto& event:f.events){CHECK(event["payload"]["turn_id"]==turn[0]["id"]);auto next=event["payload"]["sequence_number"].get<std::uint64_t>();CHECK(next>sequence);sequence=next;CHECK(event["type"]!="model.empty_completion");}
  if(continuations) {
    CHECK(generations[0]["status"]=="output_limit" || generations[0]["status"]=="reasoning_only");
    auto state=Json::parse(generations[0]["state_json"].get<std::string>());CHECK(state["reasoning"].get<std::string>().size()==24576 && state["usage"]["output_tokens"]==8192);
    CHECK(script->requests[1].messages.dump().find(std::string(100,'r'))!=std::string::npos);
    CHECK(script->requests[1].max_tokens>script->requests[0].max_tokens);
  }
  if(expected_tools){CHECK(db.query("SELECT count(*) AS n FROM tool_runs WHERE turn_id IS NULL OR generation_id IS NULL OR tool_call_id IS NULL")[0]["n"]==0);CHECK(!db.query("SELECT id FROM events WHERE type='tool.result.committed'").empty());}
}
void runtime_tests() {
  run_scenario({answer()},0);
  auto simultaneous=tool("first","file_read",{{"path","existing.txt"}});
  simultaneous.events.insert(simultaneous.events.begin()+1,{ProviderEventKind::ToolDelta,"",{{"index",1},{"id","second"},{"name","file_read"},{"arguments",Json{{"path","existing.txt"}}.dump()}}});
  run_scenario({simultaneous,answer()},2);
  run_scenario({{{{ProviderEventKind::Finished,"",Json::object(),FinishReason::Stop}},false},answer()},0);
  run_scenario({tool("read","file_read",{{"path","existing.txt"}}),answer()},1);
  run_scenario({tool("read1","file_read",{{"path","existing.txt"}}),tool("read2","file_read",{{"path","existing.txt"}}),answer()},2);
  auto commentary=tool("read","file_read",{{"path","existing.txt"}});commentary.events.insert(commentary.events.begin(),{ProviderEventKind::Commentary,"I checked the fixture; I will read it next."});run_scenario({commentary,answer()},1);
  run_scenario({limited(),answer()},0,1);
  run_scenario({limited(),tool("read","file_read",{{"path","existing.txt"}}),answer()},1,1);
  run_scenario({limited(true),answer()},0,1);
  run_scenario({{{},true},answer()},0,0,1);
  run_scenario({tool("write","file_write",{{"path","written.txt"},{"content","once\n"},{"description","Synthetic once-only write"}}),{{{ProviderEventKind::Reasoning,"preserved after reset"}},true},answer()},1,0,1);
  {Fixture f;
    auto next=f.root/"workspace";
    auto create=tool("new-task","task_create",{{"title","Workspace fixture"},{"objective","Create the requested program"},{"risk","low"},{"checks",Json::array({"Observed result"})}});
    auto open=tool("workspace","project_open",{{"path",next.string()},{"create",true}});
    auto backend=std::make_unique<ScriptBackend>(std::vector<Segment>{create,open,open,create,answer(),answer()});auto* script=backend.get();auto r=f.runtime(std::move(backend));
    auto input="Create a program in "+next.string();r->chat(input,f.emit(),"operator-message");
    auto requests=script->requests.size();r->chat(input,f.emit(),"operator-message");CHECK(script->requests.size()==requests);
    auto& db=*r->persona().db;
    CHECK(db.query("SELECT count(*) AS n FROM turns")[0]["n"]==1 && db.query("SELECT count(*) AS n FROM messages WHERE role='user'")[0]["n"]==1);
    CHECK(db.query("SELECT count(*) AS n FROM tasks")[0]["n"]==1);
    CHECK(db.query("SELECT count(*) AS n FROM events WHERE type='workspace.changed'")[0]["n"]==1);
    CHECK(db.query("SELECT count(*) AS n FROM tool_runs WHERE tool_call_id='workspace'")[0]["n"]==1);
    CHECK(r->persona().project_root==next);
    rejects([&]{r->chat("different",f.emit(),"operator-message");});
  }
  {Fixture f;f.config.default_max_output_tokens=16;f.config.hard_max_output_tokens=32;auto first=limited();first.events[1].data["output_tokens"]=16ULL;auto backend=std::make_unique<ScriptBackend>(std::vector<Segment>{first,answer()});auto* script=backend.get();auto r=f.runtime(std::move(backend));r->chat("hello",f.emit());CHECK(script->requests[0].max_tokens==16 && script->requests[1].max_tokens==32);}
  {Fixture f;auto r=f.runtime(std::make_unique<ScriptBackend>(std::vector<Segment>{limited()}));bool stopped=false;r->service([&]{if(!stopped && !r->persona().db->query("SELECT id FROM events WHERE type='reasoning.started'").empty()){stopped=true;r->command("stop");}});r->chat("hello",f.emit());CHECK(r->persona().db->query("SELECT status FROM turns")[0]["status"]=="cancelled");CHECK(r->persona().db->query("SELECT status FROM generations")[0]["status"]=="cancelled");auto cancelled=r->persona().db->query("SELECT payload_json FROM events WHERE type='turn.cancelled'");CHECK(cancelled.size()==1 && Json::parse(cancelled[0]["payload_json"].get<std::string>()).contains("turn_id"));}
  {Fixture f;auto r=f.runtime(std::make_unique<ScriptBackend>(std::vector<Segment>{answer()}));auto& db=*r->persona().db;
   db.exec("INSERT INTO turns(id,session_id,status,phase,started_at) VALUES('crashed',?,'active','executing_tool',?)",{r->persona().session,now()});
   db.exec("INSERT INTO model_calls(session_id,purpose,model,started_at,status) VALUES(?,'chat','script',?,'completed')",{r->persona().session,now()});auto model=db.query("SELECT max(id) AS id FROM model_calls")[0]["id"];
   db.exec("INSERT INTO generations(id,turn_id,session_id,purpose,status,started_at,requested_max_output_tokens,effective_max_output_tokens,input_estimate) VALUES(?,'crashed',?,'chat','completed',?,16,16,1)",{model,r->persona().session,now()});
   db.exec("INSERT INTO tool_runs(session_id,tool,arguments_json,started_at,status,turn_id,generation_id,tool_call_id) VALUES(?,'file_write','{}',?,'completed','crashed',?,'done')",{r->persona().session,now(),model});
   db.exec("INSERT INTO tool_runs(session_id,tool,arguments_json,started_at,status,turn_id,generation_id,tool_call_id) VALUES(?,'file_write','{}',?,'running','crashed',?,'unknown')",{r->persona().session,now(),model});
   r->close("user_exit");r.reset();auto backend=std::make_unique<ScriptBackend>(std::vector<Segment>{answer()});auto* script=backend.get();r=f.runtime(std::move(backend));CHECK(script->requests.empty());CHECK(r->persona().db->query("SELECT status FROM turns WHERE id='crashed'")[0]["status"]=="interrupted");CHECK(r->persona().db->query("SELECT status FROM tool_runs WHERE tool_call_id='done'")[0]["status"]=="completed");CHECK(!r->persona().db->query("SELECT * FROM events WHERE type='tool.execution.unknown_after_crash'").empty());}
}
}
int main(){try{adapter_tests();runtime_tests();std::cout<<"PASS normalized provider streams, durable turns, output-limit continuation, transport retries, tools, cancellation and crash recovery\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL generations: "<<e.what()<<'\n';return 1;}}
