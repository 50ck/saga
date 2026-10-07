#include "check.hpp"
#include "mock_model.hpp"
#include <saga/web.hpp>
#include <saga/persona.hpp>
#include <saga/runtime.hpp>
using namespace saga;
namespace {
WebResponse page(std::string body,long status=200) {WebResponse r;r.url="https://html.duckduckgo.com/html/";r.body=std::move(body);r.content_type="text/html";r.status=status;return r;}
const char* search_html=R"HTML(<html><div class="result"><a class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fdocs.example.com%2Frules%3Fx%3D1%26y%3D2">Rules &amp; examples</a><a class="result__snippet">Version 2 rotation rules.</a></div><div class="result"><a class="result__a" href="https://spam.test/rules">Spam</a><p class="result__snippet">Unrelated neighboring snippet.</p></div><div class="result"><a class="result__a" href="https://docs.example.com/rules?x=1&amp;y=2">Duplicate</a></div><form><input type="hidden" name="q" value="rules"><input type="hidden" name="s" value="30"><input type="hidden" name="vqd" value="synthetic-token"></form></html>)HTML";
void parsing() {
  SearchRequest query{"site:example.com \"rotation rules\" +modern -old","",5,{"example.com"},{}};
  auto result=DuckDuckGoEngine::parse(page(search_html),query);
  CHECK(result["results"].size()==1);CHECK(result["results"][0]["snippet"]=="Version 2 rotation rules.");CHECK(result["results"][0]["url"]=="https://docs.example.com/rules?x=1&y=2");CHECK(result["results"][0]["title"]=="Rules & examples");CHECK(result.contains("next_cursor"));
  query.exclude_domains={"docs.example.com"};CHECK(DuckDuckGoEngine::parse(page(search_html),query)["empty"]==true);
  CHECK(DuckDuckGoEngine::parse(page("<p class='no-results'>No results found</p>"),query)["empty"]==true);
  rejects([&]{DuckDuckGoEngine::parse(page("<p>Unexpected markup</p>"),query);});
  rejects([&]{DuckDuckGoEngine::parse(page("<form id='challenge-form'>Challenge</form>",202),query);});
  rejects([&]{DuckDuckGoEngine::parse(page("",429),query);});
  auto lite=DuckDuckGoEngine::parse(page("<table><tr><td><a class='result-link' href='https://docs.example.com/a'>A</a></td></tr><tr><td class='result-snippet'>Snippet A</td></tr><tr><td><a class='result-link' href='https://docs.example.com/b'>B</a></td></tr><tr><td class='result-snippet'>Snippet B</td></tr></table>"),SearchRequest{"rules","",5,{},{}});
  CHECK(lite["results"][0]["snippet"]=="Snippet A");CHECK(lite["results"][1]["snippet"]=="Snippet B");
  CHECK(web_encode("\"東京\" site:example.com +x -y").find("%22%E6%9D%B1%E4%BA%AC%22")!=std::string::npos);
  auto doc=extract_web_document(page("<html><head><title>Docs</title></head><nav>unwanted</nav><main><h1>Rules</h1><p>Read <b>bold</b> and <code>code</code>.</p><pre>if (x) {\n  y();\n}</pre><table><tr><th>A</th><td>B</td></tr></table><script>evil</script><p hidden>secret</p><a href='/guide'>Guide</a></main></html>"));
  CHECK(doc.title=="Rules");CHECK(doc.text.find("Read bold and `code`.")!=std::string::npos);CHECK(doc.text.find("\n  y();\n")!=std::string::npos);CHECK(doc.text.find("A | B |")!=std::string::npos);CHECK(doc.text.find("unwanted")==std::string::npos);CHECK(doc.text.find("evil")==std::string::npos);CHECK(doc.text.find("secret")==std::string::npos);CHECK(doc.text.find("https://html.duckduckgo.com/guide")!=std::string::npos);
  CHECK(extract_web_document(page("<aside>noise</aside><div role='main'><p>Actual documentation.</p></div>")).text.find("Actual documentation.")!=std::string::npos);
  rejects([]{extract_web_document(page("<main>Enable JavaScript to continue</main>"));});
  rejects([]{extract_web_document(page("<script>window._cf_chl_opt={}</script><p>Challenge</p>"));});
  auto plain=page(std::string(300000,'x'));plain.content_type="text/plain";rejects([&]{extract_web_document(plain);});
  plain.content_type="application/pdf";rejects([&]{extract_web_document(plain);});plain.body=std::string(3*1024*1024,'x');rejects([&]{extract_web_document(plain);});
}
void boundaries() {
  for(auto& tool:Tools::definitions())CHECK(tool["function"]["parameters"]["required"].is_array());
  for(auto* ip:{"127.0.0.1","10.1.2.3","169.254.169.254","100.100.100.200","172.16.0.1","192.168.1.1","198.18.0.1","224.0.0.1","::1","fe80::1","fc00::1","::ffff:127.0.0.1","2001:db8::1","2002:7f00:1::"})CHECK(!public_web_address(ip));
  CHECK(public_web_address("8.8.8.8"));CHECK(public_web_address("2606:4700:4700::1111"));
  for(auto* url:{"file:///etc/passwd","http://localhost/","http://127.1/","http://2130706433/","http://[::1]/","https://user:password@example.com/","http://169.254.169.254/latest","http://x.local/","https://example.com/\nfoo"})rejects([&]{web_url(url);});
  CHECK(web_url("HTTPS://DOCS.Example.COM:443/page#anchor")=="https://docs.example.com/page");
  rejects([]{web_url("http://localhost./");});
  rejects([]{fetch_public_web("https://example.com/","",[]{throw TurnCancelled();});});
  CHECK(web_url("/doc#section","https://example.com/base")=="https://example.com/doc");
  rejects([]{fetch_public_web("http://127.0.0.1/","",{});});
  rejects([]{make_search_engine("unavailable");});
}
void adapter() {
  int calls=0;std::string observed_form;
  DuckDuckGoEngine engine([&](const std::string& url,const std::string& form,const std::function<void()>& control){if(control)control();++calls;observed_form=form;auto response=page(search_html);response.url=url;return response;});
  SearchRequest query{"\"rotation rules\" site:example.com","",5,{"example.com"},{}};
  auto first=engine.search(query,{});CHECK(observed_form=="q="+web_encode(query.query));query.cursor=first["next_cursor"];engine.search(query,{});CHECK(observed_form.find("s=30")!=std::string::npos);CHECK(calls==2);
  query.query="changed";rejects([&]{engine.search(query,{});});query.cursor="";query.query="!g rules";rejects([&]{engine.search(query,{});});query.query="\\rules";rejects([&]{engine.search(query,{});});query.query="rules";query.include_domains={"https://example.com"};rejects([&]{engine.search(query,{});});
  int fallback=0;DuckDuckGoEngine fallback_engine([&](auto& url,auto&,auto&){auto response=page(search_html,++fallback==1 ? 503:200);response.url=url;return response;});query.include_domains.clear();fallback_engine.search(query,{});CHECK(fallback==2);
  DuckDuckGoEngine challenge_engine([&](auto&,auto&,auto&){++fallback;return page("challenge",202);});rejects([&]{challenge_engine.search(query,{});});CHECK(fallback==3);
}
void storage() {
  auto root=fs::temp_directory_path()/("saga-web-tests-"+uuid());
  struct Cleanup{fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{root};
  Paths paths{root/"config",root/"data",root/"state",root/"run"};paths.create();private_dir(root/"project");Registry registry(paths);auto meta=registry.create("Synthetic research persona","");PersonaContext persona(paths,meta,root/"project");
  Config config;config.endpoint="http://example.invalid";config.model="synthetic";config.context_length=65536;config.save(paths);CHECK(Config::load(paths).search_engine=="duckduckgo");CHECK(Config::load(paths).web_read_limit==8);auto invalid_config=config;invalid_config.search_engine="unavailable";rejects([&]{invalid_config.validate();});
  auto session=persona.db->exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()});
  int network=0;WebResearch web(*persona.db,config,[&](const std::string& url,const std::string&,const std::function<void()>& service){++network;if(service)service();auto response=page(url.find("duckduckgo")!=std::string::npos ? search_html : "<main><h1>Modern rules</h1><p>Rotation is clockwise.</p></main>");response.url=url;return response;});
  web.begin_turn(session,0);auto search=web.dispatch("web_search",{{"query","rules"}},session,0);auto snippet=search["results"][0]["source_id"];
  auto q=web.question("How does rotation work?",true,session,0);rejects([&]{web.dispatch("research_resolve",{{"id",q},{"status","supported"},{"assessment",{{"proposition","How does rotation work?"},{"coverage","full"},{"rationale","The selected passage documents the rotation direction."}}},{"conclusion","Clockwise"},{"sources",Json::array({{{"source_id",snippet},{"quote","Version 2 rotation rules."},{"relation","supports"}}})}},session,0);});
  auto doc=web.dispatch("web_read",{{"source_id",snippet}},session,0);auto id=doc["source_id"];CHECK(doc["kind"]=="document");CHECK(doc["content"].get<std::string>().find("Rotation is clockwise.")!=std::string::npos);
  web.dispatch("research_resolve",{{"id",q},{"status","supported"},{"assessment",{{"proposition","How does rotation work?"},{"coverage","full"},{"rationale","The selected passage documents the rotation direction."}}},{"conclusion","Clockwise"},{"sources",Json::array({{{"source_id",id},{"quote","Rotation is clockwise."},{"relation","supports"}}})}},session,0);CHECK(web.unresolved_required(session,0).empty());
  rejects([&]{web.dispatch("research_resolve",{{"id",q},{"status","supported"},{"assessment",{{"proposition","How does rotation work?"},{"coverage","full"},{"rationale","Fixture citation verification."}}},{"conclusion","Fabrication"},{"sources",Json::array({{{"source_id",id},{"quote","invented quote"},{"relation","supports"}}})}},session,0);});
  rejects([&]{persona.db->exec("UPDATE web_sources SET text='tampered' WHERE id=?",{id});});
  web.settings(false);auto cached=web.dispatch("web_read",{{"source_id",id}},session,0);CHECK(cached["source_id"]==id);CHECK(network==2);rejects([&]{web.dispatch("web_search",{{"query","rules"}},session,0);});CHECK(network==2);
  web.settings(true);web.service([&]{web.settings(false);});rejects([&]{web.dispatch("web_read",{{"url","https://example.com/new"}},session,0);});CHECK(network==2);
  PersonaContext other(paths,registry.create("Other synthetic persona",""),root/"project");CHECK(other.db->query("SELECT id FROM web_sources").empty());
  persona.db->migrate();CHECK(persona.db->query("SELECT version FROM schema_version")[0]["version"]==13);
}
void evidence_contract() {
  auto root=fs::temp_directory_path()/("saga-passages-"+uuid());
  struct Cleanup{fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{root};
  Paths paths{root/"config",root/"data",root/"state",root/"run"};paths.create();private_dir(root/"project");Registry registry(paths);
  PersonaContext p(paths,registry.create("Passage fixture",""),root/"project");Config config;
  auto session=p.db->exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()});
  std::string html="<main><h1>Modern rules</h1><p>A T-spin examines three occupied corners after rotation.</p><h2>Scoring</h2><table><tr><th>Rule</th><th>Result</th></tr>";
  for(int i=0;i<16;++i)html+="<tr><td>row "+std::to_string(i)+"</td><td>"+(i==4 || i==13 ? "T-spin [Impossible.]" : "Unrelated scoring")+"</td></tr>";
  html+="</table><pre>    int score = 1;\n    return score;</pre></main>";
  int fetches=0;WebResearch web(*p.db,config,[&](auto &url,auto&,auto&){++fetches;auto r=page(html);r.url=url;return r;});web.begin_turn(session,0);
  auto claim=web.question("A T-spin examines only two occupied corners",true,session,0);
  auto doc=web.dispatch("web_read",{{"url","https://rules.example.com/twist"},{"query","T-spin"},{"question_id",claim}},session,0);
  CHECK(doc["query"]=="T-spin" && doc["focused"]==true && doc["next_offset"].is_null());CHECK(!doc["passages"].empty());
  Id passage=0,table=0;
  for(auto &entry:doc["passages"]) {
    auto block=web.dispatch("web_read",{{"passage_id",entry["passage_id"]}},session,0);
    CHECK(doc["content"].get<std::string>().find(block["content"].get<std::string>())!=std::string::npos);
    auto content=block["content"].get<std::string>();
    if(content.find("three occupied corners")!=std::string::npos)passage=entry["passage_id"];
    if(content.find("[Impossible.]")!=std::string::npos) {table=entry["passage_id"];CHECK(content.find("Unrelated scoring")==std::string::npos);CHECK(block["selection_partial"]==true);}
  }
  CHECK(p.db->query("SELECT text FROM web_sources WHERE id=?",{doc["source_id"]})[0]["text"].get<std::string>().find("    int score = 1;\n    return score;")!=std::string::npos);
  auto selection=Json::parse(p.db->query("SELECT selection_json FROM web_source_passages WHERE id=?",{table})[0]["selection_json"].get<std::string>());CHECK(selection["original_row_indices"]==Json::array({0,5,14}));
  CHECK(passage>0 && table>0 && fetches==1);CHECK(doc["reduced"]==true && doc["complete"]==false);
  auto cached=web.dispatch("web_read",{{"source_id",doc["source_id"]},{"query","T-spin"}},session,0);CHECK(cached["content"]==doc["content"] && cached["passages"]==doc["passages"]);
  rejects([&]{p.db->exec("UPDATE web_source_passages SET text='changed' WHERE id=?",{passage});});
  rejects([&]{p.db->exec("DELETE FROM web_source_passages WHERE id=?",{passage});});
  auto assessment=[&](std::string proposition,std::string coverage){return Json{{"proposition",proposition},{"coverage",coverage},{"rationale","The passage documents three occupied corners; evaluate the original claim."}};};
  Json resolution={{"id",claim},{"status","supported"},{"conclusion","Three corners are inspected"},{"assessment",assessment("A T-spin examines three occupied corners","full")},{"sources",Json::array({{{"passage_id",passage},{"relation","supports"}}})}};
  try {web.dispatch("research_resolve",resolution,session,0);CHECK(false);}catch(const ResearchEvidenceError &e){CHECK(e.recovery["proposition"]=="A T-spin examines only two occupied corners");CHECK(e.recovery.contains("recovery_action"));}
  resolution["assessment"]=assessment("A T-spin examines only two occupied corners","partial");rejects([&]{web.dispatch("research_resolve",resolution,session,0);});
  resolution["status"]="unverified";auto partial=web.dispatch("research_resolve",resolution,session,0);CHECK(partial["assessment"]["coverage"]=="partial" && !web.unresolved_required(session,0).empty());
  resolution["status"]="supported";resolution["assessment"]["coverage"]="full";resolution["sources"][0]["relation"]="contradicts";rejects([&]{web.dispatch("research_resolve",resolution,session,0);});
  resolution["sources"].push_back({{"passage_id",passage},{"relation","supports"}});rejects([&]{web.dispatch("research_resolve",resolution,session,0);});resolution["sources"].erase(1);
  resolution["status"]="contradicted";auto contradicted=web.dispatch("research_resolve",resolution,session,0);CHECK(contradicted["sources"][0]["citation_validated"]==true);CHECK(contradicted["assessment"]["semantic_validation"]=="agent_assessed");
  auto replacement=web.dispatch("research_revise",{{"id",claim},{"question","A T-spin examines three occupied corners"},{"reason","The original corner count is wrong"}},session,0);
  CHECK(replacement["required"]==1);auto original=p.db->query("SELECT * FROM research_questions WHERE id=?",{claim})[0];CHECK(original["question"]=="A T-spin examines only two occupied corners" && original["status"]=="contradicted");CHECK(original["superseded_by"]==replacement["claim_id"]);
  CHECK(web.unresolved_required(session,0).size()==1);rejects([&]{web.dispatch("research_resolve",resolution,session,0);});
  resolution["id"]=replacement["claim_id"];resolution["status"]="supported";resolution["assessment"]=assessment(replacement["proposition"],"full");resolution["sources"][0]["relation"]="supports";
  auto supported=web.dispatch("research_resolve",resolution,session,0);CHECK(supported["sources"][0]["content_hash"].is_string());CHECK(web.unresolved_required(session,0).empty());
  resolution["sources"]=Json::array({{{"source_id",doc["source_id"]},{"quote","T-spin Impossible."},{"relation","supports"}}});
  try {web.dispatch("research_resolve",resolution,session,0);CHECK(false);}catch(const ResearchEvidenceError &e){CHECK(e.recovery["recovery_action"]["tool"]=="web_read");CHECK(e.recovery["citation_index"]==0);}
  auto table_claim=web.question("The scoring table includes [Impossible.] entries",false,session,0);resolution["id"]=table_claim;resolution["assessment"]=assessment("The scoring table includes [Impossible.] entries","full");resolution["conclusion"]="The selected table rows contain [Impossible.] entries.";resolution["sources"]=Json::array({{{"passage_id",table},{"relation","supports"}}});CHECK(web.dispatch("research_resolve",resolution,session,0)["status"]=="supported");
  // Local header/documentation observations are distinct from compilation proof.
  auto local=web.question("Does the local ncurses header declare initscr?",true,session,0);
  auto ev=p.db->event("tool.completed",{{"tool","file_read"},{"result",{{"content","extern WINDOW *initscr(void);"},{"encoding","utf-8"},{"path","fixture/curses.h"}}}},session);
  Json local_resolution={{"id",local},{"status","supported"},{"conclusion","The captured header declares initscr."},{"assessment",assessment("Does the local ncurses header declare initscr?","full")},{"sources",Json::array({{{"event_id",ev},{"quote","WINDOW *initscr(void)"},{"relation","supports"}}})}};
  CHECK(web.dispatch("research_resolve",local_resolution,session,0)["sources"][0]["basis"]=="local_document");
  auto ack=p.db->event("tool.completed",{{"tool","remember"},{"result",{{"content","extern WINDOW *initscr(void);"},{"encoding","utf-8"}}}},session);local_resolution["sources"][0]["event_id"]=ack;rejects([&]{web.dispatch("research_resolve",local_resolution,session,0);});
  auto binary=p.db->event("tool.completed",{{"tool","file_read"},{"result",{{"content","WINDOW *initscr(void)"},{"encoding","base64"}}}},session);local_resolution["sources"][0]["event_id"]=binary;rejects([&]{web.dispatch("research_resolve",local_resolution,session,0);});
  // Migration preserves legacy conclusions and citations but requests semantic reassessment.
  p.db->sql("DROP TABLE web_source_passages; ALTER TABLE research_questions DROP COLUMN assessment_json; ALTER TABLE research_questions DROP COLUMN superseded_by; UPDATE schema_version SET version=11");p.db->migrate();
  auto migrated=p.db->query("SELECT status,conclusion,assessment_json,sources_json FROM research_questions WHERE id=?",{local})[0];CHECK(migrated["status"]=="unverified" && migrated["conclusion"]=="The captured header declares initscr.");CHECK(Json::parse(migrated["assessment_json"].get<std::string>())["coverage"]=="unreviewed");CHECK(!Json::parse(migrated["sources_json"].get<std::string>()).empty());
}

