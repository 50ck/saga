#include "check.hpp"
#include <saga/debug.hpp>
#include <saga/persona.hpp>
#include <sys/socket.h>

#define main saga_daemon_binary_main
#include "../src/daemon.cpp"
#undef main

using namespace saga;
namespace {
Json request(Channel& channel,const std::string& type,const Json& payload=Json::object()) {
  auto id=uuid();channel.send({{"type",type},{"request_id",id},{"payload",payload}});
  for(int i=0;i<100;++i){auto frame=channel.receive(1000);if(!frame || frame->value("request_id","")!=id)continue;
    if((*frame)["type"]=="error")throw std::runtime_error((*frame)["payload"]["message"]);
    if((*frame)["type"]=="result")return (*frame)["payload"];
  }
  throw std::runtime_error("No daemon result");
}
Json erase(Channel& channel,const std::string& selector) {
  return request(channel,"command",{{"name","erase"},{"arguments",{{"persona",selector}}}});
}
void registry_tests(const Paths& paths,const fs::path& project) {
  // Migrate an existing registry without touching existing selection data.
  {Database db(paths.data/"registry.db",false);db.sql("CREATE TABLE personas(uuid TEXT PRIMARY KEY,display_name TEXT NOT NULL,created_at INTEGER NOT NULL,last_used_at INTEGER)");}
  Registry registry(paths);
  auto a=registry.create("Same name","Synthetic identity A"),b=registry.create("Same name","Synthetic identity B");
  std::string aid=a["uuid"],bid=b["uuid"];
  rejects([&]{registry.resolve("");});rejects([&]{registry.resolve("Same name");});rejects([&]{registry.resolve("../");});
  CHECK(registry.resolve(aid)["uuid"]==aid);
  auto attached=std::make_unique<PersonaContext>(paths,a,project);
  rejects([&]{registry.erase(aid);});
  rejects([&]{registry.erase(bid,attached->lock_fd);});
  CHECK(registry.list().size()==2 && fs::exists(paths.persona(bid)));
  registry.rename(aid,"Renamed identity");
  std::vector<fs::path> owned;
  auto recordings=[&](const Json& meta,const fs::path& directory){
    DebugOptions options;options.profile=DebugProfile::Forensic;options.directory=directory;
    DebugLogger logger(options);DebugScope recording(&logger);
    logger.session(meta["display_name"].get<std::string>(),5,{{"persona_id",meta["uuid"]},{"persona_directory",paths.persona(meta["uuid"].get<std::string>()).string()}});
    trace("memory","memory.fixture",{{"content","Synthetic private memory"}},DebugProfile::Forensic);logger.flush();return logger.path().parent_path();
  };
  owned.push_back(recordings(a,project.parent_path()/"custom-recordings"));
  owned.push_back(recordings(registry.resolve(aid),paths.state/"logs"));
  auto other_log=recordings(b,paths.state/"logs");
  auto dir=paths.persona(aid);
  atomic_write(dir/"artifacts/private.txt","private artifact");atomic_write(dir/"cache/snapshot.txt","cached source");
  atomic_write(project/"tetris.c","external project survives");
  auto catalog=read_file(dir/"diagnostic-bundles.json");
  atomic_write(dir/"diagnostic-bundles.json",Json::array({other_log.string()}).dump());
  bool detached=false;rejects([&]{registry.erase(aid,attached->lock_fd,[&]{detached=true;});});CHECK(!detached);
  atomic_write(dir/"diagnostic-bundles.json",Json::array({project.string()}).dump());
  rejects([&]{registry.erase(aid,attached->lock_fd);});CHECK(fs::exists(project/"tetris.c"));
  atomic_write(dir/"diagnostic-bundles.json",catalog);
  // Internal symlinks are unlinked, never followed into a project or sibling.
  fs::create_directory_symlink(project,dir/"artifacts/workspace-link");
  auto old_lock=attached->lock_fd;
  registry.erase(aid,old_lock,[&]{attached.reset();detached=true;rejects([&]{PersonaContext blocked(paths,a,project);});});
  CHECK(detached && !fs::exists(dir));for(const auto& bundle:owned)CHECK(!fs::exists(bundle));
  CHECK(fs::exists(other_log) && fs::exists(paths.persona(bid)/"agent.db") && fs::exists(project/"tetris.c"));
  CHECK(registry.list().size()==1);rejects([&]{registry.select(aid);});rejects([&]{PersonaContext stale(paths,a,project);});
  // Pre-catalog default recordings remain discoverable by UUID, not name.
  fs::remove(paths.persona(bid)/"diagnostic-bundles.json");registry.erase(bid);CHECK(!fs::exists(other_log));
  auto pending=registry.create("Interrupted deletion","");auto pid=pending["uuid"].get<std::string>();
  {Database db(paths.data/"registry.db",false);db.exec("UPDATE personas SET erase_pending=1 WHERE uuid=?",{pid});}
  fs::remove_all(paths.persona(pid)/"cache");
  CHECK(registry.list().empty());rejects([&]{registry.select(pid);});registry.erase(pid);CHECK(!fs::exists(paths.persona(pid)));
  auto missing=registry.create("Missing directory","");auto mid=missing["uuid"].get<std::string>();fs::remove_all(paths.persona(mid));registry.erase(mid);CHECK(registry.list().empty());
}
void protocol_tests(const Paths& paths,const fs::path& project) {
  Config config;config.endpoint="http://127.0.0.1:1";config.model="fixture";config.context_length=65536;config.save(paths);
  int sockets[2];CHECK(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets)==0);
  std::jthread server([&]{serve_connected(sockets[1],paths);});
  {
    Channel client(sockets[0]);DebugOptions options;options.profile=DebugProfile::Forensic;options.directory=project.parent_path()/"protocol-logs";
    request(client,"debug.configure",options.json());
    auto idle=request(client,"personas.create",{{"name","Idle creation fixture"}});
    auto idle_id=idle["uuid"].get<std::string>();auto idle_logs=Json::parse(read_file(paths.persona(idle_id)/"diagnostic-bundles.json"));
    Registry(paths).erase(idle_id); // Another client can erase a newly created, unattached profile.
    request(client,"debug.configure",options.json());
    for(const auto& path:idle_logs)CHECK(!fs::exists(path.get<std::string>()));
    auto a=request(client,"personas.create",{{"name","Active profile"},{"soul","Synthetic A"}});
    auto b=request(client,"personas.create",{{"name","Other profile"},{"soul","Synthetic B"}});
    auto aid=a["uuid"].get<std::string>(),bid=b["uuid"].get<std::string>();
    request(client,"persona.activate",{{"uuid",aid},{"cwd",project.string()}});
    request(client,"command",{{"name","fact"},{"arguments",{{"subject","test"},{"predicate","value"},{"object","private"}}}});
    rejects([&]{erase(client,"");});rejects([&]{erase(client,"Unknown");});
    {PersonaContext busy(paths,b,project);rejects([&]{erase(client,bid);});}
    CHECK(erase(client,"Other profile")["current"]==false);
    CHECK(request(client,"command",{{"name","soul"}})["content"]=="Synthetic A");
    auto catalog=Json::parse(read_file(paths.persona(aid)/"diagnostic-bundles.json"));
    auto erased=erase(client,"Active profile");CHECK(erased["erased"]==true && erased["current"]==true);
    CHECK(!fs::exists(paths.persona(aid)) && !fs::exists(paths.persona(bid)));
    for(const auto& path:catalog)CHECK(!fs::exists(path.get<std::string>()));
    CHECK(request(client,"personas.list").empty());
    rejects([&]{request(client,"command",{{"name","status"}});});
    CHECK(request(client,"session.close")["closed"]==true); // Must not recreate memories or files.
    auto fresh=request(client,"personas.create",{{"name","Active profile"},{"soul","Fresh identity"}});
    CHECK(fresh["uuid"]!=aid);
    request(client,"persona.activate",{{"uuid",fresh["uuid"]},{"cwd",project.string()}});
    CHECK(request(client,"command",{{"name","memory"},{"arguments",{{"kind","know"},{"query","private"}}}})["results"].empty());
    CHECK(erase(client,fresh["uuid"])["current"]==true);
    auto closed=request(client,"personas.create",{{"name","Closed session fixture"}});
    auto closed_id=closed["uuid"].get<std::string>();
    request(client,"persona.activate",{{"uuid",closed_id},{"cwd",project.string()}});
    request(client,"session.close");auto closed_logs=Json::parse(read_file(paths.persona(closed_id)/"diagnostic-bundles.json"));
    Registry(paths).erase(closed_id);request(client,"debug.configure",options.json());
    for(const auto& path:closed_logs)CHECK(!fs::exists(path.get<std::string>()));
  }
  server.join();CHECK(!fs::exists(paths.persona("00000000-0000-0000-0000-000000000000")));
}
}
int main() {
  auto root=fs::temp_directory_path()/("saga-persona-erase-"+uuid());
  try{
    Paths paths{root/"config",root/"data",root/"state",root/"run"};paths.create();private_dir(root/"project");
    registry_tests(paths,root/"project");protocol_tests(paths,root/"project");fs::remove_all(root);
    std::cout<<"PASS persona erasure, isolation, locking, ownership, migration and current-session lifecycle\n";return 0;
  }catch(const std::exception& error){std::cerr<<"FAIL persona erasure: "<<error.what()<<'\n';fs::remove_all(root);return 1;}
}
