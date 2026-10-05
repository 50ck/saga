#include "check.hpp"
#include <sys/socket.h>

// Exercise the production connection handler over an inherited Unix socket.
// The sandbox permits socketpair, though it denies binding the listener.
#define main saga_daemon_binary_main
#include "../src/daemon.cpp"
#undef main

namespace {
int approval_requests=0;
saga::Json request(saga::Channel& client,std::string type,saga::Json payload = saga::Json::object(),bool approved = false) {
  auto id = saga::uuid();
  client.send({{"type",std::move(type)},{"request_id",id},{"payload",std::move(payload)}});
  for (int tries = 0; tries < 100; ++tries) {
    auto frame = client.receive(1000);
    if (!frame || frame->value("request_id","") != id) continue;
    auto event = frame->value("type","");
    if (event == "approval.requested") {
      ++approval_requests;
      client.send({{"type","approval"},{"request_id",id},{"payload",{{"approval_id",frame->at("payload").at("approval_id")},{"approved",approved}}}});
    }
    if (event == "error") return {{"error",(*frame)["payload"]["message"]}};
    if (event == "result") return frame->at("payload");
  }
  throw std::runtime_error("Daemon did not finish the request");
}
}
int main() {
  using namespace saga;
  fs::path root = fs::temp_directory_path() / ("saga-daemon-protocol-" + uuid());
  try {
    Paths paths{root/"config",root/"data",root/"state",root/"run"}; paths.create();
    atomic_write(paths.runtime/"control.token","test-control-secret");
    for (bool valid : {false,true}) {
      int auth_pair[2]; CHECK(socketpair(AF_UNIX,SOCK_STREAM | SOCK_CLOEXEC,0,auth_pair) == 0);
      std::jthread auth_server([&]{ serve_connected(auth_pair[1],paths,true); });
      {
        Channel auth(auth_pair[0]);
        auth.send({{"type","authenticate"},{"request_id","auth-check"},{"payload",{{"token",valid ? "test-control-secret" : "wrong"}}}});
        if (valid) { auto reply=auth.receive(1000); CHECK(reply && (*reply)["payload"]["authenticated"] == true); CHECK(request(auth,"ping")["runtime"] == "Saga"); }
        else rejects([&]{ auth.receive(1000); });
      }
      auth_server.join();
    }
    fs::path project = root/"project"; private_dir(project);
    Config config; config.endpoint = "http://127.0.0.1:1"; config.model = "test-engine"; config.context_length = 65536; config.save(paths);
    int pair[2]; CHECK(socketpair(AF_UNIX,SOCK_STREAM | SOCK_CLOEXEC,0,pair) == 0);
    std::jthread server([&]{ serve_connected(pair[1],paths); });
    std::string own_id;
    {
      Channel client(pair[0]);
      auto info=request(client,"ping");CHECK(info["runtime"]=="Saga" && info["context_revision"]==runtime_context_revision);
      CHECK(request(client,"personas.list").empty());
      auto a = request(client,"personas.create",{{"name","Identity A"},{"soul","Only A's identity"}});
      auto b = request(client,"personas.create",{{"name","Identity B"},{"soul","Only B's identity"}});
      CHECK(a.contains("uuid") && b.contains("uuid") && a["uuid"] != b["uuid"]);
      own_id = a["uuid"];
      auto activate = [&](const Json& metadata){ return request(client,"persona.activate",{{"uuid",metadata["uuid"]},{"cwd",project.string()}}); };
      CHECK(activate(a)["active"] == true);
      CHECK(request(client,"command",{{"name","permissions"}})["mode"] == "ask_always");
      if (shell_sandbox_available()) {
        auto shell=[&](std::string command,std::string execution,bool approve = false){ return request(client,"command",{{"name","tool"},{"arguments",{{"name","shell_exec"},{"arguments",{{"command",command},{"execution",execution}}}}}},approve); };
        CHECK(shell("printf sandbox","sandbox")["stdout"] == "sandbox"); CHECK(approval_requests == 0);
        CHECK(shell("printf host","host").contains("error")); CHECK(approval_requests == 1);
        CHECK(shell("printf host","host",true)["stdout"] == "host"); CHECK(approval_requests == 2);
      }
      CHECK(request(client,"command",{{"name","permissions"},{"arguments",{{"mode","always_approve"}}}})["mode"] == "always_approve");
      CHECK(request(client,"personas.list").contains("error"));
      CHECK(request(client,"command",{{"name","soul"}})["content"] == "Only A's identity");
      CHECK(request(client,"command",{{"name","fact"},{"arguments",{{"subject","machine"},{"predicate","OS"},{"object","FreeBSD"}}}}).contains("id"));
      CHECK(request(client,"command",{{"name","memory"},{"arguments",{{"kind","know"},{"query","FreeBSD"}}}})["results"].size() == 1);
      CHECK(activate(b)["active"] == true);
      CHECK(request(client,"command",{{"name","permissions"}})["mode"] == "ask_always");
      CHECK(request(client,"command",{{"name","soul"}})["content"] == "Only B's identity");
      CHECK(request(client,"command",{{"name","memory"},{"arguments",{{"kind","know"},{"query","FreeBSD"}}}})["results"].empty());
      CHECK(activate(a)["active"] == true);
      CHECK(request(client,"command",{{"name","permissions"}})["mode"] == "always_approve");
      auto status=request(client,"command",{{"name","status"}}); CHECK(status["name"] == "Identity A"); CHECK(status["soul_path"] == (paths.persona(own_id)/"SOUL.md").string()); CHECK(status["compactions"] == 0);
      CHECK(request(client,"command",{{"name","memory"},{"arguments",{{"kind","know"},{"query","FreeBSD"}}}})["results"].size() == 1);
      CHECK(request(client,"command",{{"name","name"},{"arguments",{{"name","Renamed A"}}}})["name"] == "Renamed A");
      CHECK(request(client,"session.close")["closed"] == true);
      auto list = request(client,"personas.list"); CHECK(list.size() == 2);
      for (const auto& persona : list) {
        if (persona["uuid"] == a["uuid"]) CHECK(persona["display_name"] == "Renamed A");
        if (persona["uuid"] == b["uuid"]) CHECK(persona["display_name"] == "Identity B");
      }
    }
    server.join();
    Database own(paths.persona(own_id)/"agent.db");
    CHECK(own.query("SELECT * FROM journal_entries").size() == 1);
    fs::remove_all(root);
    std::cout << "PASS daemon control protocol, session switch and isolation\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL daemon protocol: " << e.what() << '\n';
    fs::remove_all(root); return 1;
  }
}
