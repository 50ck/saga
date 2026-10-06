#include "check.hpp"
#include "mock_model.hpp"
#include <saga/runtime.hpp>
#include <saga/search.hpp>
using namespace saga;
namespace {
WebResponse response(const std::string &url,std::string body,long status=200){WebResponse r;r.url=url;r.content_type=body.starts_with('{') ? "application/json":"text/html";r.body=std::move(body);r.status=status;return r;}
struct Fixture {
  fs::path root=fs::temp_directory_path()/("saga-research-regression-"+uuid());
  Paths paths{root/"config",root/"data",root/"state",root/"run"};
  Fixture(){paths.create();private_dir(root/"project");}
  ~Fixture(){fs::remove_all(root);}
};
class Scenario final: public MockChatBackend {
  fs::path workspace_;
  int step_=0;
  Json claims_=Json::array();std::vector<Id> sources_;
public:
  explicit Scenario(fs::path workspace):workspace_(std::move(workspace)){}
  ModelInfo discover() override{return {"research-regression",65536,true,true,false};}
  CapabilityReport probe() override{return {true,true,true,true};}
  void chat(const ChatRequest &request,StreamCallback callback) override {
    Json state=Json::object(),last=Json::object();size_t attention_count=0;
    for(auto &message:request.messages){
      if(!message.value("content",Json()).is_string())continue;
      auto text=message["content"].get<std::string>();
      auto marker=std::string("Runtime attention update (data):\n");
      if(message["role"]=="user" && text.starts_with(marker)) {state=Json::parse(text.substr(marker.size()));++attention_count;}
      if(message["role"]=="tool")last=Json::parse(text);
    }
    CHECK(attention_count>=1);
    claims_=state.value("research_questions",Json::array());
    auto claim=[&](const std::string &word)->Id {for(auto &c:claims_)if(c["question"].get<std::string>().find(word)!=std::string::npos)return c["id"];throw std::runtime_error("Missing scenario claim");};
    callback({{"choices",Json::array({{{"index",0},{"delta",{{"reasoning_content","private activity"}}}}})}});
    auto invoke=[&](const std::string &name,Json args){if(name=="research_resolve") {for(auto &c:claims_)if(c["id"]==args["id"])args["assessment"]={{"proposition",c["question"]},{"coverage","full"},{"rationale","The fixture passage directly documents this behavior."}};for(auto &cite:args["sources"])cite["relation"]="supports";}callback({{"choices",Json::array({{{"index",0},{"delta",{{"tool_calls",Json::array({{{"index",0},{"id","scenario-"+std::to_string(step_)},{"function",{{"name",name},{"arguments",args.dump()}}}}})}}},{"finish_reason","tool_calls"}}})}});};
    switch(step_++) {
      case 0:invoke("task_create",{{"title","Terminal game"},{"objective","Implement modern Tetris"},{"risk","low"},{"checks",Json::array({"Game compiles"})}});break;
      case 1:invoke("project_open",{{"path",workspace_.string()},{"create",true}});break;
      case 2:CHECK(!last.contains("error"));invoke("task_create",{{"title","Terminal game"},{"objective","Implement modern Tetris"},{"risk","low"},{"checks",Json::array({"Game compiles"})}});break;
      case 3:invoke("project_open",{{"path",workspace_.string()},{"create",true}});break;
      case 4:invoke("research_plan",{{"intent","Implement modern terminal Tetris"},{"operator_constraints",{"C","ncurses","minimal dependencies","requested controls",workspace_.string()}},{"desired_actions",{"Implement the game","Compile it"}},{"goals",Json::array({{{"question","Modern rotation rules"},{"required",true},{"claims",{"SRS wall-kick behavior"}}},{{"question","Modern randomization"},{"required",true},{"claims",{"7-bag randomizer behavior"}}},{{"question","Modern twist detection"},{"required",true},{"claims",{"T-spin corner recognition"}}}})}});break;
      case 5:CHECK(!last.contains("error"));invoke("web_search",{{"query","SRS wall kicks"},{"question_id",claim("SRS")}});break;
      case 6:CHECK(!last.contains("error"));invoke("web_search",{{"query","7-bag randomizer"},{"question_id",claim("7-bag")}});break;
      case 7:CHECK(!last.contains("error"));invoke("web_search",{{"query","T-spin detection"},{"question_id",claim("T-spin")}});break;
      case 8:CHECK(last["engine"]=="fourget");invoke("web_read",{{"url","https://rules.example.com/srs"},{"question_id",claim("SRS")}});break;
      case 9:sources_.push_back(last["source_id"]);invoke("web_read",{{"url","https://rules.example.com/bag"},{"question_id",claim("7-bag")}});break;
      case 10:sources_.push_back(last["source_id"]);invoke("web_read",{{"url","https://rules.example.com/spin"},{"question_id",claim("T-spin")}});break;
      case 11:sources_.push_back(last["source_id"]);invoke("research_resolve",{{"id",claim("SRS")},{"status","supported"},{"conclusion","Fixture SRS behavior is documented."},{"sources",Json::array({{{"source_id",sources_[0]},{"quote","SRS uses ordered wall-kick tests."}}})}});break;
      case 12:CHECK(!last.contains("error"));invoke("research_resolve",{{"id",claim("7-bag")},{"status","supported"},{"conclusion","Fixture 7-bag behavior is documented."},{"sources",Json::array({{{"source_id",sources_[1]},{"quote","7-bag shuffles the seven tetrominoes once per bag."}}})}});break;
      case 13:CHECK(!last.contains("error"));invoke("research_resolve",{{"id",claim("T-spin")},{"status","supported"},{"conclusion","Fixture T-spin detection is documented."},{"sources",Json::array({{{"source_id",sources_[2]},{"quote","T-spin recognition examines occupied corners after rotation."}}})}});break;
      case 14:CHECK(!last.contains("error"));invoke("shell_exec",{{"command","command -v cc"},{"execution","host"}});break;
      default:callback({{"choices",Json::array({{{"index",0},{"delta",{{"content","Research and compiler preflight completed; implementation still pending."}}},{"finish_reason","stop"}}})}});break;
    }
  }
};
void complete_regression() {
  Fixture fixture;Registry registry(fixture.paths);Config config;config.endpoint="http://example.invalid";config.model="fixture";config.context_length=65536;
  int ddg=0,directory=0,probes=0,api=0,reads=0;
  auto transport=[&](const std::string &url,const std::string &,const std::function<void()> &control){
    if(control)control();
    if(url.find("duckduckgo")!=std::string::npos){++ddg;if(ddg==3)return response(url,"<form id='challenge-form'>",202);return response(url,"<div class='result'><h2 class='result__title'><a href='https://rules.example.com/srs'>SRS</a></h2><p class='result__snippet'>Search discovery only.</p></div>");}
    if(url=="https://4get.ca/instances"){++directory;return response(url,"<noscript><table><tr><th>Server</th></tr><tr><td><a href='https://search.example.com'>Instance</a></td></tr></table></noscript>");}
    if(url.ends_with("/ami4get")){++probes;return response(url,R"({"status":"ok","service":"4get","server":{"api_enabled":true,"bot_protection":0,"version":9}})");}
    if(url.find("/api/v1/web")!=std::string::npos){++api;return response(url,R"({"status":"ok","web":[{"title":"T-spin","url":"https://rules.example.com/spin","description":null}],"npt":null})");}
    ++reads;
    std::string body=url.ends_with("/srs") ? "SRS uses ordered wall-kick tests." : url.ends_with("/bag") ? "7-bag shuffles the seven tetrominoes once per bag." : "T-spin recognition examines occupied corners after rotation.";
    return response(url,"<main><h1>Modern rules</h1><p>"+body+"</p></main>");
  };
  auto workspace=fixture.root/"tetris";
  Runtime runtime(std::make_unique<PersonaContext>(fixture.paths,registry.create("Synthetic scenario",""),fixture.root/"project"),config,std::make_unique<Scenario>(workspace),[](auto&,auto&){return true;},transport);
  runtime.start();runtime.command("permissions",{{"mode","host_always"}});
  Json events=Json::array();std::string input="Create a modern Tetris in "+workspace.string()+"; search online for SRS, 7-bag and T-spins.";
  auto emit=[&](auto &type,auto &payload){events.push_back({{"type",type},{"payload",payload}});};
  runtime.chat(input,emit,"one-human-message");
  auto &db=*runtime.persona().db;
  CHECK(db.query("SELECT id FROM turns").size()==1);CHECK(db.query("SELECT id FROM tasks").size()==1);CHECK(db.query("SELECT id FROM events WHERE type='user.message'").size()==1);CHECK(db.query("SELECT id FROM events WHERE type='workspace.changed'").size()==1);
  auto claims=db.query("SELECT question,status,sources_json FROM research_questions");CHECK(claims.size()==3);
  for(auto &claim:claims){CHECK(claim["question"]!=input);CHECK(claim["status"]=="supported");CHECK(!Json::parse(claim["sources_json"].get<std::string>()).empty());}
  CHECK(ddg==3 && directory==1 && probes==1 && api==1 && reads==3);
  CHECK(db.query("SELECT id FROM research_attempts WHERE succeeded=0").empty()); // Engine challenge recovered; overall search succeeded.
  CHECK(std::any_of(events.begin(),events.end(),[](auto &event){return event["type"]=="search.fallback";}));
  bool generation_after_tool=false,thought_after_tool=false;
  for(size_t i=1;i<events.size();++i)if(events[i]["type"]=="generation.started") {
    generation_after_tool=generation_after_tool || std::any_of(events.begin(),events.begin()+static_cast<std::ptrdiff_t>(i),[](auto &event){return event["type"]=="tool.completed";});
    for(size_t j=i+1;j<events.size() && events[j]["type"]!="generation.completed";++j)if(events[j]["type"]=="reasoning.started")thought_after_tool=true;
  }
  CHECK(generation_after_tool && thought_after_tool);
  auto before=db.query("SELECT id FROM tool_runs").size();runtime.chat(input,emit,"one-human-message");CHECK(db.query("SELECT id FROM tool_runs").size()==before);
  for(auto &event:events)if(event["type"]=="research.warning")CHECK(event["payload"]["content"].get<std::string>().find(input)==std::string::npos);
}
void failure_is_not_verification() {
  Fixture f;Registry registry(f.paths);PersonaContext p(f.paths,registry.create("Claim fixture",""),f.root/"project");Config config;
  auto session=p.db->exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()});
  config.search_engines={"duckduckgo"};
  WebResearch research(*p.db,config,[](auto &url,auto&,auto&){if(url.find("duckduckgo")!=std::string::npos)return response(url,"challenge-form",202);return response(url,"<main>Rotation is clockwise.</main>");});
  research.begin_turn(session,0);auto claim=research.question("Rotation direction?",true,session,0);
  rejects([&]{research.dispatch("web_search",{{"query","rotation"},{"question_id",claim}},session,0);});
  research.account_for_pending(session,0,{});CHECK(research.questions(session,0)[0]["status"]=="pending");
  auto source=research.dispatch("web_read",{{"url","https://rules.example.com/rotation"},{"question_id",claim}},session,0);
  CHECK(p.db->query("SELECT engine FROM web_sources WHERE id=?",{source["source_id"]})[0]["engine"]=="web_acquisition");
  research.dispatch("research_resolve",{{"id",claim},{"status","supported"},{"assessment",{{"proposition","Rotation direction?"},{"coverage","full"},{"rationale","The fetched fixture directly specifies the direction."}}},{"conclusion","Clockwise rotation is documented."},{"sources",Json::array({{{"source_id",source["source_id"]},{"quote","Rotation is clockwise."},{"relation","supports"}}})}},session,0);
  CHECK(research.unresolved_required(session,0).empty());CHECK(p.db->query("SELECT id FROM research_attempts WHERE succeeded=0").size()==1);
  auto user=p.db->event("user.message",{{"content","Implement a program with C in /tmp/widget; inspect modern rules."}},session);
  rejects([&]{research.question("Implement a program with C in /tmp/widget; inspect modern rules.",true,session,0);});
  Json plan={{"intent","Implement a program"},{"operator_constraints",{"C","/tmp/widget"}},{"desired_actions",{"Compile"}},{"goals",Json::array({{{"question","Modern rules"},{"required",true},{"claims",{"Modern rotation rule?"}}}})}};
  auto result=research.dispatch("research_plan",plan,session,0);CHECK(result["goals"][0]["claims"].size()==1);CHECK(user>0);
  auto first_task=p.db->exec("INSERT INTO tasks(session_id,title,objective,status,risk,created_at) VALUES(?,'first','fixture','active','low',?)",{session,now()});
  auto second_task=p.db->exec("INSERT INTO tasks(session_id,title,objective,status,risk,created_at) VALUES(?,'second','fixture','active','low',?)",{session,now()});
  auto turn=uuid();p.db->exec("INSERT INTO turns(id,session_id,status,phase,started_at) VALUES(?,?,'active','thinking',?)",{turn,session,now()});
  research.begin_turn(session,first_task,turn);research.require_plan(user,turn,session,first_task);
  auto scoped=research.plan(plan,session,first_task);auto count=p.db->query("SELECT id FROM research_questions").size();
  CHECK(research.plan(plan,session,first_task)["reused"]==true);CHECK(p.db->query("SELECT id FROM research_questions").size()==count);
  research.rebind_task(first_task,second_task,turn);
  CHECK(p.db->query("SELECT task_id FROM research_plans WHERE id=?",{scoped["plan_id"]})[0]["task_id"]==second_task);
  CHECK(!research.unresolved_required(session,second_task).empty());
  config.endpoint="http://example.invalid";config.model="fixture";config.context_length=65536;
  p.project=p.db->exec("INSERT INTO projects(name,root_path,created_at,last_seen_at) VALUES('Fixture',?,?,?)",{p.project_root.string(),now(),now()});
  Memory memory(p,config);Tools tools(p,memory,[](auto&,auto&){return true;},config);
  p.session=session;p.turn=turn;p.task=0;auto tasks_before=p.db->query("SELECT id FROM tasks").size();
  auto first=tools.execute("task_create",{{"title","Risk refinement"},{"objective","Fixture"},{"risk","low"},{"checks",{"Check A"}}});
  CHECK(!first.contains("error"));
  auto refined=tools.execute("task_create",{{"title","Risk refinement"},{"objective","Fixture"},{"risk","high"},{"checks",{"Check B"}}});
  CHECK(first["id"]==refined["id"] && refined["reused"]==true);CHECK(p.db->query("SELECT id FROM tasks").size()==tasks_before+1);
  CHECK(p.db->query("SELECT risk FROM tasks WHERE id=?",{first["id"]})[0]["risk"]=="high");
  auto invalid=plan;invalid["goals"][0]["claims"]={"Implement a program with C in /tmp/widget; inspect modern rules."};auto before=p.db->query("SELECT id FROM research_goals").size();rejects([&]{research.plan(invalid,session,0);});CHECK(p.db->query("SELECT id FROM research_goals").size()==before);
}
void reference_contract() {
  Fixture f;Registry registry(f.paths);PersonaContext p(f.paths,registry.create("Reference fixture",""),f.root/"project");Config config;
  config.endpoint="http://example.invalid";config.model="fixture";config.context_length=65536;config.search_engines={"duckduckgo"};
  auto &db=*p.db;
  auto old_session=db.exec("INSERT INTO sessions(started_at,status) VALUES(?,'completed')",{now()});
  for(int i=0;i<4;++i)db.exec("INSERT INTO research_questions(session_id,question,required,status,created_at,updated_at) VALUES(?,?,1,'pending',?,?)",{old_session,"Legacy gap "+std::to_string(i),now(),now()});
  p.session=db.exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()});
  p.project=db.exec("INSERT INTO projects(name,root_path,created_at,last_seen_at) VALUES('Fixture',?,?,?)",{p.project_root.string(),now(),now()});
  p.turn=uuid();db.exec("INSERT INTO turns(id,session_id,status,phase,started_at) VALUES(?,?,'active','thinking',?)",{p.turn,p.session,now()});
  auto user=db.event("user.message",{{"content","Resume the terminal game; research its rules, implement and compile it."}},p.session);
  int requests=0;
  Memory memory(p,config);Tools tools(p,memory,[](auto&,auto&){return true;},config,[&](auto &url,auto&,auto &control){++requests;if(control)control();if(url.find("duckduckgo")!=std::string::npos)return response(url,"<p class='no-results'>No results found</p>");return response(url,"<main><p>Fixture rule text.</p></main>");});
  tools.web().begin_turn(p.session,0,p.turn);tools.web().require_plan(user,p.turn,p.session,0);
  Json plan={{"intent","Build the game"},{"operator_constraints",{"C","minimal dependencies"}},{"desired_actions",{"Compile it"}},{"goals",Json::array({
    {{"question","Rotation rules"},{"claims",{"Rotation states?","Kick ordering?","Piece groups?","Transition table?"}}},
    {{"question","Randomization rules"},{"claims",{"Bag contents?","Bag shuffle?","Preview behavior?"}}},
    {{"question","Twist rules"},{"claims",{"Corner test?","Mini classification?","Scoring?","Rotation requirement?"}}}})}};
  auto result=tools.execute("research_plan",plan);CHECK(!result.contains("error"));CHECK(result["goals"].size()==3 && !result.contains("plans"));
  CHECK(result["goals"][0]["goal_id"]==1);CHECK(result["goals"][0]["claims"][0]["claim_id"]==5);
  CHECK(db.query("SELECT id FROM research_questions WHERE session_id=? AND required=1",{p.session}).size()==11);
  CHECK(tools.execute("research_plan",plan)["reused"]==true);CHECK(db.query("SELECT id FROM research_goals").size()==3);
  for(Id wrong:{1,2,3}) {
    auto error=tools.execute("web_search",{{"query","Fixture query "+std::to_string(wrong)},{"question_id",wrong}});
    CHECK(error["error_type"]=="ResearchReferenceError");CHECK(error["available_claims"].size()==11);CHECK(error["available_goals"].size()==3);
    CHECK(std::none_of(error["available_claims"].begin(),error["available_claims"].end(),[](auto &c){return c["claim_id"].template get<Id>()<5;}));
  }
  CHECK(requests==0);CHECK(db.query("SELECT id FROM research_attempts").empty());
  auto unresolved=tools.execute("research_resolve",{{"id",1},{"status","unverified"},{"conclusion","Cannot verify"},{"sources",Json::array()}});
  CHECK(unresolved["error_type"]=="ResearchReferenceError");
  for(Id goal:{1,2,3}) {
    auto search=tools.execute("web_search",{{"query","Goal query "+std::to_string(goal)},{"goal_id",goal}});
    CHECK(!search.contains("error"));CHECK(search["goal_id"]==goal);
  }
  CHECK(requests==3);CHECK(db.query("SELECT id FROM research_attempts WHERE succeeded=1").size()==11);
  CHECK(db.query("SELECT id FROM research_questions WHERE session_id=? AND status='pending'",{p.session}).size()==11);
  auto mismatch=tools.execute("web_search",{{"query","Wrong goal"},{"goal_id",1},{"question_id",9}});CHECK(mismatch["error_type"]=="ResearchReferenceError");CHECK(requests==3);
  auto source=tools.execute("web_read",{{"url","https://rules.example.com/rotation"},{"goal_id",1}});CHECK(source["claim_ids"].size()==4);
  CHECK(db.query("SELECT id FROM research_attempts WHERE source_id=?",{source["source_id"]}).size()==4);
  auto unsupported=tools.execute("research_resolve",{{"id",5},{"status","supported"},{"conclusion","Claim proved"},{"sources",Json::array()}});CHECK(unsupported.contains("error"));
  auto task=tools.execute("task_create",{{"title","Resume game"},{"objective","Fixture"},{"risk","low"},{"checks",{"Compile"}}});CHECK(!task.contains("error"));
  CHECK(db.query("SELECT id FROM research_questions WHERE task_id=?",{p.task}).size()==11);
  CHECK(db.query("SELECT task_id FROM research_plans WHERE id=?",{result["plan_id"]})[0]["task_id"]==p.task);
  auto other_task=db.exec("INSERT INTO tasks(session_id,title,objective,status,created_at) VALUES(?,'Other','Unrelated','active',?)",{p.session,now()});
  auto unrelated=tools.web().question("Unrelated workspace gap?",true,p.session,other_task);
  CHECK(tools.execute("web_search",{{"query","Unrelated"},{"question_id",unrelated}})["error_type"]=="ResearchReferenceError");
  CHECK(tools.execute("research_resolve",{{"id",unrelated},{"status","unverified"},{"conclusion","Not relevant"},{"sources",Json::array()}})["error_type"]=="ResearchReferenceError");
  auto status=tools.execute("research_status",Json::object());CHECK(status["available_claims"].size()==11);
  auto context=research_plan_context(db,p.session,p.task);CHECK(context.size()==1 && context[0]["plan_id"]==result["plan_id"]);CHECK(!context[0]["decomposition"].contains("goals"));
  ContextBuilder builder(p,memory,config);
  builder.build({{"large_observation",std::string(40000,'x')}});
  auto attention=Json::parse(db.query("SELECT payload_json FROM events WHERE type='context.attention' ORDER BY id DESC LIMIT 1")[0]["payload_json"].get<std::string>());
  CHECK(attention["state"]["research_references"].size()==11);
  for(auto &ref:attention["state"]["research_references"])CHECK(ref["claim_id"].get<Id>()>=5 && ref["claim_id"].get<Id>()<=15);
  CHECK(tools.execute("web_read",{{"url","https://rules.example.com/rotation"},{"goal_id",1},{"question_id",5}})["claim_ids"].size()==1);
  auto attempts=db.query("SELECT id FROM research_attempts").size();tools.web().service([]{throw TurnCancelled();});
  bool cancelled=false;try{tools.execute("web_read",{{"url","https://rules.example.com/cancel"},{"goal_id",1}});}catch(const TurnCancelled &){cancelled=true;}
  CHECK(cancelled && db.query("SELECT id FROM research_attempts").size()==attempts);
}

