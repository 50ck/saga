#include <saga/debug.hpp>
#include <saga/db.hpp>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <fstream>
#include <set>
#include "check.hpp"
// Reuse the production-runtime synthetic provider scenarios without networking.
#define main generation_suite_main
#include "generation_tests.cpp"
#undef main
using namespace saga;
namespace {
Json lines(const fs::path& file) {
  Json out=Json::array();std::ifstream input(file);std::string line;
  while(std::getline(input,line))out.push_back(Json::parse(line));
  return out;
}
size_t count(const Json& events,std::string_view name) {return std::count_if(events.begin(),events.end(),[&](auto& e){return e.value("event","")==name;});}
void redaction() {
  RedactionEngine redact;redact.secret("registered-credential");
  auto result=redact.apply(Json{{"Authorization","Bearer abc123"},{"password","foo"},{"nested",Json::array({{{"access_token","zzz"}}})},{"body","Authorization: Bearer abc123\npassword=foo\nhttps://example.test/?token=zzz\nregistered-credential"},{"key","-----BEGIN PRIVATE KEY-----\nabc\n-----END PRIVATE KEY-----"},{"triple",{{"predicate","password"},{"object","foo"}}}}).dump();
  CHECK(result.find("abc123")==std::string::npos && result.find("foo")==std::string::npos && result.find("zzz")==std::string::npos && result.find("registered-credential")==std::string::npos);
  CHECK(result.find("<redacted>")!=std::string::npos);
  std::string stream="data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"registered-\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"credential\"}}]}\n\n";
  auto safe_stream=redact.apply(Json(stream)).get<std::string>();CHECK(safe_stream.find("registered-")==std::string::npos && safe_stream.find("credential")==std::string::npos);
  CHECK(RedactionEngine(true).apply(Json{{"password","foo"}})["password"]=="foo");
  CHECK(redact.apply(Json("{\"password\":\"foo\"}"))=="{\"password\":\"<redacted>\"}");
  CHECK(redact.apply(Json(std::string(150,'[')+"0"+std::string(150,']')))=="<redacted:invalid-json>");
  CHECK(debug_sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(debug_sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(debug_sha256(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
void cli_options() {
  std::vector<std::string> args={"saga","--debug","--debug-dir","/tmp/fixture","--debug-components","runtime,llm.stream","--debug-exclude","tui","--debug-rotate-size","100M","--debug-keep","3"};
  std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());DebugOptions options;
  for(int i=1;i<static_cast<int>(argv.size());++i)CHECK(debug_argument(argv.size(),argv.data(),i,options));
  CHECK(options.profile==DebugProfile::Debug && options.rotate_bytes==100*1024*1024 && options.keep==3 && options.components.size()==2);
  CHECK(debug_profile("off")==DebugProfile::Off);rejects([]{debug_profile("invalid");});
}
void benchmark(const fs::path& root) {
  DebugOptions options;options.profile=DebugProfile::Debug;options.directory=root/"benchmark";
  DebugLogger logger(options);auto started=std::chrono::steady_clock::now();
  for(int i=0;i<5000;++i)logger.emit("runtime","benchmark.sample",{{"index",i},{"generation_id",1}},DebugProfile::Debug,"DEBUG");
  logger.flush();auto duration=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  std::cout<<"Debug recorder benchmark: "<<duration<<" ms / 5000 events ("<<duration/5<<" us/event, including writer drain)\n";
}
void logger_tests(const fs::path& root) {
  CHECK(debug_filename("../noa ?",42,1791310031)=="-noa-42-1791310031.log");
  CHECK(debug_filename("",0,1)=="saga-0-1.log");
  DebugOptions options;options.profile=DebugProfile::Forensic;options.directory=root/"logs";
  fs::path path;
  {
    DebugLogger log(options);log.session("noa",42,{{"persona_id","synthetic"}});path=log.path();DebugScope scope(&log);
    std::vector<std::jthread> threads;
    for(int i=0;i<4;++i)threads.emplace_back([&,i]{for(int j=0;j<50;++j)log.emit("runtime","thread.event",{{"writer",i},{"index",j},{"password","fixture-secret"}});});
    threads.clear();
    {TraceSpan parent("runtime","parent");TraceSpan child("tool","child");}
    log.emit("prompt","prompt.snapshot",{{"payload",{{"messages",Json::array({{{"role","user"},{"content","hello"}}})}}}},DebugProfile::Forensic);
    log.record_context(Json::array({{{"role","user"},{"content","hello"}}}),Json::array(),{{"context_id","ctx_1"}});
    log.record_context(Json::array({{{"role","user"},{"content","hello"}},{{"role","assistant"},{"content","answer"}}}),Json::array(),{{"context_id","ctx_2"}});
    log.crash("synthetic diagnostic exception");
    CHECK(fs::exists(path.parent_path()/"crash/flight-recorder.jsonl"));
  }
  auto events=lines(path);CHECK(events.front()["event"]=="debug.session_started" && events.back()["event"]=="debug.session_completed");
  CHECK(count(events,"thread.event")==200 && count(events,"context.diff")==1);
  std::uint64_t seq=0,mono=0;for(auto& event:events){CHECK(event["seq"].get<std::uint64_t>()==++seq);CHECK(event["mono_ns"].get<std::uint64_t>()>=mono);mono=event["mono_ns"];CHECK(event.contains("ts"));}
  CHECK(read_file(path).find("fixture-secret")==std::string::npos);
  struct stat info{};CHECK(stat(path.c_str(),&info)==0 && (info.st_mode&0777)==0600);CHECK(stat(path.parent_path().c_str(),&info)==0 && (info.st_mode&0777)==0700);
  auto manifest=Json::parse(read_file(path.parent_path()/"manifest.json"));CHECK(manifest["events_written"]==events.size());
  for(auto& event:events)if(event["event"]=="prompt.snapshot"){auto payload=event["payload"];auto data=read_file(path.parent_path()/payload["path"].get<std::string>());CHECK(debug_sha256(data)==payload["sha256"].get<std::string>());}
  options.directory=root/"filtered";options.components={"runtime"};options.exclude={"runtime.verbose"};
  {DebugLogger log(options);path=log.path();log.emit("memory","excluded");log.emit("runtime","included");log.emit("runtime.verbose","excluded");log.emit("memory","error",Json::object(),DebugProfile::Debug,"ERROR");}
  events=lines(path);CHECK(count(events,"included")==1 && count(events,"excluded")==0 && count(events,"error")==1);
  options.components.clear();options.exclude.clear();options.directory=root/"rotation";options.rotate_bytes=4096;options.keep=2;
  {DebugLogger log(options);path=log.path();for(int i=0;i<80;++i)log.emit("runtime","rotate",{{"content",std::string(800,'x')}});}
  manifest=Json::parse(read_file(path.parent_path()/"manifest.json"));CHECK(manifest["log_files"].size()==2);for(auto& file:manifest["log_files"])CHECK(!lines(path.parent_path()/file.get<std::string>()).empty());
  options.directory=root/"drop";options.queue_bytes=1024;
  {DebugLogger log(options);path=log.path();for(int i=0;i<100;++i)log.emit("tui","verbose",{{"content",std::string(800,'x')}},DebugProfile::Trace,"TRACE",false);log.emit("runtime","critical.completed");}
  events=lines(path);CHECK(count(events,"logger.events_dropped")>0 && count(events,"critical.completed")==1);
  // Disk failure disables diagnostics rather than throwing into the agent.
  options.directory="/proc/saga-unwritable";{DebugLogger log(options);log.emit("runtime","still_alive");log.flush();}
}
void runtime_recording(const fs::path& root) {
  Fixture f;DebugOptions options;options.profile=DebugProfile::Forensic;options.directory=root/"runtime";fs::path path;
  {
    DebugLogger recorder(options);DebugScope recording(&recorder);
    auto r=f.runtime(std::make_unique<ScriptBackend>(std::vector<Segment>{limited(),tool("open","project_open",{{"path",(f.root/"second").string()},{"create",true}}),answer()}));
    path=recorder.path();auto input="hello marker "+(f.root/"second").string();r->chat(input,f.emit(),"one-operator-message");r->chat(input,f.emit(),"one-operator-message");
    auto& p=r->persona();Memory memory(p,f.config);
    auto source=p.db->event("user.message",{{"content","fixture choice marker"}},p.session);
    auto fact=memory.fact("fixture","choice","marker",source);CHECK(fact>0);memory.fact("fixture","choice","marker",source);memory.search("know","marker");
    atomic_write(p.project_root/"known.txt","hello\n");Tools tools(p,memory,[](auto&,auto&){return true;},f.config);
    p.turn=p.db->query("SELECT id FROM turns")[0]["id"];p.tool_call="same-file-read";
    auto a=tools.execute("file_read",{{"path","known.txt"}});auto b=tools.execute("file_read",{{"path","known.txt"}});CHECK(a==b);
    p.tool_call="fixture-write";tools.execute("file_write",{{"path","written.txt"},{"content","synthetic file\n"},{"description","Recorder fixture"}});
    tools.permissions("host_always");p.tool_call="fixture-shell";
    auto shell=tools.execute("shell_exec",{{"command","printf 'fixture stdout'; printf 'fixture stderr' >&2"},{"execution","host"}});
    CHECK(shell["stdout"]=="fixture stdout" && shell["stderr"]=="fixture stderr");p.turn.clear();p.tool_call.clear();
    p.db->transaction([&]{p.db->exec("UPDATE facts SET object=? WHERE id=?",{"updated",fact});});
    rejects([&]{p.db->transaction([&]{p.db->exec("UPDATE facts SET object=? WHERE id=?",{"rolled-back",fact});throw std::runtime_error("fixture rollback");});});
    auto through=p.db->query("SELECT max(id) AS id FROM messages WHERE session_id=?",{p.session})[0]["id"].get<Id>();memory.checkpoint("manual_fixture",through);
    r->close("fixture_completed");
  }
  auto events=lines(path);CHECK(count(events,"user_message.received")==1 && count(events,"user_message.dispatched")==1 && count(events,"turn.started")==1);
  CHECK(count(events,"memory.write.committed")>0 && count(events,"memory.write.rolled_back")==1 && count(events,"memory.selection")==1);
  CHECK(count(events,"workspace.transition")==1 && count(events,"tool.call.started")>=3);
  bool classification=false,continuation=false;
  for(auto& event:events){if(event["event"]=="generation.classified" && event["classification"]=="output_limit")classification=true;if(event["event"]=="runtime.decision" && event["decision"]=="continue_generation" && event["reason"]=="output_limit")continuation=true;}
  CHECK(classification && continuation);CHECK(count(events,"context.diff")>0 && count(events,"context.reset")==1);
  CHECK(count(events,"fs.written")==1 && count(events,"fs.diff")==1);
  bool shell_recorded=false;for(auto& event:events)if(event["event"]=="tool.completed" && event["tool"]=="shell_exec")shell_recorded=event["result"]["stdout"]=="fixture stdout" && event["result"]["stderr"]=="fixture stderr";CHECK(shell_recorded);
}
void cancellation_recording(const fs::path& root) {
  Fixture fixture;DebugOptions options;options.profile=DebugProfile::Trace;options.directory=root/"cancelled";fs::path path;
  {DebugLogger logger(options);DebugScope scope(&logger);auto runtime=fixture.runtime(std::make_unique<ScriptBackend>(std::vector<Segment>{limited()}));path=logger.path();bool stopped=false;
    runtime->service([&]{if(!stopped && !runtime->persona().db->query("SELECT id FROM events WHERE type='reasoning.started'").empty()){stopped=true;runtime->command("stop");}});
    runtime->chat("hello",fixture.emit());runtime->close("cancelled_fixture");}
  auto events=lines(path);CHECK(count(events,"cancellation.requested")==1 && count(events,"turn.cancelled")==1 && count(events,"debug.session_completed")==1);
}
void fatal_recorder(const fs::path& root) {
  auto child=fork();CHECK(child>=0);
  if(child==0){DebugOptions options;options.profile=DebugProfile::Debug;options.directory=root/"fatal";DebugLogger log(options);DebugScope scope(&log);install_debug_crash_handlers();log.emit("runtime","checkpoint.completed");log.flush();raise(SIGABRT);_exit(1);}
  int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==128+SIGABRT);
  bool marker=false;for(auto& file:fs::recursive_directory_iterator(root/"fatal"))if(file.path().filename()=="fatal-signal.log")marker=!lines(file.path()).empty();CHECK(marker);
}
}
int main(){auto root=fs::temp_directory_path()/("saga-debug-tests-"+uuid());try{private_dir(root);redaction();cli_options();logger_tests(root);runtime_recording(root);cancellation_recording(root);fatal_recorder(root);benchmark(root);fs::remove_all(root);std::cout<<"Diagnostic recorder tests passed\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<"\nFixtures: "<<root<<'\n';return 1;}}
