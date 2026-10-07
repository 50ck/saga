#include <saga/persona.hpp>
#include <saga/debug.hpp>
#include <set>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace saga {
namespace {
void finish_persona_recording(const std::string& id) noexcept {
  try {
    if(auto log=debug_logger();log && log->context().value("persona_id","")==id)
      log->session("saga",0);
  } catch(...) {} // Diagnostic failure must not prevent releasing a session.
}
}
Registry::Registry(const Paths& paths) : paths_(paths), db_(paths.data / "registry.db",false) {
  db_.sql("PRAGMA secure_delete=ON; CREATE TABLE IF NOT EXISTS personas(uuid TEXT PRIMARY KEY,display_name TEXT NOT NULL,created_at INTEGER NOT NULL,last_used_at INTEGER,erase_pending INTEGER NOT NULL DEFAULT 0);");
  auto columns=db_.query("PRAGMA table_info(personas)");
  if(std::none_of(columns.begin(),columns.end(),[](const Json& c){return c["name"]=="erase_pending";}))
    db_.sql("ALTER TABLE personas ADD COLUMN erase_pending INTEGER NOT NULL DEFAULT 0");
}
Json Registry::list() { return db_.query("SELECT uuid,display_name,created_at,last_used_at FROM personas WHERE erase_pending=0 ORDER BY created_at,uuid"); }
Json Registry::create(std::string name, std::string soul) {
  name = trim(name);
  if (name.empty()) name = "Assistant";
  if (name.size() > 512 || soul.size() > 32768 || name.find('\0') != std::string::npos)
    throw std::runtime_error("Persona name or SOUL is too large or invalid");
  if (trim(soul).empty()) soul = default_soul;
  auto id = uuid(); auto dir = paths_.persona(id);
  private_dir(dir); private_dir(dir / "cache"); private_dir(dir / "artifacts");
  try {
    if(auto log=debug_logger())log->session(name,0,{{"persona_id",id},{"persona_directory",dir.string()}});
    atomic_write(dir / "SOUL.md",soul);
    {Database agent(dir / "agent.db"); agent.migrate();
     agent.event("persona.created",{{"name",name},{"soul",soul}});}
    finish_persona_recording(id); // Publish only after its creation log is closed.
    db_.exec("INSERT INTO personas(uuid,display_name,created_at) VALUES(?,?,?)",{id,name,now()});
  } catch (...) { finish_persona_recording(id);fs::remove_all(dir); throw; }
  return {{"uuid",id},{"display_name",name}};
}
Json Registry::select(const std::string& id) {
  paths_.persona(id);
  auto rows = db_.query("SELECT uuid,display_name FROM personas WHERE uuid=? AND erase_pending=0",{id});
  if (rows.empty()) throw std::runtime_error("Unknown persona UUID");
  db_.exec("UPDATE personas SET last_used_at=? WHERE uuid=?",{now(),id});
  return rows[0];
}
void Registry::rename(const std::string& id,const std::string& name) {
  paths_.persona(id);
  if (trim(name).empty() || name.size() > 512 || name.find('\0') != std::string::npos) throw std::runtime_error("Invalid persona name");
  db_.exec("UPDATE personas SET display_name=? WHERE uuid=? AND erase_pending=0",{trim(name),id});
  if (!db_.changes()) throw std::runtime_error("Unknown persona UUID");
}
Json Registry::resolve(const std::string& selector) {
  auto value=trim(selector);
  if(value.empty())throw std::runtime_error("Use /erase PERSONA_NAME_OR_UUID");
  auto rows=db_.query("SELECT uuid,display_name FROM personas WHERE uuid=?",{value});
  if(rows.empty())rows=db_.query("SELECT uuid,display_name FROM personas WHERE display_name=?",{value});
  if(rows.empty())throw std::runtime_error("Unknown persona name or UUID");
  if(rows.size()!=1)throw std::runtime_error("Ambiguous persona name; use its UUID with /erase");
  return rows[0];
}
void Registry::erase(const std::string& id,int owned_lock,const std::function<void()>& detach,
                     const std::vector<fs::path>& debug_directories) {
  auto dir=paths_.persona(id);
  auto metadata=db_.query("SELECT uuid,erase_pending FROM personas WHERE uuid=?",{id});
  if(metadata.empty())throw std::runtime_error("Unknown persona UUID");
  struct Lock {int fd=-1;~Lock(){if(fd>=0)::close(fd);}} lock;
  if(fs::exists(dir)) {
    if(fs::is_symlink(dir/"cache"))throw std::runtime_error("Refusing aliased persona cache");
    if(metadata[0]["erase_pending"]==1 && !fs::exists(dir/"cache"))private_dir(dir/"cache");
    if(owned_lock>=0) {
      struct stat owned{},target{};
      if(fstat(owned_lock,&owned) || lstat((dir/"cache/runtime.lock").c_str(),&target) ||
         owned.st_dev!=target.st_dev || owned.st_ino!=target.st_ino || !S_ISREG(target.st_mode))
        throw std::runtime_error("Runtime lock does not belong to this persona");
    }
    lock.fd=owned_lock>=0 ? fcntl(owned_lock,F_DUPFD_CLOEXEC,0) :
      ::open((dir/"cache/runtime.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(lock.fd<0 || flock(lock.fd,LOCK_EX|LOCK_NB))throw std::runtime_error("Persona is in use; close its other session before erasing it");
  }
  // Validate all owned logs before detaching or removing anything. Names alone
  // cannot establish ownership: a rename or identical display names are legal.
  std::set<fs::path> bundles;
  std::vector<std::pair<fs::path,fs::path>> follow_links;
  auto catalog=dir/"diagnostic-bundles.json";
  if(fs::is_symlink(catalog))throw std::runtime_error("Refusing aliased diagnostic ownership catalog");
  if(fs::exists(catalog)) {
    auto entries=Json::parse(read_file(catalog,16*1024*1024));
    if(!entries.is_array())throw std::runtime_error("Invalid diagnostic ownership catalog");
    for(const auto& entry:entries) {
      if(!entry.is_string())throw std::runtime_error("Invalid diagnostic bundle path");
      fs::path path=entry.get<std::string>();
      if(!path.is_absolute())throw std::runtime_error("Diagnostic bundle path must be absolute");
      if(fs::exists(path))bundles.insert(path);
    }
  }
  // Include pre-catalog recordings in the default and currently configured
  // log locations; never remove a bundle based on the display-name prefix.
  auto directories=debug_directories;directories.push_back(paths_.state/"logs");
  for(const auto& directory:directories) {
    if(!fs::exists(directory))continue;
    for(const auto& entry:fs::directory_iterator(directory)) {
      auto manifest=entry.path()/"manifest.json";
      if(entry.is_symlink() || !entry.is_directory() || fs::is_symlink(manifest) || !fs::is_regular_file(manifest))continue;
      auto data=Json::parse(read_file(manifest,16*1024*1024),nullptr,false);
      if(data.is_object() && data.contains("context") && data["context"].is_object() && data["context"].value("persona_id","")==id)
        bundles.insert(fs::canonical(entry.path()));
    }
  }
  for(const auto& bundle:bundles) {
    if(fs::is_symlink(bundle) || fs::canonical(bundle)!=bundle || within(dir,bundle))throw std::runtime_error("Unsafe diagnostic bundle path");
    for(const auto& persona:db_.query("SELECT uuid FROM personas"))if(persona["uuid"]!=id && within(bundle,paths_.persona(persona["uuid"].get<std::string>())))
      throw std::runtime_error("Diagnostic bundle overlaps another persona");
    auto manifest=bundle/"manifest.json";
    if(fs::is_symlink(manifest))throw std::runtime_error("Refusing aliased diagnostic manifest");
    auto data=Json::parse(read_file(manifest,16*1024*1024));
    if(!data.is_object() || data.value("format_version",0)!=1 || !data.contains("context") ||
       !data["context"].is_object() || data["context"].value("persona_id","")!=id ||
       !data.contains("log_files") || !data["log_files"].is_array())
      throw std::runtime_error("Diagnostic bundle ownership could not be verified");
    auto context=data["context"];auto prefix=debug_filename(context.value("persona_name",""),context.value("session_id",Id(0)),0);
    prefix.resize(prefix.size()-5); // Strip "0.log", leaving the identity/session prefix.
    auto name=bundle.filename().string();
    if(!name.starts_with(prefix) || name.size()==prefix.size() ||
       !std::all_of(name.begin()+static_cast<std::ptrdiff_t>(prefix.size()),name.end(),[](unsigned char c){return std::isdigit(c) || c=='-';}))
      throw std::runtime_error("Refusing non-Saga diagnostic directory");
    if(data.contains("follow_path") && data["follow_path"].is_string()) {
      fs::path follow=data["follow_path"].get<std::string>();
      if(follow.is_absolute())follow_links.emplace_back(follow,bundle);
    }
  }
  // Persist the deletion intent first. A crash or I/O failure must not revive
  // a partly deleted identity through selection or detached maintenance.
  db_.exec("UPDATE personas SET erase_pending=1 WHERE uuid=?",{id});
  if(detach)detach(); // The duplicated flock survives closing the runtime/DB.
  // A crashed recorder may leave its alias behind. Never unlink an alias
  // that has since moved to another session, or follow it into its target.
  for(const auto& [follow,bundle]:follow_links)if(fs::is_symlink(follow)) {
    auto target=fs::read_symlink(follow);
    if(target.is_relative())target=follow.parent_path()/target;
    if(within(target,bundle))fs::remove(follow);
  }
  for(const auto& bundle:bundles)fs::remove_all(bundle);
  fs::remove_all(dir); // Includes SQLite WAL/SHM, SOUL, caches and artifacts.
  db_.exec("DELETE FROM personas WHERE uuid=?",{id});
  db_.sql("PRAGMA wal_checkpoint(TRUNCATE)");
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
    // Metadata selected before /erase may be stale. Revalidate under the lock
    // before opening a database or creating any persona-scoped recordings.
    Registry(paths).select(id);
    project_root = fs::canonical(cwd);
    if (!fs::is_directory(project_root)) throw std::runtime_error("Project directory does not exist");
    // A workspace must not include Saga's identity, registry, config, or socket.
    for (const auto& protected_root : {paths.data,paths.config,paths.state,paths.runtime})
      if (within(project_root,protected_root) || within(protected_root,project_root))
        throw std::runtime_error("Choose a project directory outside Saga's storage and its ancestors");
    for (const auto& protected_root : {paths.data,paths.config,paths.state,paths.runtime})
      for (const auto* system_root : {"/usr","/bin","/sbin","/lib","/lib64"})
        if (within(protected_root,system_root)) throw std::runtime_error("Saga storage must be outside system program/library directories to preserve tool isolation");
    if(auto log=debug_logger())log->session(name,0,{{"persona_id",id},{"persona_directory",directory.string()}});
    soul = read_file(directory / "SOUL.md",32768);
    db = std::make_unique<Database>(directory / "agent.db"); db->migrate();
    // Never silently lose sessions after a process crash.
    db->transaction([&]{
      for (auto& row : db->query("SELECT id FROM sessions WHERE status='active'")) {
        Id sid = row["id"];
        db->exec("UPDATE sessions SET ended_at=?,end_reason='crash_recovery',status='needs_consolidation' WHERE id=?",{now(),sid});
        db->event("session.closed",{{"reason","crash_recovery"}},sid);
      }
      for(auto& turn:db->query("SELECT id FROM turns WHERE status='active'")) {
        db->event("turn.interrupted",{{"turn_id",turn["id"]},{"reason","crash_recovery"},{"tool_replay","forbidden"}});
        db->exec("UPDATE turns SET status='interrupted',ended_at=? WHERE id=?",{now(),turn["id"]});
      }
      for(auto& generation:db->query("SELECT id,turn_id FROM generations WHERE status IN ('created','streaming')")) {
        db->event("generation.interrupted",{{"generation_id",generation["id"]},{"turn_id",generation["turn_id"]},{"reason","crash_recovery"}});
        db->exec("UPDATE generations SET status='interrupted',state_json=json_set(state_json,'$.status','interrupted','$.interruption','crash_recovery'),ended_at=? WHERE id=?",{now(),generation["id"]});
      }
      for(auto& tool:db->query("SELECT id,turn_id,tool_call_id FROM tool_runs WHERE status='running'"))db->event("tool.execution.unknown_after_crash",tool);
      db->exec("UPDATE tool_runs SET status='failed',error_type='crash_recovery' WHERE status='running'");
      db->exec("UPDATE model_calls SET status='failed',error='crash_recovery' WHERE status='running'");
      for(auto& row:db->query("SELECT id FROM steering_messages WHERE status='queued'"))db->event("steering.cancelled",{{"id",row["id"]},{"reason","crash_recovery"}});
      db->exec("UPDATE steering_messages SET status='cancelled',updated_at=? WHERE status='queued'",{now()});
    });
  } catch (...) { finish_persona_recording(id);close(lock_fd); lock_fd = -1; throw; }
}
PersonaContext::~PersonaContext() {
  db.reset();
  // A detached logger must never recreate deleted sidecars after another client
  // acquires this identity's lock. Join/rebind its writer before unlocking.
  finish_persona_recording(id);
  if (lock_fd >= 0) close(lock_fd);
}
void PersonaContext::edit_soul(const std::string& text) {
  if (trim(text).empty() || text.size() > 32768) throw std::runtime_error("SOUL must contain 1–32768 bytes");
  auto previous = soul;
  db->event("soul.edit_requested",{{"content",text},{"before",previous}},session);
  atomic_write(directory / "SOUL.md",text); soul = text;
  db->event("soul.user_edited",{{"hash",digest(text)},{"before",previous},{"after",text}},session);
}
}