void plan_defaults() {
  Fixture f;Registry registry(f.paths);PersonaContext p(f.paths,registry.create("Defaults fixture",""),f.root/"project");
  auto session=p.db->exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()});
  WebResearch research(*p.db,Config{});research.begin_turn(session,0);
  Json args={{"intent","Inspect a technical topic"},{"operator_constraints",Json::array()},{"desired_actions",Json::array()},
    {"goals",Json::array({{{"question","Optional external gap?"},{"claims",{"Optional rule?"}}}})}};
  CHECK(research.plan(args,session,0)["goals"][0]["required"]==0);
  auto turn=uuid();p.db->exec("INSERT INTO turns(id,session_id,status,phase,started_at) VALUES(?,?,'active','thinking',?)",{turn,session,now()});
  auto user=p.db->event("user.message",{{"content","Research an external specification."}},session);
  research.begin_turn(session,0,turn);research.require_plan(user,turn,session,0);
  auto optional=args;optional["goals"][0]["required"]=false;
  auto before=p.db->query("SELECT id FROM research_questions").size();rejects([&]{research.plan(optional,session,0);});
  CHECK(research.plan_pending(session,0));CHECK(p.db->query("SELECT id FROM research_questions").size()==before);
  args["goals"][0]["claims"]=Json::array();
  for(int i=0;i<32;++i)args["goals"][0]["claims"].push_back("Rule "+std::to_string(i)+" "+std::string(450,'r'));
  auto result=research.plan(args,session,0);CHECK(result["goals"][0]["claims"].size()==32 && result.dump().size()<32000);
  CHECK(result["goals"][0]["claims"][0]["proposition"]==args["goals"][0]["claims"][0]);
  CHECK(result["goals"][0]["required"]==1);CHECK(research.plan(args,session,0)["reused"]==true);
  auto stored=Json::parse(p.db->query("SELECT decomposition_json FROM research_plans WHERE id=?",{result["plan_id"]})[0]["decomposition_json"].get<std::string>());
  CHECK(stored["goals"][0]["claims"][0].get<std::string>().size()>450);
}

