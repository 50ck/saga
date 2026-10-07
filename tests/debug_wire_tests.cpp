#include <saga/debug.hpp>
#include <fstream>
#include <set>
#define SAGA_INTEGRATION_NO_MAIN
#include "integration.cpp"
using namespace saga;
namespace {
Json records(const fs::path& directory) {
  Json events=Json::array();for(auto& file:fs::recursive_directory_iterator(directory)) {
    if(file.path().extension()!=".log" || file.path().filename()=="fatal-signal.log")continue;
    std::ifstream input(file.path());std::string line;while(std::getline(input,line))events.push_back(Json::parse(line));
  }return events;
}
void cli_profiles(MockModel& mock,const std::string& daemon,const std::string& client) {
  Environment env;env.start(daemon);Peer peer(env.paths.socket());
  auto persona=peer.request("personas.create",{{"name","Recorder fixture"},{"soul","Synthetic identity"}});
  peer.request("backend.setup",{{"endpoint",mock.endpoint}});
  for(auto profile:{"debug","trace","wire","forensic"}) {
    int pipe_fd[2];CHECK(pipe(pipe_fd)==0);auto child=fork();CHECK(child>=0);
    if(child==0) {
      chdir(env.project.c_str());dup2(pipe_fd[0],0);close(pipe_fd[0]);close(pipe_fd[1]);
      auto output=env.root/(std::string(profile)+".out");int fd=open(output.c_str(),O_CREAT|O_WRONLY|O_TRUNC,0600);dup2(fd,1);dup2(fd,2);close(fd);
      auto id=persona["uuid"].get<std::string>();execl(client.c_str(),client.c_str(),"--batch",id.c_str(),"--no-tui","--debug",profile,static_cast<char*>(nullptr));_exit(127);
    }
    close(pipe_fd[0]);std::string input="hello\n/fact fixture choice marker\n/know marker\n/exit\n";CHECK(write(pipe_fd[1],input.data(),input.size())==static_cast<ssize_t>(input.size()));close(pipe_fd[1]);int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    auto output=read_file(env.root/(std::string(profile)+".out"));
    CHECK(output.find("Saga diagnostic follow: ")!=std::string::npos && output.find("/follow (use tail -F --max-unchanged-stats=1)")!=std::string::npos);
  }
  env.stop();auto events=records(env.paths.state/"logs");std::set<std::string> profiles;
  for(auto& event:events)if(event["event"]=="debug.session_started")profiles.insert(event["debug_profile"].get<std::string>());
  CHECK(profiles.size()==4);
  bool prompt=false,raw=false,memory=false;for(auto& event:events){prompt|=event["event"]=="prompt.snapshot";raw|=event["event"]=="stream.raw_complete";memory|=event["event"]=="memory.search.started";}
  CHECK(prompt && raw && memory);
}
}
int main(int argc,char** argv) {
  auto root=fs::temp_directory_path()/("saga-debug-wire-"+uuid());
  try {
    CHECK(argc==3);private_dir(root);MockModel mock;DebugOptions options;options.profile=DebugProfile::Forensic;options.directory=root/"records";
    {DebugLogger log(options);DebugScope scope(&log);model_contracts(mock);http_continuation(mock);}
    auto events=records(root/"records");bool raw=false,parsed=false,decision=false;
    for(auto& event:events){
      if(event["event"]=="stream.raw_complete"){raw=true;CHECK(event.contains("request_id"));auto file=root/"records";bool found=false;
        for(auto& entry:fs::recursive_directory_iterator(file))if(entry.path().generic_string().ends_with(event["payload"]["path"].get<std::string>())) {auto text=read_file(entry.path(),32*1024*1024);CHECK(debug_sha256(text)==event["payload"]["sha256"].get<std::string>());CHECK(text.find("test-key")==std::string::npos);found=true;break;}
        CHECK(found);
      }
      if(event["event"]=="stream.parsed"){parsed=true;CHECK(event.contains("request_id"));}
      if(event["event"]=="runtime.decision" && event["decision"]=="continue_generation")decision=true;
    }
    CHECK(raw && parsed && decision);
    cli_profiles(mock,fs::canonical(argv[1]).string(),fs::canonical(argv[2]).string());
    end_to_end(mock,fs::canonical(argv[1]).string(),fs::canonical(argv[2]).string());
    fs::remove_all(root);std::cout<<"Wire and CLI recorder tests passed\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<"\nFixtures: "<<root<<'\n';return 1;}
}