void proof_and_failure_gates() {
  auto root=fs::temp_directory_path()/("saga-web-proof-"+uuid());
  struct Cleanup{fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{root};
  Paths paths{root/"config",root/"data",root/"state",root/"run"};paths.create();private_dir(root/"project");Registry registry(paths);PersonaContext p(paths,registry.create("Evidence fixture",""),root/"project");
  Config c;c.model="fixture";c.endpoint="http://example.invalid";c.context_length=65536;
  Memory memory(p,c);int approvals=0,fetches=0;bool transferring=false;
  Tools tools(p,memory,[&](auto&,auto&){++approvals;return true;},c,[&](auto& url,auto&,auto& service){++fetches;struct Reset{bool& value;~Reset(){value=false;}} reset{transferring};transferring=true;if(service)service();auto r=page("<main><h1>Rules</h1><p>Rotation is clockwise.</p></main>");r.url=url;return r;});
  p.project=p.db->exec("INSERT INTO projects(name,root_path,created_at,last_seen_at) VALUES(?,?,?,?)",{"Fixture",p.project_root.string(),now(),now()});
  p.session=p.db->exec("INSERT INTO sessions(started_at,status) VALUES(?,'active')",{now()});
  tools.permissions("always_approve");tools.execute("task_create",{{"title","Fixture"},{"objective","Research and implement"},{"risk","low"},{"checks",Json::array({"Program works"})}});
  tools.web().begin_turn(p.session,p.task);auto q=tools.web().question("Rotation?",true,p.session,p.task);
  auto premature=tools.execute("file_write",{{"path","early.txt"},{"content","premature"},{"description","fixture"}});CHECK(premature.contains("error"));CHECK(!fs::exists(root/"project/early.txt"));
  auto result=tools.execute("web_read",{{"url","https://docs.example.com/rules"}});CHECK(!result.contains("error"));CHECK(approvals==0 && fetches==1);
  auto checks=p.db->query("SELECT id FROM task_checks WHERE task_id=?",{p.task});auto check=checks[0]["id"];
  auto bad=tools.execute("check_resolve",{{"check_id",check},{"source_event_id",result["source_event_id"]},{"passed",true},{"explanation","Downloaded docs"},{"quote","Rotation is clockwise."}});CHECK(bad.contains("error"));CHECK(p.db->query("SELECT status FROM task_checks WHERE id=?",{check})[0]["status"]=="unresolved");
  auto research_check=tools.execute("task_add_check",{{"description","Find documented rotation direction"},{"kind","research"}})["check_id"];
  auto good=tools.execute("check_resolve",{{"check_id",research_check},{"source_event_id",result["source_event_id"]},{"passed",true},{"explanation","Documented rule"},{"quote","Rotation is clockwise."}});CHECK(!good.contains("error"));
  auto resolve=tools.execute("research_resolve",{{"id",q},{"status","supported"},{"assessment",{{"proposition","Rotation?"},{"coverage","full"},{"rationale","The cited rule answers the rotation question."}}},{"conclusion","The source documents clockwise rotation."},{"sources",Json::array({{{"source_id",result["source_id"]},{"quote","Rotation is clockwise."},{"relation","supports"}}})}});CHECK(!resolve.contains("error"));
  auto fact=tools.execute("learn_fact",{{"subject","rotation"},{"predicate","direction"},{"object","clockwise"},{"source_event_id",result["source_event_id"]}});CHECK(!fact.contains("error"));double before=memory.confidence("fact",fact["id"]);
  auto repeat=tools.execute("web_read",{{"url","https://docs.example.com/rules"},{"refresh",true}});tools.execute("learn_fact",{{"subject","rotation"},{"predicate","direction"},{"object","clockwise"},{"source_event_id",repeat["source_event_id"]}});CHECK(memory.confidence("fact",fact["id"])==before);
  auto source_count=p.db->query("SELECT count(*) AS n FROM web_sources")[0]["n"];
  memory.checkpoint("test_research",0);auto handoff=memory.handoff();CHECK(handoff.contains("research"));CHECK(handoff["research"][0]["source_ids"].get<std::string>().find(std::to_string(result["source_id"].get<Id>()))!=std::string::npos);CHECK(p.db->query("SELECT count(*) AS n FROM web_sources")[0]["n"]==source_count);
  tools.web().question("Unverifiable detail",true,p.session,p.task);tools.web().settings(false);Json warnings=Json::array();
  auto write=tools.execute("file_write",{{"path","uncertain.txt"},{"content","reversible draft"},{"description","fixture"}},[&](auto& type,auto& data){if(type=="research.warning")warnings.push_back(data);});CHECK(!write.contains("error"));CHECK(warnings.size()==1);CHECK(fs::exists(root/"project/uncertain.txt"));
  CHECK(tools.execute("task_update",{{"id",p.task},{"status","completed"}}).contains("error"));
  p.db->exec("UPDATE tasks SET risk='high' WHERE id=?",{p.task});CHECK(tools.execute("file_write",{{"path","dangerous.txt"},{"content","bad"},{"description","fixture"}}).contains("error"));CHECK(!fs::exists(root/"project/dangerous.txt"));
  CHECK(tools.execute("file_read",{{"path","uncertain.txt"}})["content"]=="reversible draft");
  tools.web().settings(true);tools.web().begin_turn(p.session,p.task);
  for(int i=0;i<8;++i)CHECK(!tools.execute("web_read",{{"url","https://docs.example.com/rules"}}).contains("error"));
  auto network_before=fetches;CHECK(tools.execute("web_read",{{"url","https://docs.example.com/rules"}}).contains("error"));CHECK(fetches==network_before);
  tools.web().begin_turn(p.session,p.task);
  auto snapshots_before=p.db->query("SELECT count(*) AS n FROM web_sources")[0]["n"];
  tools.service([&]{if(transferring)tools.web().settings(false);});
  auto interrupted=tools.execute("web_read",{{"url","https://docs.example.com/rules"}});
  CHECK(interrupted.contains("error") && interrupted["error"].get<std::string>().find("disabled")!=std::string::npos);
  CHECK(p.db->query("SELECT count(*) AS n FROM web_sources")[0]["n"]==snapshots_before);
  CHECK(fetches==network_before+1);network_before=fetches;tools.web().settings(true);
  tools.web().begin_turn(p.session,p.task);tools.service([]{throw TurnCancelled();});bool cancelled=false;try{tools.execute("web_read",{{"url","https://docs.example.com/rules"}});}catch(const TurnCancelled&){cancelled=true;}CHECK(cancelled && fetches==network_before);
}
class ResearchBackend final : public MockChatBackend {
  int step_=0;
  Json doc_,search_;Id question_=0,task_=0,check_=0;
public:
  ModelInfo discover() override{return {"research-fixture",65536,true,true,false};}
  CapabilityReport probe() override{return {true,true,true,true};}
  void chat(const ChatRequest& request,StreamCallback callback) override {
    Json state=Json::object(),last=Json::object();
    for(auto& m:request.messages) {
      auto content=m.value("content",Json());if(!content.is_string())continue;auto text=content.get<std::string>();
      std::string marker="Runtime attention update (data):\n";
      if(m["role"]=="user" && text.starts_with(marker))state=Json::parse(text.substr(marker.size()));
      if(m["role"]=="tool")last=Json::parse(text);
    }
    if(!state.empty()) {task_=state["active_task"][0]["id"];check_=state["task_checks"][0]["id"];if(!state["research_questions"].empty())question_=state["research_questions"][0]["id"];}
    auto invoke=[&](std::string name,Json args){if(name=="research_resolve")args["assessment"]={{"proposition","How does rotation work?"},{"coverage","full"},{"rationale","The cited fixture rule states the documented rotation direction."}};callback({{"choices",Json::array({{{"index",0},{"delta",{{"tool_calls",Json::array({{{"index",0},{"id","fixture-"+std::to_string(step_)},{"function",{{"name",name},{"arguments",args.dump()}}}}})}}},{"finish_reason","tool_calls"}}})}});};
    switch(step_++) {
      case 0:invoke("research_plan",{{"intent","Implement a widget"},{"operator_constraints",{"C++"}},{"desired_actions",{"Write and compile"}},{"goals",Json::array({{{"question","Modern rotation direction"},{"required",true},{"claims",{"How does rotation work?"}}}})}});break;
      case 1:invoke("web_search",{{"query","site:docs.example.com \"rotation\" rules"},{"include_domains",{"docs.example.com"}}});break;
      case 2:search_=last;CHECK(!search_.contains("error"));invoke("web_read",{{"source_id",search_["results"][0]["source_id"]}});break;
      case 3:doc_=last;CHECK(!doc_.contains("error"));invoke("research_resolve",{{"id",question_},{"status","supported"},{"conclusion","The documented fixture rule is clockwise rotation."},{"sources",Json::array({{{"source_id",doc_["source_id"]},{"quote","Rotation is clockwise."},{"relation","supports"}}})}});break;
      case 4:CHECK(!last.contains("error"));invoke("file_write",{{"path","widget.cpp"},{"content","int main() { return 0; }\n"},{"description","Synthetic research implementation"}});break;
      case 5:CHECK(!last.contains("error"));invoke("shell_exec",{{"command","c++ widget.cpp -o widget && ./widget"},{"execution","host"}});break;
      case 6:CHECK(!last.contains("error") && last["exit_code"]==0);invoke("check_resolve",{{"check_id",check_},{"source_event_id",last["source_event_id"]},{"passed",true},{"explanation","Compiled and executed the fixture program successfully"}});break;
      case 7:CHECK(!last.contains("error"));invoke("task_update",{{"id",task_},{"status","completed"}});break;
      default:CHECK(!last.contains("error"));callback({{"choices",Json::array({{{"index",0},{"delta",{{"content","Implemented and executed the fixture. [Rules](https://docs.example.com/rules)"}}},{"finish_reason","stop"}}})}});break;
    }
  }
};
void runtime_research() {
  auto root=fs::temp_directory_path()/("saga-research-runtime-"+uuid());struct Cleanup{fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{root};
  Paths paths{root/"config",root/"data",root/"state",root/"run"};paths.create();private_dir(root/"project");Registry registry(paths);Config c;c.endpoint="http://example.invalid";c.model="fixture";c.context_length=65536;
  int approvals=0,network=0;Json events=Json::array();
  Runtime runtime(std::make_unique<PersonaContext>(paths,registry.create("Research runtime fixture",""),root/"project"),c,std::make_unique<ResearchBackend>(),[&](auto&,auto&){++approvals;return true;},[&](auto& url,auto&,auto& service){++network;if(service)service();auto r=page(url.find("duckduckgo")!=std::string::npos ? search_html : "<main>Rotation is clockwise.</main>");r.url=url;return r;});
  runtime.start();runtime.command("permissions",{{"mode","always_approve"}});runtime.chat("Create a widget; search online for its rules.",[&](auto& type,auto& data){events.push_back({{"type",type},{"payload",data}});});
  CHECK(network==2 && approvals==0);CHECK(fs::exists(root/"project/widget.cpp"));CHECK(fs::exists(root/"project/widget"));
  CHECK(runtime.persona().db->query("SELECT status FROM tasks ORDER BY id DESC LIMIT 1")[0]["status"]=="completed");CHECK(runtime.persona().db->query("SELECT status FROM research_questions")[0]["status"]=="supported");
  auto completed=std::find_if(events.begin(),events.end(),[](const Json& e){return e["type"]=="assistant.completed";});CHECK(completed!=events.end());CHECK((*completed)["payload"]["content"].get<std::string>().starts_with("Implemented"));CHECK(runtime.command("status")["web"]["searches"]==1);
  CHECK(live_command_allowed("web",{{"enabled",false}}));runtime.command("web",{{"enabled",false}});CHECK(runtime.command("status")["web"]["enabled"]==false);
}

class NoResearchBackend final : public MockChatBackend {
public:
  ModelInfo discover() override{return {"fixture",65536,true,true,false};}
  CapabilityReport probe() override{return {true,true,true,true};}
  void chat(const ChatRequest&,StreamCallback callback) override{callback({{"choices",Json::array({{{"index",0},{"delta",{{"content","I researched this and it is verified."}}},{"finish_reason","stop"}}})}});}
};
void absent_research_gate() {
  auto root=fs::temp_directory_path()/("saga-research-missing-"+uuid());struct Cleanup{fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{root};
  Paths paths{root/"config",root/"data",root/"state",root/"run"};paths.create();private_dir(root/"project");Registry registry(paths);Config c;c.endpoint="http://example.invalid";c.model="fixture";c.context_length=65536;
  Runtime runtime(std::make_unique<PersonaContext>(paths,registry.create("Missing research fixture",""),root/"project"),c,std::make_unique<NoResearchBackend>(),[](auto&,auto&){return false;});runtime.start();Json events=Json::array();
  runtime.chat("Search online for the rotation specification.",[&](auto& type,auto& data){events.push_back({{"type",type},{"payload",data}});});
  CHECK(std::none_of(events.begin(),events.end(),[](const Json& e){return e["type"]=="assistant.delta";}));
  auto completed=std::find_if(events.begin(),events.end(),[](const Json& e){return e["type"]=="assistant.completed";});CHECK(completed!=events.end());CHECK((*completed)["payload"]["content"].get<std::string>().find("did not perform")!=std::string::npos);
}

}
int main(){try{parsing();boundaries();adapter();storage();evidence_contract();proof_and_failure_gates();runtime_research();absent_research_gate();std::cout<<"Web adapter, extraction, boundaries and provenance checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