void orphan_binding_and_disabled_plan() {
  Fixture f;Registry registry(f.paths);PersonaContext p(f.paths,registry.create("Binding fixture",""),f.root/"project");Config config;
  config.endpoint="http://example.invalid";config.model="fixture";config.context_length=65536;
  p.session=p.db->exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()});
  p.project=p.db->exec("INSERT INTO projects(name,root_path,created_at,last_seen_at) VALUES('Fixture',?,?,?)",{p.project_root.string(),now(),now()});
  p.turn=uuid();p.db->exec("INSERT INTO turns(id,session_id,status,phase,started_at) VALUES(?,?,'active','thinking',?)",{p.turn,p.session,now()});
  Memory memory(p,config);Tools tools(p,memory,[](auto&,auto&){return true;},config);auto &web=tools.web();web.begin_turn(p.session,0,p.turn);
  auto question=tools.execute("research_question",{{"question","Additional external gap?"},{"required",true}});CHECK(question["claim_id"]==question["id"] && question.contains("goal_id"));
  CHECK(!tools.execute("task_create",{{"title","Fixture"},{"objective","Inspect"},{"risk","low"},{"checks",{"Checked"}}}).contains("error"));
  CHECK(p.db->query("SELECT task_id FROM research_questions WHERE id=?",{question["id"]})[0]["task_id"]==p.task);
  auto user=p.db->event("user.message",{{"content","Research the external reference."}},p.session,p.task);web.require_plan(user,p.turn,p.session,p.task);
  web.settings(false);web.account_for_pending(p.session,p.task,{});CHECK(p.db->query("SELECT status FROM research_plans WHERE turn_id=?",{p.turn})[0]["status"]=="unverified");
  web.settings(true);
  Json plan={{"intent","Inspect the reference"},{"operator_constraints",Json::array()},{"desired_actions",Json::array()},
    {"goals",Json::array({{{"question","Reference behavior?"},{"claims",{"Documented behavior?"}}}})}};
  auto result=tools.execute("research_plan",plan);CHECK(!result.contains("error"));CHECK(result["goals"][0]["required"]==1);
  CHECK(p.db->query("SELECT id FROM research_plans WHERE turn_id=?",{p.turn}).size()==1);
}

}
int main(){try{complete_regression();failure_is_not_verification();reference_contract();plan_defaults();orphan_binding_and_disabled_plan();return 0;}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
