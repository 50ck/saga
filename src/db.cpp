#include <saga/db.hpp>
#include <sqlite3.h>
#include <tuple>
#include <schema.hpp>
#include <sys/stat.h>

namespace saga {
Database::Database(const fs::path& path) {
  if (fs::is_symlink(path)) throw std::runtime_error("Refusing symlink database");
  if (sqlite3_open_v2(path.c_str(), &handle_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
    std::string err = handle_ ? sqlite3_errmsg(handle_) : "Cannot open database";
    if (handle_) sqlite3_close_v2(handle_);
    handle_ = nullptr; throw std::runtime_error(err);
  }
  chmod(path.c_str(),0600);
  sqlite3_busy_timeout(handle_,5000);
  sql("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;");
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
Id Database::exec(std::string_view statement, const std::vector<Json>& params) { query(statement,params); return sqlite3_last_insert_rowid(handle_); }
int Database::changes() const { return sqlite3_changes(handle_); }
void Database::transaction(const std::function<void()>& operation) {
  sql("BEGIN IMMEDIATE");
  try { operation(); sql("COMMIT"); } catch (...) { sql("ROLLBACK"); throw; }
}
void Database::migrate() {
  sql("CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL); INSERT INTO schema_version SELECT 0 WHERE NOT EXISTS(SELECT 1 FROM schema_version);");
  auto version = query("SELECT version FROM schema_version")[0]["version"].get<int>();
  if (version > 10) throw std::runtime_error("Database schema is newer than this Saga binary");
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

}
Id Database::event(std::string_view type, const Json& payload, Id session, Id task) {
  return exec("INSERT INTO events(ts,session_id,task_id,type,payload_json) VALUES(?,?,?,?,?)",
    {now(),session ? Json(session) : Json(),task ? Json(task) : Json(),type,payload.dump()});
}
}
