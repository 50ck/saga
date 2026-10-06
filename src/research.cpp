#include <saga/web.hpp>
#include <saga/web/acquisition.hpp>
#include <set>
namespace saga {
Id WebResearch::question(const std::string &text, bool required, Id session, Id task,
                         Id assumption) {
  if (trim(text).empty() || text.size() > 512 || text.find_first_of("\r\n")!=std::string::npos)
    throw std::runtime_error("A research claim must be one concrete external question of at most 512 bytes");
  auto user=db_.query("SELECT json_extract(payload_json,'$.content') AS content FROM events WHERE session_id=? AND type='user.message' ORDER BY id DESC LIMIT 1",{session});
  if(!user.empty() && trim(user[0]["content"].get<std::string>())==trim(text))
    throw std::runtime_error("User instructions are not research claims; decompose the external knowledge requirements first");
  auto rows = db_.query("SELECT id FROM research_questions WHERE session_id=? AND "
                        "coalesce(task_id,0)=? AND question=? AND status='pending'",
                        {session, task, text});
  if (!rows.empty()) {
    if (required) {
      db_.exec("UPDATE research_questions SET required=1 WHERE id=?", {rows[0]["id"]});
      db_.exec("UPDATE research_goals SET required=1 WHERE id=(SELECT goal_id FROM research_questions WHERE id=?)",{rows[0]["id"]});
    }
    return rows[0]["id"];
  }
  Id id=0;
  db_.transaction([&]{
  id = db_.exec("INSERT INTO "
                     "research_questions(session_id,task_id,assumption_id,question,required,"
                     "created_at,updated_at) VALUES(?,?,?,?,?,?,?)",
                     {session, task ? Json(task) : Json(), assumption ? Json(assumption) : Json(),
                      text, required, now(), now()});
  db_.event("research.question_created", {{"id", id}, {"question", text}, {"required", required}},
            session, task);
  auto goal=db_.exec("INSERT INTO research_goals(session_id,task_id,question,required,created_at) VALUES(?,?,?,?,?)",{session,task ? Json(task):Json(),text,required,now()});
  db_.exec("UPDATE research_questions SET goal_id=? WHERE id=?",{goal,id});
  });
  return id;
}
Json research_context(Database &db, Id session, Id task) {
  return db.query("SELECT id,goal_id,task_id,assumption_id,substr(question,1,512) AS "
                  "question,required,status,substr(conclusion,1,1024) AS conclusion,(SELECT "
                  "json_group_array(json_extract(value,'$.source_id')) FROM "
                  "json_each(sources_json)) AS source_ids,disclosed FROM research_questions WHERE "
                  "session_id=? OR (task_id IS NOT NULL AND task_id=?) ORDER BY id DESC LIMIT 30",
                  {session, task});
}
Json WebResearch::questions(Id session, Id task) const {
  return research_context(db_, session, task);
}
Json WebResearch::unresolved_required(Id session, Id task) const {
  return db_.query("SELECT * FROM research_questions WHERE required=1 AND status IN "
                   "('pending','unverified','contradicted') AND ((task_id IS NULL AND "
                   "session_id=?) OR (task_id IS NOT NULL AND task_id=?)) ORDER BY id",
                   {session, task});
}
void WebResearch::disclose_failures(Id session, Id task, const Emit &emit) {
  for (auto &row : unresolved_required(session, task))
    if (row["status"] != "pending" && row["disclosed"] == 0) {
      auto content =
          "Research incomplete: " + utf8_excerpt(row["question"].get<std::string>(),180) + "\n" +
          utf8_excerpt(row["conclusion"].get<std::string>(),240) +
          "\nReversible work may continue with uncertainty; this is not verified completion.";
      db_.event("research.uncertainty_disclosed",
                {{"question_id", row["id"]}, {"content", content}}, session, task);
      db_.exec("INSERT INTO messages(session_id,ts,role,content_json,token_count) "
               "VALUES(?,?,'assistant',?,?)",
               {session, now(), Json{{"role", "assistant"}, {"content", content}}.dump(),
                estimate_tokens(content)});
      db_.exec("UPDATE research_questions SET disclosed=1 WHERE id=?", {row["id"]});
      if (emit)
        emit("research.warning", {{"content", content}});
    }
}
void WebResearch::account_for_pending(Id session, Id task, const Emit &emit) {
  // Attempt failures and exhausted search budgets do not evaluate claims.
  // Only disabled access is a deterministic inability to acquire new sources.
  if(!settings()["enabled"].get<bool>()) {
    for(auto &claim:unresolved_required(session,task)) if(claim["status"]=="pending")
      db_.exec("UPDATE research_questions SET status='unverified',conclusion='Public web access is disabled.',updated_at=? WHERE id=?",{now(),claim["id"]});
    if(plan_pending(session,task)) {
      db_.exec("UPDATE research_plans SET status='unverified' WHERE status='pending' AND (session_id=? OR task_id=?)",{session,task});
      if(emit)emit("research.warning",{{"content","Public web access is disabled; external requirements remain unverified."}});
    }
  }
  disclose_failures(session,task,emit);
}


void WebResearch::require_plan(Id user_event,const std::string &turn,Id session,Id task) {
  db_.exec("INSERT OR IGNORE INTO research_plans(session_id,task_id,turn_id,user_source_event_id,created_at) VALUES(?,?,?,?,?)",{session,task ? Json(task):Json(),turn,user_event,now()});
  db_.event("research.decomposition_requested",{{"user_source_event_id",user_event},{"turn_id",turn}},session,task);
}
bool WebResearch::plan_pending(Id session,Id task) const {
  return !db_.query("SELECT id FROM research_plans WHERE status='pending' AND ((task_id IS NULL AND session_id=?) OR (task_id IS NOT NULL AND task_id=?)) LIMIT 1",{session,task}).empty();
}
Json research_plan_context(Database &db,Id session,Id task) {
  Json result=Json::array();
  for(auto &row:db.query("SELECT id,status,decomposition_json FROM research_plans WHERE session_id=? OR task_id=? ORDER BY id DESC LIMIT 8",{session,task})) {
    row["decomposition"]=Json::parse(row["decomposition_json"].get<std::string>());row.erase("decomposition_json");
    row["goals"]=db.query("SELECT id,question,required FROM research_goals WHERE plan_id=?",{row["id"]});
    for(auto &goal:row["goals"]) {
      goal["claims"]=db.query("SELECT id,question AS proposition,status,required FROM research_questions WHERE goal_id=?",{goal["id"]});
      bool supported=!goal["claims"].empty(),contradicted=false,pending=false,partial=false;
      for(auto &claim:goal["claims"]) {supported=supported && claim["status"]=="supported";contradicted=contradicted || claim["status"]=="contradicted";pending=pending || claim["status"]=="pending";partial=partial || claim["status"]=="supported";}
      goal["status"]=supported ? "established" : contradicted ? "contradicted" : partial ? "partially_established" : pending ? "unknown" : "not_established";
    }
    result.push_back(std::move(row));
  }
  return result;
}
Json WebResearch::plan(const Json &args,Id session,Id task) {
  auto intent=args.at("intent").get<std::string>();auto goals=args.at("goals");
  if(trim(intent).empty() || intent.size()>512 || !goals.is_array() || goals.empty() || goals.size()>8)
    throw std::runtime_error("Research decomposition requires a concise intent and 1–8 external goals");
  for(auto *name:{"operator_constraints","desired_actions"}) {
    auto values=args.at(name);
    if(!values.is_array() || values.size()>16)throw std::runtime_error("Research decomposition allows at most sixteen constraints/actions");
    for(auto &value:values)if(!value.is_string() || value.get_ref<const std::string &>().size()>512)throw std::runtime_error("Research constraints/actions must be concise strings");
  }
  size_t claims=0;std::set<std::string> unique;
  for(auto &goal:goals) {
    auto question=goal.at("question").get<std::string>();
    if(trim(question).empty() || question.size()>512 || question.find_first_of("\r\n")!=std::string::npos || !goal.at("claims").is_array() || goal["claims"].empty())throw std::runtime_error("Each research goal needs a focused question and concrete claims");
    for(auto &claim:goal["claims"]) {
      if(!claim.is_string())throw std::runtime_error("Research claims must be strings");
      auto text=claim.get<std::string>();
      if(trim(text).empty() || text.size()>512 || text.find_first_of("\r\n")!=std::string::npos || !unique.insert(lower(trim(text))).second || ++claims>32)throw std::runtime_error("Use at most 32 distinct, one-line external claims");
      if(lower(trim(text))==lower(trim(intent)))throw std::runtime_error("Task intent is not an external research claim");
      for(auto *field:{"operator_constraints","desired_actions"})for(auto &value:args[field])
        if(lower(trim(text))==lower(trim(value.get<std::string>())))throw std::runtime_error("Operator constraints and actions are not external research claims");
      auto user=db_.query("SELECT json_extract(payload_json,'$.content') AS content FROM events WHERE session_id=? AND type='user.message' ORDER BY id DESC LIMIT 1",{session});
      if(!user.empty() && trim(user[0]["content"].get<std::string>())==trim(text))throw std::runtime_error("An entire operator request cannot be a research claim");
    }
  }
  if(!turn_.empty()) {
    auto previous=db_.query("SELECT id,decomposition_json FROM research_plans WHERE turn_id=? AND status='ready'",{turn_});
    if(!previous.empty()) {
      if(previous[0]["decomposition_json"]!=args.dump())throw std::runtime_error("This turn already has a research plan; add newly discovered gaps with research_question");
      return {{"plan_id",previous[0]["id"]},{"plans",research_plan_context(db_,session,task)},{"reused",true}};
    }
  }
  auto pending=db_.query("SELECT id FROM research_plans WHERE status='pending' AND ((task_id IS NULL AND session_id=?) OR (task_id IS NOT NULL AND task_id=?)) ORDER BY id DESC LIMIT 1",{session,task});
  Id plan_id=0;
  db_.transaction([&]{
    plan_id=pending.empty() ? db_.exec("INSERT INTO research_plans(session_id,task_id,turn_id,created_at) VALUES(?,?,?,?)",{session,task ? Json(task):Json(),turn_.empty() ? Json():Json(turn_),now()}) : pending[0]["id"].get<Id>();
    bool any_required=false;for(auto &goal:goals)any_required=any_required || goal.value("required",false);
    if(!pending.empty() && !any_required)throw std::runtime_error("Explicit research needs at least one required external goal");
    for(auto &goal:goals) {
      auto id=db_.exec("INSERT INTO research_goals(plan_id,session_id,task_id,question,required,created_at) VALUES(?,?,?,?,?,?)",{plan_id,session,task ? Json(task):Json(),goal["question"],goal.value("required",false),now()});
      for(auto &claim:goal["claims"])db_.exec("INSERT INTO research_questions(session_id,task_id,goal_id,question,required,created_at,updated_at) VALUES(?,?,?,?,?,?,?)",{session,task ? Json(task):Json(),id,claim,goal.value("required",false),now(),now()});
    }
    db_.exec("UPDATE research_plans SET status='ready',decomposition_json=? WHERE id=?",{args.dump(),plan_id});
    db_.event("research.decomposed",{{"plan_id",plan_id},{"goals",goals.size()},{"claims",claims}},session,task);
  });
  return {{"plan_id",plan_id},{"plans",research_plan_context(db_,session,task)}};
}
Json WebResearch::dispatch(const std::string &name,const Json &args,Id session,Id task,const Emit &emit) {
  Id claim=args.value("question_id",Id(0));
  Id goal=0;
  bool web=name=="web_search" || name=="web_read" || name=="web_fetch";
  if(web && !claim) {
    auto pending=db_.query("SELECT id FROM research_questions WHERE status='pending' AND (session_id=? OR task_id=?) ORDER BY id",{session,task});
    if(pending.size()==1)claim=pending[0]["id"];
  }
  if(web && claim && db_.query("SELECT id FROM research_questions WHERE id=? AND (session_id=? OR task_id=?)",{claim,session,task}).empty())throw std::runtime_error("Unknown research claim for this search/source operation");
  if(web && claim) {auto row=db_.query("SELECT goal_id FROM research_questions WHERE id=?",{claim});if(!row[0]["goal_id"].is_null())goal=row[0]["goal_id"];}
  Emit tagged=[&](const std::string &type,const Json &data){if(emit){auto payload=data;if(claim)payload["claim_id"]=claim;if(goal)payload["research_goal_id"]=goal;emit(type,payload);}};
  auto record=[&](bool succeeded,Id source){
    if(!web || !claim)return;
    auto event=db_.event("research.attempt",{{"claim_id",claim},{"research_goal_id",goal},{"operation",name},{"succeeded",succeeded},{"source_id",source}},session,task);
    db_.exec("INSERT INTO research_attempts(claim_id,operation,source_id,succeeded,source_event_id,created_at) VALUES(?,?,?,?,?,?)",{claim,name,source ? Json(source):Json(),succeeded,event,now()});
  };
  try {
    auto result=dispatch_operation(name,args,session,task,tagged);
    record(true,result.value("source_id",Id(0)));return result;
  }catch(const TurnCancelled &){throw;}
   catch(...) {record(false,0);throw;}
}
void WebResearch::rebind_task(Id previous,Id next,const std::string &turn) {
  if(turn.empty() || previous==next)return;
  db_.transaction([&]{
    db_.exec("UPDATE research_plans SET task_id=? WHERE turn_id=?",{next,turn});
    db_.exec("UPDATE research_goals SET task_id=? WHERE plan_id IN (SELECT id FROM research_plans WHERE turn_id=?)",{next,turn});
    db_.exec("UPDATE research_questions SET task_id=? WHERE goal_id IN (SELECT id FROM research_goals WHERE plan_id IN (SELECT id FROM research_plans WHERE turn_id=?)) OR (task_id=? AND session_id=(SELECT session_id FROM turns WHERE id=?) AND created_at>=(SELECT started_at FROM turns WHERE id=?))",{next,turn,previous,turn,turn});
    db_.exec("UPDATE research_goals SET task_id=? WHERE id IN (SELECT goal_id FROM research_questions WHERE task_id=?)",{next,next});
  });
}

}
