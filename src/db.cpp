#include <saga/db.hpp>
#include <saga/debug.hpp>
#include <sqlite3.h>
#include <tuple>
#include <cstring>
#include <schema.hpp>
#include <sys/stat.h>

namespace saga {
Database::Database(const fs::path& path):path_(path) {
  if (fs::is_symlink(path)) throw std::runtime_error("Refusing symlink database");
  if (sqlite3_open_v2(path.c_str(), &handle_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
    std::string err = handle_ ? sqlite3_errmsg(handle_) : "Cannot open database";
    if (handle_) sqlite3_close_v2(handle_);
    handle_ = nullptr; throw std::runtime_error(err);
  }
  chmod(path.c_str(),0600);
  sqlite3_busy_timeout(handle_,5000);
  sql("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;");
  trace("database","database.opened",{{"path",path_.string()},{"backend","sqlite"},{"journal_mode","WAL"},{"synchronous","FULL"}},DebugProfile::Debug);
}
Database::~Database() { if (handle_) sqlite3_close_v2(handle_); }
void Database::sql(std::string_view text) {
  char* error = nullptr;
  if (sqlite3_exec(handle_, std::string(text).c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    std::string msg = error ? error : sqlite3_errmsg(handle_);
    sqlite3_free(error); throw std::runtime_error(msg);
  }
}
Json Database::query(std::string_view sql_text, const std::vector<Json>& params) {
  TraceSpan span("database","query",{{"database",path_.string()}});
  if(auto log=debug_logger();log && log->enabled(DebugProfile::Forensic))trace("database","database.statement",{{"database",path_.string()},{"statement",sql_text},{"parameters",params}},DebugProfile::Forensic);
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(handle_,sql_text.data(),static_cast<int>(sql_text.size()),&raw,nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(handle_));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw,sqlite3_finalize);
  for (size_t i = 0; i < params.size(); ++i) {
    auto& v = params[i]; int rc;
    if (v.is_null()) rc = sqlite3_bind_null(raw, static_cast<int>(i+1));
    else if (v.is_boolean()) rc = sqlite3_bind_int64(raw,static_cast<int>(i+1),v.get<bool>());
    else if (v.is_number_integer()) rc = sqlite3_bind_int64(raw,static_cast<int>(i+1),v.get<Id>());
    else if (v.is_number()) rc = sqlite3_bind_double(raw,static_cast<int>(i+1),v.get<double>());
    else {
      auto str = v.is_string() ? v.get<std::string>() : v.dump();
      rc = sqlite3_bind_text(raw,static_cast<int>(i+1),str.data(),static_cast<int>(str.size()),SQLITE_TRANSIENT);
    }
    if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(handle_));
  }
  Json rows = Json::array(); int rc;
  while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
    Json row = Json::object();
    for (int i = 0; i < sqlite3_column_count(raw); ++i) {
      auto name = sqlite3_column_name(raw,i);
      switch (sqlite3_column_type(raw,i)) {
        case SQLITE_NULL: row[name] = nullptr; break;
        case SQLITE_INTEGER: row[name] = sqlite3_column_int64(raw,i); break;
        case SQLITE_FLOAT: row[name] = sqlite3_column_double(raw,i); break;
        default: row[name] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(raw,i)),static_cast<size_t>(sqlite3_column_bytes(raw,i)));
      }
    }
    rows.push_back(std::move(row));
  }
  if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(handle_));
  return rows;
}
void Database::diagnostic(std::string type,const Json& fields) {
  if(!debug_logger())return;
  if(recording_transaction_)pending_diagnostics_.emplace_back(std::move(type),fields);
  else debug_logger()->observe(type,fields);
}
Id Database::exec(std::string_view statement, const std::vector<Json>& params) {
  if(!debug_logger()){query(statement,params);return sqlite3_last_insert_rowid(handle_);}
  auto text=lower(std::string(statement));std::string operation;
  if(text.starts_with("insert"))operation="INSERT";else if(text.starts_with("update"))operation="UPDATE";else if(text.starts_with("delete"))operation="DELETE";
  std::string table;
  for(auto candidate:{"facts","episodes","praxis","beliefs","intentions","goals","memory_artifacts","entity_mentions","artifacts","journal_entries","self_beliefs","developed_traits","relationship_beliefs","commitments","open_loops"}) {
    auto at=text.find(candidate);if(at!=std::string::npos && (at==0 || text[at-1]==' ') && (at+std::strlen(candidate)==text.size() || text[at+std::strlen(candidate)]==' ' || text[at+std::strlen(candidate)]=='(')){table=candidate;break;}
  }
  auto logger=debug_logger();auto op=logger && !table.empty() && !operation.empty()?logger->next_span():std::string();
  if(operation=="INSERT" && (text.find("on conflict")!=std::string::npos || text.find("or ignore")!=std::string::npos))operation="INSERT_OR_UPDATE_OR_IGNORE";
  Json fields={{"memory_op_id",op},{"database",path_.string()},{"backend","sqlite"},{"table",table},{"operation",operation}};
  if(!op.empty()){trace("memory","memory.write.started",fields);trace("memory","memory.write_candidate",{{"memory_op_id",op},{"candidate",params},{"statement",statement}},DebugProfile::Forensic);}
  try{query(statement,params);}catch(...){if(!op.empty())trace("memory","memory.write.failed",fields,DebugProfile::Debug,"ERROR");throw;}
  auto row=sqlite3_last_insert_rowid(handle_);
  if(!op.empty()) {fields["last_insert_row_id"]=row;fields["rows_affected"]=changes();if(operation=="INSERT")fields["row_id"]=row;else if(text.find("where id=?")!=std::string::npos && !params.empty() && params.back().is_number_integer())fields["row_id"]=params.back();diagnostic("memory.write.committed",fields);}
  return row;
}
int Database::changes() const { return sqlite3_changes(handle_); }
void Database::transaction(const std::function<void()>& operation) {
  TraceSpan span("database","transaction",{{"database",path_.string()}});
  sql("BEGIN IMMEDIATE");recording_transaction_=true;pending_diagnostics_.clear();
  trace("database","database.transaction.started",{{"database",path_.string()}},DebugProfile::Trace);
  try {
    operation();sql("COMMIT");recording_transaction_=false;
    for(auto& [type,data]:pending_diagnostics_)if(debug_logger())debug_logger()->observe(type,data);
    pending_diagnostics_.clear();trace("database","database.transaction.committed",{{"database",path_.string()}},DebugProfile::Trace);
  } catch (...) {
    sql("ROLLBACK");recording_transaction_=false;
    for(auto& [type,data]:pending_diagnostics_)if(type=="memory.write.committed")trace("memory","memory.write.rolled_back",data,DebugProfile::Debug,"WARN");
    pending_diagnostics_.clear();trace("database","database.transaction.rollback",{{"database",path_.string()}},DebugProfile::Trace,"WARN");throw;
  }
}
void Database::migrate() {
  TraceSpan migration("database","migration");
  sql("CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL); INSERT INTO schema_version SELECT 0 WHERE NOT EXISTS(SELECT 1 FROM schema_version);");
  auto version = query("SELECT version FROM schema_version")[0]["version"].get<int>();
  trace("database","database.schema",{{"database",path_.string()},{"schema_version",version},{"target_version",12}},DebugProfile::Debug);
  if (version > 12) throw std::runtime_error("Database schema is newer than this Saga binary");
  if(version<12)trace("database","database.migration_started",{{"database",path_.string()},{"from_version",version},{"target_version",12}});
  if (version < 1) transaction([&]{ sql(saga_schema); sql("UPDATE schema_version SET version=1"); });
  if (version < 2) transaction([&]{
    bool scoped_facts = false;
    for (const auto& column : query("PRAGMA table_info(facts)")) if (column["name"] == "project_id") scoped_facts = true;
    if (!scoped_facts) sql("ALTER TABLE facts ADD COLUMN project_id INTEGER REFERENCES projects(id)");
    sql("UPDATE schema_version SET version=2");
  });
  if (version < 3) transaction([&]{
    sql("CREATE TABLE IF NOT EXISTS runtime_settings(key TEXT PRIMARY KEY,value TEXT NOT NULL,updated_at INTEGER NOT NULL); INSERT OR IGNORE INTO runtime_settings VALUES('host_permissions','ask_always',0);");
    sql("CREATE TABLE IF NOT EXISTS context_checkpoints(id INTEGER PRIMARY KEY,session_id INTEGER NOT NULL REFERENCES sessions(id),through_message_id INTEGER NOT NULL,reason TEXT NOT NULL,state_json TEXT NOT NULL CHECK(json_valid(state_json)),created_at INTEGER NOT NULL);");
    sql("CREATE INDEX IF NOT EXISTS checkpoints_session ON context_checkpoints(session_id,id);");
    sql("UPDATE schema_version SET version=3");
  });
  if (version < 4) transaction([&]{
    sql("CREATE TABLE IF NOT EXISTS steering_messages(id INTEGER PRIMARY KEY,session_id INTEGER NOT NULL REFERENCES sessions(id),source_event_id INTEGER NOT NULL REFERENCES events(id),content TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'queued' CHECK(status IN ('queued','delivered','cancelled')),created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);");
    sql("UPDATE schema_version SET version=4");
  });
  if (version < 5) transaction([&]{
    bool typed_checks=false;
    for(const auto& column:query("PRAGMA table_info(task_checks)"))if(column["name"]=="kind")typed_checks=true;
    if(!typed_checks)sql("ALTER TABLE task_checks ADD COLUMN kind TEXT NOT NULL DEFAULT 'execution' CHECK(kind IN ('execution','research'))");
    sql(R"SQL(
      CREATE TABLE IF NOT EXISTS web_sources(id INTEGER PRIMARY KEY,session_id INTEGER NOT NULL REFERENCES sessions(id),task_id INTEGER REFERENCES tasks(id),kind TEXT NOT NULL CHECK(kind IN ('search','document')),engine TEXT NOT NULL,url TEXT NOT NULL,title TEXT NOT NULL,query TEXT NOT NULL DEFAULT '',retrieved_at INTEGER NOT NULL,content_hash TEXT NOT NULL,text TEXT NOT NULL,truncated INTEGER NOT NULL DEFAULT 0,source_event_id INTEGER NOT NULL REFERENCES events(id));
      CREATE INDEX IF NOT EXISTS web_sources_url ON web_sources(url,id);
      CREATE TABLE IF NOT EXISTS research_questions(id INTEGER PRIMARY KEY,session_id INTEGER NOT NULL REFERENCES sessions(id),task_id INTEGER REFERENCES tasks(id),assumption_id INTEGER REFERENCES assumptions(id),question TEXT NOT NULL,required INTEGER NOT NULL DEFAULT 0,status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','supported','contradicted','unverified')),conclusion TEXT NOT NULL DEFAULT '',sources_json TEXT NOT NULL DEFAULT '[]' CHECK(json_valid(sources_json)),disclosed INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);
      INSERT OR IGNORE INTO runtime_settings VALUES('web_enabled','true',0);
      CREATE TRIGGER IF NOT EXISTS web_sources_no_update BEFORE UPDATE ON web_sources BEGIN SELECT RAISE(ABORT,'Web source snapshots are immutable'); END;
      CREATE TRIGGER IF NOT EXISTS web_sources_no_delete BEFORE DELETE ON web_sources BEGIN SELECT RAISE(ABORT,'Web source snapshots are immutable'); END;
      UPDATE schema_version SET version=5;
    )SQL");
  });
  if(version<6)transaction([&]{sql(R"SQL(
    CREATE TABLE IF NOT EXISTS web_source_documents(source_id INTEGER PRIMARY KEY REFERENCES web_sources(id),document_json TEXT NOT NULL,diagnostics_json TEXT NOT NULL);
    CREATE TABLE IF NOT EXISTS web_http_cache(url_hash TEXT PRIMARY KEY,url TEXT NOT NULL,final_url TEXT NOT NULL,status INTEGER NOT NULL,content_type TEXT NOT NULL,headers_json TEXT NOT NULL,body TEXT NOT NULL,updated_at INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS web_instances(origin TEXT NOT NULL,mount_path TEXT NOT NULL,descriptor_json TEXT NOT NULL,expires_at INTEGER NOT NULL,PRIMARY KEY(origin,mount_path));
    CREATE TABLE IF NOT EXISTS web_templates(origin TEXT NOT NULL,text_hash TEXT NOT NULL,structure_hash TEXT NOT NULL,simhash TEXT NOT NULL,distinct_pages INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(origin,text_hash,structure_hash));
    CREATE TABLE IF NOT EXISTS web_template_pages(origin TEXT NOT NULL,text_hash TEXT NOT NULL,structure_hash TEXT NOT NULL,page_hash TEXT NOT NULL,PRIMARY KEY(origin,text_hash,structure_hash,page_hash));
    CREATE TABLE IF NOT EXISTS web_document_cache(url_hash TEXT PRIMARY KEY,body_hash TEXT NOT NULL,document_json TEXT NOT NULL,diagnostics_json TEXT NOT NULL,updated_at INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS web_document_fingerprints(content_hash TEXT PRIMARY KEY,simhash TEXT NOT NULL,url_hash TEXT NOT NULL,last_seen_at INTEGER NOT NULL);
    UPDATE schema_version SET version=6;
  )SQL");});
  if(version<7)transaction([&]{sql(R"SQL(
    CREATE TABLE IF NOT EXISTS web_search_cursors(token TEXT PRIMARY KEY,cursor_json TEXT NOT NULL,created_at INTEGER NOT NULL);
    CREATE TRIGGER IF NOT EXISTS web_source_documents_no_update BEFORE UPDATE ON web_source_documents BEGIN SELECT RAISE(ABORT,'Canonical source snapshots are immutable'); END;
    CREATE TRIGGER IF NOT EXISTS web_source_documents_no_delete BEFORE DELETE ON web_source_documents BEGIN SELECT RAISE(ABORT,'Canonical source snapshots are immutable'); END;
    UPDATE schema_version SET version=7;
  )SQL");});
  if(version<8)transaction([&]{sql(R"SQL(
    CREATE TABLE IF NOT EXISTS turns(id TEXT PRIMARY KEY,session_id INTEGER NOT NULL REFERENCES sessions(id),status TEXT NOT NULL,phase TEXT NOT NULL,started_at INTEGER NOT NULL,ended_at INTEGER,input_tokens INTEGER NOT NULL DEFAULT 0,output_tokens INTEGER NOT NULL DEFAULT 0,sequence_number INTEGER NOT NULL DEFAULT 0);
    CREATE TABLE IF NOT EXISTS generations(id INTEGER PRIMARY KEY REFERENCES model_calls(id),turn_id TEXT REFERENCES turns(id),session_id INTEGER REFERENCES sessions(id),purpose TEXT NOT NULL,status TEXT NOT NULL,started_at INTEGER NOT NULL,ended_at INTEGER,requested_max_output_tokens INTEGER NOT NULL,effective_max_output_tokens INTEGER NOT NULL,input_estimate INTEGER NOT NULL,state_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(state_json)));
    CREATE INDEX IF NOT EXISTS generations_turn ON generations(turn_id,id);
  )SQL");
    auto columns=query("PRAGMA table_info(tool_runs)");
    for(auto [name,type]:{std::pair{"turn_id","TEXT REFERENCES turns(id)"},{"generation_id","INTEGER REFERENCES generations(id)"},{"tool_call_id","TEXT"}}) {
      bool exists=false;for(auto& column:columns)if(column["name"]==name)exists=true;
      if(!exists)sql(std::string("ALTER TABLE tool_runs ADD COLUMN ")+name+" "+type);
    }
    sql("UPDATE schema_version SET version=8");
  });

  if(version<9)transaction([&]{
    for(auto [table,name,type]:{std::tuple{"turns","user_message_id","TEXT"},std::tuple{"turns","user_content_hash","TEXT"},std::tuple{"turns","user_source_event_id","INTEGER REFERENCES events(id)"},std::tuple{"tool_runs","result_json","TEXT CHECK(result_json IS NULL OR json_valid(result_json))"}}) {
      auto columns=query(std::string("PRAGMA table_info(")+table+")");bool exists=false;
      for(auto& column:columns)if(column["name"]==name)exists=true;
      if(!exists)sql(std::string("ALTER TABLE ")+table+" ADD COLUMN "+name+" "+type);
    }
    sql("CREATE UNIQUE INDEX IF NOT EXISTS turns_user_message ON turns(session_id,user_message_id) WHERE user_message_id IS NOT NULL; CREATE TABLE IF NOT EXISTS tool_dispatch_keys(turn_id TEXT NOT NULL REFERENCES turns(id),tool_call_id TEXT NOT NULL,run_id INTEGER NOT NULL REFERENCES tool_runs(id),PRIMARY KEY(turn_id,tool_call_id)); UPDATE schema_version SET version=9;");
  });
  if(version<10)transaction([&]{sql(R"SQL(
    CREATE TABLE IF NOT EXISTS search_result_cache(cache_key TEXT PRIMARY KEY,response_json TEXT NOT NULL CHECK(json_valid(response_json)),expires_at INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS search_instance_cache(origin TEXT PRIMARY KEY,state_json TEXT NOT NULL CHECK(json_valid(state_json)),updated_at INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS search_directory_cache(directory TEXT PRIMARY KEY,origins_json TEXT NOT NULL CHECK(json_valid(origins_json)),updated_at INTEGER NOT NULL);
    UPDATE schema_version SET version=10;
  )SQL");});

  if(version<11)transaction([&]{sql(R"SQL(
    CREATE TABLE IF NOT EXISTS research_plans(id INTEGER PRIMARY KEY,session_id INTEGER NOT NULL REFERENCES sessions(id),task_id INTEGER REFERENCES tasks(id),turn_id TEXT UNIQUE REFERENCES turns(id),user_source_event_id INTEGER REFERENCES events(id),status TEXT NOT NULL DEFAULT 'pending',decomposition_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(decomposition_json)),created_at INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS research_goals(id INTEGER PRIMARY KEY,plan_id INTEGER REFERENCES research_plans(id),session_id INTEGER NOT NULL REFERENCES sessions(id),task_id INTEGER REFERENCES tasks(id),question TEXT NOT NULL,required INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS research_attempts(id INTEGER PRIMARY KEY,claim_id INTEGER NOT NULL REFERENCES research_questions(id),operation TEXT NOT NULL,source_id INTEGER REFERENCES web_sources(id),succeeded INTEGER NOT NULL,source_event_id INTEGER REFERENCES events(id),created_at INTEGER NOT NULL);
    CREATE INDEX IF NOT EXISTS research_attempts_claim ON research_attempts(claim_id,id);
  )SQL");
    bool linked=false;for(auto &column:query("PRAGMA table_info(research_questions)"))if(column["name"]=="goal_id")linked=true;
    if(!linked)sql("ALTER TABLE research_questions ADD COLUMN goal_id INTEGER REFERENCES research_goals(id)");
    // Whole-request entries from older runtimes remain audit data, not proof obligations.
    sql("UPDATE research_questions SET required=0,status='unverified',conclusion='Legacy whole-request entry retained for audit; external facts must be decomposed separately.' WHERE goal_id IS NULL AND EXISTS(SELECT 1 FROM events WHERE events.session_id=research_questions.session_id AND events.type='user.message' AND json_extract(events.payload_json,'$.content')=research_questions.question);");
    sql("UPDATE schema_version SET version=11");
  });

  if(version<12)transaction([&]{
    bool assessment=false,superseded=false;
    for(auto &column:query("PRAGMA table_info(research_questions)")) {assessment |= column["name"]=="assessment_json";superseded |= column["name"]=="superseded_by";}
    if(!assessment)sql("ALTER TABLE research_questions ADD COLUMN assessment_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(assessment_json))");
    if(!superseded)sql("ALTER TABLE research_questions ADD COLUMN superseded_by INTEGER REFERENCES research_questions(id)");
    sql(R"SQL(
    CREATE TABLE IF NOT EXISTS web_source_passages(id INTEGER PRIMARY KEY,source_id INTEGER NOT NULL REFERENCES web_sources(id),block_id INTEGER NOT NULL,text TEXT NOT NULL,content_hash TEXT NOT NULL,selection_json TEXT NOT NULL CHECK(json_valid(selection_json)),created_at INTEGER NOT NULL,UNIQUE(source_id,block_id,content_hash));
    CREATE TRIGGER IF NOT EXISTS web_passages_no_update BEFORE UPDATE ON web_source_passages BEGIN SELECT RAISE(ABORT,'Source passages are immutable'); END;
    CREATE TRIGGER IF NOT EXISTS web_passages_no_delete BEFORE DELETE ON web_source_passages BEGIN SELECT RAISE(ABORT,'Source passages are immutable'); END;

    UPDATE research_questions SET assessment_json='{"assessed_by":"legacy_agent","coverage":"unreviewed","citation_validation":"legacy_exact_quote"}' WHERE status IN ('supported','contradicted');
    UPDATE research_questions SET status='unverified',disclosed=0 WHERE json_extract(assessment_json,'$.coverage')='unreviewed';
    UPDATE schema_version SET version=12;
  )SQL");});

  if(version<12)trace("database","database.migration_completed",{{"database",path_.string()},{"from_version",version},{"schema_version",12}});
}
Id Database::event(std::string_view type, const Json& payload, Id session, Id task) {
  auto id=exec("INSERT INTO events(ts,session_id,task_id,type,payload_json) VALUES(?,?,?,?,?)",
    {now(),session ? Json(session) : Json(),task ? Json(task) : Json(),type,payload.dump()});
  if(debug_logger()){Json fields=payload;if(!fields.is_object())fields={{"payload",fields}};fields["event_id"]=id;fields["session_id"]=session;fields["task_id"]=task;diagnostic(std::string(type),fields);}
  return id;
}
}
