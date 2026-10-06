#pragma once
#include <saga/db.hpp>
namespace saga {
// Registry is a control-plane object. It is never passed to cognition or tools.
class Registry {
  Paths paths_;
  Database db_;
public:
  explicit Registry(const Paths& paths);
  Json list();
  Json create(std::string name = "Assistant", std::string soul = std::string(default_soul));
  Json select(const std::string& id);
  void rename(const std::string& id,const std::string& name);
  Json resolve(const std::string& selector);
  // Caller detaches an owned runtime only after validation and exclusive locking.
  void erase(const std::string& id,int owned_lock=-1,
             const std::function<void()>& detach={},const std::vector<fs::path>& debug_directories={});
};
struct PersonaContext {
  std::string id, name;
  fs::path directory, project_root;
  std::string soul;
  std::unique_ptr<Database> db;
  std::vector<fs::path> private_roots;
  Json host_environment = Json::object();
  Id session = 0, project = 0, task = 0, generation = 0;
  std::string turn,tool_call;
  int lock_fd = -1;
  PersonaContext(const Paths& paths, const Json& metadata, const fs::path& cwd);
  ~PersonaContext();
  PersonaContext(const PersonaContext&) = delete;
  void edit_soul(const std::string& content);
};
}
