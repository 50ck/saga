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
    Json state=Json::object(),last=Json::object();
    for(auto &message:request.messages){
      if(!message.value("content",Json()).is_string())continue;
      auto text=message["content"].get<std::string>();
      auto marker=std::string("Runtime attention update (data):\n");
      if(message["role"]=="user" && text.starts_with(marker))state=Json::parse(text.substr(marker.size()));
      if(message["role"]=="tool")last=Json::parse(text);
    }
    claims_=state.value("research_questions",Json::array());
    auto claim=[&](const std::string &word)->Id {for(auto &c:claims_)if(c["question"].get<std::string>().find(word)!=std::string::npos)return c["id"];throw std::runtime_error("Missing scenario claim");};
    callback({{"choices",Json::array({{{"index",0},{"delta",{{"reasoning_content","private activity"}}}}})}});
    auto invoke=[&](const std::string &name,Json args){callback({{"choices",Json::array({{{"index",0},{"delta",{{"tool_calls",Json::array({{{"index",0},{"id","scenario-"+std::to_string(step_)},{"function",{{"name",name},{"arguments",args.dump()}}}}})}}},{"finish_reason","tool_calls"}}})}});};
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
  research.dispatch("research_resolve",{{"id",claim},{"status","supported"},{"conclusion","Clockwise rotation is documented."},{"sources",Json::array({{{"source_id",source["source_id"]},{"quote","Rotation is clockwise."}}})}},session,0);
  CHECK(research.unresolved_required(session,0).empty());CHECK(p.db->query("SELECT id FROM research_attempts WHERE succeeded=0").size()==1);
  auto user=p.db->event("user.message",{{"content","Implement a program with C in /tmp/widget; inspect modern rules."}},session);
  rejects([&]{research.question("Implement a program with C in /tmp/widget; inspect modern rules.",true,session,0);});
  Json plan={{"intent","Implement a program"},{"operator_constraints",{"C","/tmp/widget"}},{"desired_actions",{"Compile"}},{"goals",Json::array({{{"question","Modern rules"},{"required",true},{"claims",{"Modern rotation rule?"}}}})}};
  auto result=research.dispatch("research_plan",plan,session,0);CHECK(result["plans"][0]["goals"][0]["claims"].size()==1);CHECK(user>0);
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
}
int main(){try{complete_regression();failure_is_not_verification();return 0;}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
