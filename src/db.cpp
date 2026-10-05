#include <saga/db.hpp>
#include <sqlite3.h>
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
  if (version > 3) throw std::runtime_error("Database schema is newer than this Saga binary");
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
}
Id Database::event(std::string_view type, const Json& payload, Id session, Id task) {
  return exec("INSERT INTO events(ts,session_id,task_id,type,payload_json) VALUES(?,?,?,?,?)",
    {now(),session ? Json(session) : Json(),task ? Json(task) : Json(),type,payload.dump()});
}
}
