#include <saga/persona.hpp>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace saga {
Registry::Registry(const Paths& paths) : paths_(paths), db_(paths.data / "registry.db") {
  db_.sql("CREATE TABLE IF NOT EXISTS personas(uuid TEXT PRIMARY KEY,display_name TEXT NOT NULL,created_at INTEGER NOT NULL,last_used_at INTEGER);");
}
Json Registry::list() { return db_.query("SELECT uuid,display_name,created_at,last_used_at FROM personas ORDER BY created_at,uuid"); }
Json Registry::create(std::string name, std::string soul) {
  name = trim(name);
  if (name.empty()) name = "Assistant";
  if (name.size() > 512 || soul.size() > 32768 || name.find('\0') != std::string::npos)
    throw std::runtime_error("Persona name or SOUL is too large or invalid");
  if (trim(soul).empty()) soul = default_soul;
  auto id = uuid(); auto dir = paths_.persona(id);
  private_dir(dir); private_dir(dir / "cache"); private_dir(dir / "artifacts");
  try {
    atomic_write(dir / "SOUL.md",soul);
    Database agent(dir / "agent.db"); agent.migrate();
    agent.event("persona.created",{{"name",name},{"soul",soul}});
    db_.exec("INSERT INTO personas(uuid,display_name,created_at) VALUES(?,?,?)",{id,name,now()});
  } catch (...) { fs::remove_all(dir); throw; }
  return {{"uuid",id},{"display_name",name}};
}
Json Registry::select(const std::string& id) {
  paths_.persona(id);
  auto rows = db_.query("SELECT uuid,display_name FROM personas WHERE uuid=?",{id});
  if (rows.empty()) throw std::runtime_error("Unknown persona UUID");
  db_.exec("UPDATE personas SET last_used_at=? WHERE uuid=?",{now(),id});
  return rows[0];
}
void Registry::rename(const std::string& id,const std::string& name) {
  paths_.persona(id);
  if (trim(name).empty() || name.size() > 512 || name.find('\0') != std::string::npos) throw std::runtime_error("Invalid persona name");
  db_.exec("UPDATE personas SET display_name=? WHERE uuid=?",{trim(name),id});
  if (!db_.changes()) throw std::runtime_error("Unknown persona UUID");
}
PersonaContext::PersonaContext(const Paths& paths, const Json& meta, const fs::path& cwd)
  : id(meta.at("uuid")), name(meta.at("display_name")), directory(paths.persona(id)) {
  private_roots = {paths.config,paths.data,paths.state,paths.runtime};
  if (!fs::is_directory(directory)) throw std::runtime_error("Persona directory is missing");
  if (fs::is_symlink(directory / "SOUL.md") || fs::is_symlink(directory / "cache") || fs::is_symlink(directory / "artifacts"))
    throw std::runtime_error("Persona files and areas must not alias other directories");
  lock_fd = open((directory / "cache/runtime.lock").c_str(),O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,0600);
  if (lock_fd < 0 || flock(lock_fd,LOCK_EX | LOCK_NB)) {
    if (lock_fd >= 0) close(lock_fd);
    lock_fd = -1; throw std::runtime_error("Persona already has an attached session");
  }
  try {
    project_root = fs::canonical(cwd);
    if (!fs::is_directory(project_root)) throw std::runtime_error("Project directory does not exist");
    // A workspace must not include Saga's identity, registry, config, or socket.
    for (const auto& protected_root : {paths.data,paths.config,paths.state,paths.runtime})
      if (within(project_root,protected_root) || within(protected_root,project_root))
        throw std::runtime_error("Choose a project directory outside Saga's storage and its ancestors");
    for (const auto& protected_root : {paths.data,paths.config,paths.state,paths.runtime})
      for (const auto* system_root : {"/usr","/bin","/sbin","/lib","/lib64"})
        if (within(protected_root,system_root)) throw std::runtime_error("Saga storage must be outside system program/library directories to preserve tool isolation");
    soul = read_file(directory / "SOUL.md",32768);
    db = std::make_unique<Database>(directory / "agent.db"); db->migrate();
    // Never silently lose sessions after a process crash.
    db->transaction([&]{
      for (auto& row : db->query("SELECT id FROM sessions WHERE status='active'")) {
        Id sid = row["id"];
        db->exec("UPDATE sessions SET ended_at=?,end_reason='crash_recovery',status='needs_consolidation' WHERE id=?",{now(),sid});
        db->event("session.closed",{{"reason","crash_recovery"}},sid);
      }
      db->exec("UPDATE tool_runs SET status='failed',error_type='crash_recovery' WHERE status='running'");
      db->exec("UPDATE model_calls SET status='failed',error='crash_recovery' WHERE status='running'");
      for(auto& row:db->query("SELECT id FROM steering_messages WHERE status='queued'"))db->event("steering.cancelled",{{"id",row["id"]},{"reason","crash_recovery"}});
      db->exec("UPDATE steering_messages SET status='cancelled',updated_at=? WHERE status='queued'",{now()});
    });
  } catch (...) { close(lock_fd); lock_fd = -1; throw; }
}
PersonaContext::~PersonaContext() { db.reset(); if (lock_fd >= 0) close(lock_fd); }
void PersonaContext::edit_soul(const std::string& text) {
  if (trim(text).empty() || text.size() > 32768) throw std::runtime_error("SOUL must contain 1–32768 bytes");
  auto previous = soul;
  db->event("soul.edit_requested",{{"content",text},{"before",previous}},session);
  atomic_write(directory / "SOUL.md",text); soul = text;
  db->event("soul.user_edited",{{"hash",digest(text)},{"before",previous},{"after",text}},session);
}
}
