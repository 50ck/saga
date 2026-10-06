#include <saga/debug.hpp>
#include <build_metadata.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <cctype>
#include <bit>
#include <iomanip>
#include <map>
#include <set>
#include <sys/resource.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>

namespace saga {
namespace {
thread_local DebugLogger* active=nullptr;
thread_local std::string parent_span;
std::array<std::atomic<int>,64> emergency_fds;
std::once_flag emergency_init;
void emergency(int signal) {
  const char line[]="{\"event\":\"crash.fatal_signal\",\"severity\":\"FATAL\",\"note\":\"Consult the durable journal and last flushed events\"}\n";
  for(auto &entry:emergency_fds){int fd=entry.load(std::memory_order_relaxed);if(fd>=0){auto ignored=::write(fd,line,sizeof(line)-1);(void)ignored;}}
  _exit(128+signal);
}
bool match_component(std::string_view value,const std::vector<std::string>& filters) {
  for(auto &filter:filters)if(value==filter || (value.starts_with(filter) && value.size()>filter.size() && value[filter.size()]=='.'))return true;
  return false;
}
bool sensitive(std::string key) {
  key=lower(key);std::erase_if(key,[](char c){return c=='_' || c=='-' || c==' ';});
  for(auto value:{"authorization","proxyauthorization","cookie","setcookie","apikey","token","accesstoken","refreshtoken","password","secret","privatekey","clientsecret","sessiontoken"})if(key==value)return true;
  return key.ends_with("password") || key.ends_with("apikey") || key.ends_with("accesstoken") || key.ends_with("privatekey");
}
std::string timestamp() {
  auto milliseconds=now();time_t seconds=milliseconds/1000;tm value{};gmtime_r(&seconds,&value);
  char buffer[48];std::strftime(buffer,sizeof buffer,"%Y-%m-%dT%H:%M:%S",&value);
  char suffix[16];std::snprintf(suffix,sizeof suffix,".%03lldZ",static_cast<long long>(milliseconds%1000));return std::string(buffer)+suffix;
}
void write_all(int fd,std::string_view data) {
  while(!data.empty()){auto n=::write(fd,data.data(),data.size());if(n<0 && errno==EINTR)continue;if(n<=0)throw std::runtime_error("Diagnostic write failed");data.remove_prefix(static_cast<size_t>(n));}
}
void private_file(const fs::path &path,std::string_view data) {
  int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if(fd<0)throw std::runtime_error("Cannot create diagnostic payload");
  try{write_all(fd,data);}catch(...){::close(fd);throw;}::close(fd);
}
std::vector<std::string> list(std::string text) {
  std::vector<std::string> result;size_t begin=0;
  while(begin<text.size()){auto end=text.find(',',begin);auto part=trim(std::string_view(text).substr(begin,end==std::string::npos?end:end-begin));if(part.empty() || part.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")!=std::string::npos)throw std::runtime_error("Invalid diagnostic component list");result.push_back(part);if(end==std::string::npos)break;begin=end+1;}
  return result;
}
size_t size_value(std::string text) {
  if(text.empty() || !std::isdigit(static_cast<unsigned char>(text.front())))throw std::runtime_error("Invalid diagnostic size");
  size_t used=0;auto n=std::stoull(text,&used);size_t multiplier=1;
  auto suffix=lower(text.substr(used));if(suffix=="k" || suffix=="kb")multiplier=1024;else if(suffix=="m" || suffix=="mb")multiplier=1024*1024;else if(suffix=="g" || suffix=="gb")multiplier=1024*1024*1024;else if(!suffix.empty())throw std::runtime_error("Invalid diagnostic size");
  if(n>SIZE_MAX/multiplier)throw std::runtime_error("Diagnostic size overflow");
  return static_cast<size_t>(n)*multiplier;
}
Json summary(const Json& value) {
  if(value.is_string())return Json{{"bytes",value.get_ref<const std::string&>().size()},{"tokens_estimate",estimate_tokens(value.get_ref<const std::string&>())}};
  if(value.is_object()){Json out=Json::object();for(auto it=value.begin();it!=value.end();++it)out[it.key()]=summary(it.value());return out;}
  if(value.is_array()){return Json{{"count",value.size()}};}return value;
}
}
std::string debug_sha256(std::string_view input) {
  constexpr std::array<std::uint32_t,64> constants={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  std::array<std::uint32_t,8> state={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  auto blocks=(input.size()+9+63)/64;
  for(size_t block=0;block<blocks;++block) {
    std::array<std::uint32_t,64> words{};
    for(size_t j=0;j<64;++j){auto index=block*64+j;unsigned char byte=0;
      if(index<input.size())byte=static_cast<unsigned char>(input[index]);else if(index==input.size())byte=0x80;
      else if(index>=blocks*64-8)byte=static_cast<unsigned char>((static_cast<std::uint64_t>(input.size())*8)>>((blocks*64-1-index)*8));
      words[j/4]|=static_cast<std::uint32_t>(byte)<<((3-j%4)*8);
    }
    for(size_t j=16;j<64;++j){auto a=words[j-15],b=words[j-2];words[j]=words[j-16]+(std::rotr(a,7)^std::rotr(a,18)^(a>>3))+words[j-7]+(std::rotr(b,17)^std::rotr(b,19)^(b>>10));}
    auto working=state;
    for(size_t j=0;j<64;++j){auto [a,b,c,d,e,f,g,h]=working;
      auto first=h+(std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25))+((e&f)^(~e&g))+constants[j]+words[j];
      auto second=(std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22))+((a&b)^(a&c)^(b&c));
      working={first+second,a,b,c,d+first,e,f,g};
    }
    for(size_t j=0;j<8;++j)state[j]+=working[j];
  }
  std::ostringstream output;output<<std::hex<<std::setfill('0');for(auto word:state)output<<std::setw(8)<<word;return output.str();
}
std::string debug_profile_name(DebugProfile profile){constexpr std::array names={"off","debug","trace","wire","forensic"};return names.at(static_cast<size_t>(profile));}
DebugProfile debug_profile(std::string_view name){for(auto value:{DebugProfile::Off,DebugProfile::Debug,DebugProfile::Trace,DebugProfile::Wire,DebugProfile::Forensic})if(debug_profile_name(value)==name)return value;throw std::runtime_error("Debug level must be off, debug, trace, wire or forensic");}
Json DebugOptions::json() const{return {{"profile",debug_profile_name(profile)},{"directory",directory.string()},{"allow_secrets",allow_secrets},{"rotate_bytes",rotate_bytes},{"keep",keep},{"components",components},{"exclude",exclude},{"sample_interval_ms",sample_interval_ms}};}
DebugOptions DebugOptions::from_json(const Json& value,const Paths& paths) {
  DebugOptions options;options.specified=true;options.profile=debug_profile(value.value("profile","off"));options.directory=value.value("directory",std::string());if(options.directory.empty())options.directory=paths.state/"logs";
  options.allow_secrets=value.value("allow_secrets",false);options.rotate_bytes=value.value("rotate_bytes",options.rotate_bytes);options.keep=value.value("keep",size_t(0));options.components=value.value("components",std::vector<std::string>{});options.exclude=value.value("exclude",std::vector<std::string>{});options.sample_interval_ms=value.value("sample_interval_ms",5000);
  if(options.rotate_bytes<4096 || options.rotate_bytes>4ULL*1024*1024*1024 || options.keep>10000 || options.sample_interval_ms<100 || options.sample_interval_ms>3600000)throw std::runtime_error("Diagnostic limits are out of range");
  for(auto &filter:options.components)list(filter);
  for(auto &filter:options.exclude)list(filter);
  options.directory=fs::absolute(options.directory).lexically_normal();
  if(within(options.directory,paths.data/"personas"))
    throw std::runtime_error("Diagnostic directory must be outside persona storage");
  return options;
}
bool debug_argument(int argc,char** argv,int& index,DebugOptions& options) {
  std::string arg=argv[index];if(!arg.starts_with("--debug"))return false;
  auto value=[&]()->std::string{if(index+1>=argc)throw std::runtime_error("Missing value for "+arg);return argv[++index];};
  if(arg=="--debug") {options.profile=DebugProfile::Debug;if(index+1<argc && !std::string_view(argv[index+1]).starts_with('-'))options.profile=debug_profile(value());}
  else if(arg.starts_with("--debug="))options.profile=debug_profile(arg.substr(8));
  else if(arg=="--debug-dir")options.directory=value();
  else if(arg=="--debug-allow-secrets" || arg=="--debug-no-redact")options.allow_secrets=true;
  else if(arg=="--debug-components")options.components=list(value());
  else if(arg=="--debug-exclude")options.exclude=list(value());
  else if(arg=="--debug-rotate-size")options.rotate_bytes=size_value(value());
  else if(arg=="--debug-keep"){auto count=value();if(count.empty() || !std::isdigit(static_cast<unsigned char>(count.front())))throw std::runtime_error("Invalid debug retention count");size_t used=0;options.keep=std::stoull(count,&used);if(used!=count.size())throw std::runtime_error("Invalid debug retention count");}
  else throw std::runtime_error("Unknown debug flag: "+arg);
  options.specified=true;return true;
}
std::string debug_filename(std::string_view name,Id session,Id seconds) {
  std::string clean;for(unsigned char c:name){if((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-')clean+=static_cast<char>(c);else if(clean.empty() || clean.back()!='-')clean+='-';if(clean.size()>=64)break;}
  while(!clean.empty() && clean.back()=='-')clean.pop_back();
  if(clean.empty())clean="saga";
  return clean+"-"+std::to_string(session)+"-"+std::to_string(seconds)+".log";
}
void RedactionEngine::secret(std::string_view value){if(value.empty())return;std::lock_guard lock(mutex_);if(std::find(secrets_.begin(),secrets_.end(),value)==secrets_.end())secrets_.emplace_back(value);}
Json RedactionEngine::apply(const Json& input) const {
  if(allow_)return input;
  std::lock_guard lock(mutex_);
  std::function<Json(const Json&,unsigned)> walk=[&](const Json& value,unsigned depth)->Json{
    if(depth>128)return "<redacted:depth-limit>";
    if(value.is_object()){Json out=Json::object();for(auto it=value.begin();it!=value.end();++it)out[it.key()]=(sensitive(it.key()) || (it.key()=="content" && value.value("encoding",Json())=="base64") || (it.key()=="object" && value.contains("predicate") && value["predicate"].is_string() && sensitive(value["predicate"].get<std::string>())))?Json("<redacted>"):walk(it.value(),depth+1);return out;}
    if(value.is_array()){Json out=Json::array();bool hide=false;for(auto &item:value){out.push_back(hide?Json("<redacted>"):walk(item,depth+1));hide=item.is_string() && sensitive(item.get<std::string>());}return out;}
    if(!value.is_string())return value;
    auto text=value.get<std::string>();
    if(text.size()<16*1024*1024 && !text.empty() && (text.front()=='{' || text.front()=='[')) {try {auto parsed=Json::parse(text,[](int depth,Json::parse_event_t,Json&){if(depth>128)throw std::runtime_error("Depth limit");return true;},false);if(!parsed.is_discarded())return walk(parsed,depth+1).dump();}catch(...) {return "<redacted:invalid-json>";}}
    if(depth<16 && text.find("data:")!=std::string::npos && text.find("\"choices\"")!=std::string::npos) {
      struct Frame {size_t begin,end;Json json;};std::vector<Frame> frames;
      std::map<std::string,std::string> channels;
      auto visit=[&](Json& frame,const auto& operation) {
        if(!frame.is_object() || !frame.contains("choices") || !frame["choices"].is_array())return;
        for(auto& choice:frame["choices"]) {
          if(!choice.is_object())continue;
          auto& delta=choice.contains("delta")?choice["delta"]:choice["message"];if(!delta.is_object())continue;
          auto prefix=choice.contains("index") && choice["index"].is_number_integer()?choice["index"].get<int>():0;auto key_prefix=std::to_string(prefix)+":";
          for(auto key:{"content","reasoning_content","reasoning","commentary"})if(delta.contains(key) && delta[key].is_string())operation(key_prefix+key,delta[key]);
          if(delta.contains("tool_calls") && delta["tool_calls"].is_array())for(auto& call:delta["tool_calls"])if(call.is_object() && call.contains("function") && call["function"].is_object()) {
            auto index=call.contains("index") && call["index"].is_number_integer()?call["index"].get<int>():0;auto& function=call["function"];
            for(auto key:{"name","arguments"})if(function.contains(key) && function[key].is_string())operation(key_prefix+"tool:"+std::to_string(index)+":"+key,function[key]);
          }
        }
      };
      for(size_t begin=0;begin<text.size();) {
        auto end=text.find("\n\n",begin);auto cr=text.find("\r\n\r\n",begin);
        if(end==std::string::npos || (cr!=std::string::npos && cr<end))end=cr;
        size_t next=end==std::string::npos?text.size():end+(text[end]=='\r'?4:2);
        std::string data;size_t line=begin;
        while(line<next){auto newline=text.find('\n',line);if(newline==std::string::npos || newline>next)newline=next;auto piece=std::string_view(text).substr(line,newline-line);if(piece.ends_with('\r'))piece.remove_suffix(1);if(piece.starts_with("data:")){piece.remove_prefix(5);if(piece.starts_with(' '))piece.remove_prefix(1);data.append(piece);data+='\n';}line=newline+1;}
        auto json=Json::parse(data,[](int depth,Json::parse_event_t,Json&){if(depth>128)throw std::runtime_error("Depth limit");return true;},false);
        if(!json.is_discarded()){visit(json,[&](const std::string& key,const Json& value){channels[key]+=value.get<std::string>();});frames.push_back({begin,next,std::move(json)});}
        begin=next;
      }
      std::set<std::string> unsafe;
      for(auto& [key,channel]:channels)if(walk(Json(channel),depth+1)!=Json(channel))unsafe.insert(key);
      for(auto frame=frames.rbegin();frame!=frames.rend();++frame) {
        bool changed=false;visit(frame->json,[&](const std::string& key,Json& value){if(unsafe.contains(key)){value="<redacted:stream-channel>";changed=true;}});
        if(changed)text.replace(frame->begin,frame->end-frame->begin,"data: "+frame->json.dump()+"\n\n");
      }
    }
    for(auto &secret:secrets_){size_t at=0;while((at=text.find(secret,at))!=std::string::npos){text.replace(at,secret.size(),"<redacted>");at+=10;}}
    // Linear scanning avoids regex backtracking on hostile multi-megabyte payloads.
    for(size_t at=0;(at=text.find("-----BEGIN ",at))!=std::string::npos;) {
      auto header=text.find("-----",at+11);if(header==std::string::npos)break;
      if(text.substr(at,header-at).find("PRIVATE KEY")==std::string::npos){at=header+5;continue;}
      auto end=text.find("-----END ",header+5);if(end!=std::string::npos){end=text.find("-----",end+9);if(end!=std::string::npos)end+=5;}
      text.replace(at,end==std::string::npos?text.size()-at:end-at,"<redacted>");at+=10;
    }
    for(size_t at=0;at<text.size();) {
      if(text.compare(at,4,"ssh-")==0 && (at==0 || std::isspace(static_cast<unsigned char>(text[at-1])))) {
        auto end=text.find_first_of("\r\n",at);text.replace(at,end==std::string::npos?text.size()-at:end-at,"<redacted>");at+=10;continue;
      }
      if(std::isalnum(static_cast<unsigned char>(text[at])) || text[at]=='_') {
        size_t start=at;while(at<text.size() && (std::isalnum(static_cast<unsigned char>(text[at])) || text[at]=='_' || text[at]=='-'))++at;
        auto key=text.substr(start,at-start);size_t delim=at;
        while(delim<text.size() && (text[delim]==' ' || text[delim]=='\t' || text[delim]=='"' || text[delim]=='\''))++delim;
        if(sensitive(key) && delim<text.size() && (text[delim]=='=' || text[delim]==':')) {
          size_t begin=delim+1;while(begin<text.size() && (text[begin]==' ' || text[begin]=='\t'))++begin;
          size_t end=begin;
          if(begin<text.size() && (text[begin]=='"' || text[begin]=='\'')){char quote=text[begin++];end=text.find(quote,begin);}
          else if(lower(key)=="authorization" || lower(key)=="proxy-authorization" || lower(key)=="cookie" || lower(key)=="set-cookie")end=text.find_first_of("\r\n",begin);
          else end=text.find_first_of(" \t\r\n,;&}\"'",begin);
          if(end==std::string::npos)end=text.size();
          text.replace(begin,end-begin,"<redacted>");at=begin+10;
        }
      } else ++at;
    }
    // URL userinfo can carry passwords without a named sensitive field.
    for(auto scheme:{"https://","http://"})for(size_t at=0;(at=text.find(scheme,at))!=std::string::npos;) {
      at+=std::char_traits<char>::length(scheme);auto end=text.find_first_of("/ \t\r\n",at);auto marker=text.find('@',at);
      if(marker!=std::string::npos && (end==std::string::npos || marker<end)){text.replace(at,marker-at,"<redacted>");at+=11;}
    }
    return text;
  };
  return walk(input,0);
}
Json DebugLogger::build_metadata() {
  utsname os{};uname(&os);
  return {{"version",SAGA_BUILD_VERSION},{"git_commit",SAGA_BUILD_SHA},{"git_branch",SAGA_BUILD_BRANCH},{"git_dirty",SAGA_BUILD_DIRTY},{"build_timestamp",SAGA_BUILD_TIME},{"build_type",SAGA_BUILD_TYPE},{"compiler",SAGA_BUILD_COMPILER},{"compile_flags",SAGA_BUILD_FLAGS},{"features",{"curl","sqlite","lexbor","md4c","ncurses","provider_streaming"}},{"database_schema",12},{"runtime_context_revision",runtime_context_revision},{"protocol_version",1},{"os",os.sysname},{"kernel",os.release},{"architecture",os.machine},{"pid",getpid()},{"ppid",getppid()},{"cwd",fs::current_path().string()},{"locale",std::getenv("LANG")?std::getenv("LANG"):""},{"timezone",std::getenv("TZ")?std::getenv("TZ"):"system"}};
}
DebugLogger::DebugLogger(DebugOptions options):options_(std::move(options)),redact_(options_.allow_secrets),started_(std::chrono::steady_clock::now()),sampled_(started_) {
  std::call_once(emergency_init,[]{for(auto &entry:emergency_fds)entry.store(-1);});
  if(options_.allow_secrets)std::cerr<<"Saga warning: diagnostic secret redaction is DISABLED. Logs may contain credentials and private content.\n";
  if(options_.profile==DebugProfile::Off)return;
  try {if(options_.directory.empty())options_.directory=Paths::environment().state/"logs";session("saga",0);}catch(...){fail();}
}
DebugLogger::~DebugLogger() {
  flush();
  if(fd_>=0)emit("debug","debug.session_completed",{{"events_written",written_},{"events_dropped",dropped_},{"bytes_written",bytes_},{"sidecar_bytes",sidecar_bytes_},{"errors",errors_},{"duration_ms",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started_).count()}});
  flush();{std::lock_guard lock(mutex_);stopping_=true;}condition_.notify_all();if(writer_.joinable())writer_.join();
  if(signal_slot_>=0)emergency_fds[signal_slot_].store(-1);
  if(signal_fd_>=0)::close(signal_fd_);
  if(fd_>=0)::close(fd_);
}
bool DebugLogger::enabled(DebugProfile minimum) const noexcept{return options_.profile>=minimum && options_.profile!=DebugProfile::Off;}
void DebugLogger::session(std::string_view persona,Id id,const Json& fields) noexcept {
  try{open_session(persona,id,fields);}catch(...){fail();}
}
void DebugLogger::open_session(std::string_view persona,Id id,const Json& fields) {
  if(options_.profile==DebugProfile::Off)return;
  if(writer_.joinable()){emit("debug","debug.session_completed",{{"reason","session_rebound"}});flush();{std::lock_guard lock(mutex_);stopping_=true;}condition_.notify_all();writer_.join();}
  if(signal_slot_>=0){emergency_fds[signal_slot_].store(-1);signal_slot_=-1;}if(signal_fd_>=0){::close(signal_fd_);signal_fd_=-1;}if(fd_>=0){::close(fd_);fd_=-1;}
  std::unique_lock lock(mutex_);
  private_dir(options_.directory);auto filename=debug_filename(persona,id,now()/1000);auto base=filename.substr(0,filename.size()-4);bundle_=options_.directory/base;
  unsigned collision=0;while(::mkdir(bundle_.c_str(),0700)!=0){if(errno!=EEXIST)throw std::runtime_error("Cannot create diagnostic session");bundle_=options_.directory/(base+"-"+std::to_string(++collision));}
  log_=bundle_/filename;fd_=::open(log_.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);if(fd_<0)throw std::runtime_error("Cannot open diagnostic journal");
  private_dir(bundle_/"crash");signal_fd_=::open((bundle_/"crash/fatal-signal.log").c_str(),O_WRONLY|O_APPEND|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if(signal_fd_>=0)for(size_t i=0;i<emergency_fds.size();++i){int expected=-1;if(emergency_fds[i].compare_exchange_strong(expected,signal_fd_)){signal_slot_=static_cast<int>(i);break;}}
  started_=std::chrono::steady_clock::now();sampled_=started_;
  stopping_=false;failed_=false;sequence_=0;part_=0;part_bytes_=0;written_=0;bytes_=0;errors_=0;sidecar_bytes_=0;dropped_=0;reported_drops_=0;ring_.clear();previous_context_=Json::object();context_=redact_.apply(fields);context_["session_id"]=id;context_["persona_name"]=persona;
  manifest_={{"format_version",1},{"debug_profile",debug_profile_name(options_.profile)},{"redaction_enabled",!options_.allow_secrets},{"build",build_metadata()},{"context",redact_.apply(context_)},{"start_time",timestamp()},{"log_files",Json::array({filename})},{"sidecars",Json::array()}};
  // Register exact bundle ownership before recording persona payloads. This
  // survives renames, custom --debug-dir locations, rotation and crashes.
  atomic_write(bundle_/"manifest.json",manifest_.dump(2));
  if(fields.contains("persona_directory")) {
    fs::path directory=fields.at("persona_directory").get<std::string>();
    auto catalog=directory/"diagnostic-bundles.json";
    if(fs::is_symlink(catalog))throw std::runtime_error("Refusing aliased diagnostic ownership catalog");
    auto entries=fs::exists(catalog)?Json::parse(read_file(catalog,16*1024*1024)):Json::array();
    if(!entries.is_array())throw std::runtime_error("Invalid diagnostic ownership catalog");
    entries.push_back(fs::canonical(bundle_).string());
    atomic_write(catalog,entries.dump());
  }
  writer_=std::jthread([this]{run();});
  // Enqueue directly while holding the initialization lock to make the header first.
  Json header=context_;header.update({{"ts",timestamp()},{"mono_ns",std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()},{"seq",++sequence_},{"level","debug"},{"severity","INFO"},{"component","debug"},{"event","debug.session_started"},{"debug_profile",debug_profile_name(options_.profile)},{"file_path",log_.string()},{"redaction_enabled",!options_.allow_secrets},{"allow_secrets",options_.allow_secrets}});
  queue_.push_back({redact_.apply(header),header.dump().size(),true});queued_=queue_.back().bytes;condition_.notify_all();
  lock.unlock();emit("runtime","runtime.started",build_metadata());
}
void DebugLogger::replace_context(const Json& fields){std::lock_guard lock(mutex_);context_=redact_.apply(fields);}
void DebugLogger::context(const Json& fields){std::lock_guard lock(mutex_);context_.update(redact_.apply(fields));}
Json DebugLogger::context() const {std::lock_guard lock(mutex_);return context_;}
std::string DebugLogger::next_span(){std::lock_guard lock(mutex_);return "span_"+std::to_string(++span_sequence_);}
void DebugLogger::emit(std::string_view component,std::string_view event,Json fields,DebugProfile minimum,std::string_view severity,bool critical) noexcept {
  if(!enabled(minimum))return;
  try {
    bool mandatory=severity=="ERROR" || severity=="FATAL" || component=="debug";
    if(!mandatory && ((!options_.components.empty() && !match_component(component,options_.components)) || match_component(component,options_.exclude)))return;
    if(!fields.is_object())fields={{"payload",fields}};
    auto safe=redact_.apply(fields);
    if(options_.profile==DebugProfile::Debug)for(auto it=safe.begin();it!=safe.end();++it)if(it.key()=="content" || it.key()=="reasoning" || it.key()=="arguments" || it.key()=="result" || it.key()=="payload" || it.key()=="candidate")it.value()=summary(it.value());
    size_t cost=safe.dump(-1,' ',false,Json::error_handler_t::replace).size()+1024;
    // The writer owns the descriptor and may replace it during rotation.
    std::unique_lock lock(mutex_);if(failed_ || stopping_)return;
    if(queued_+cost>options_.queue_bytes) {
      if(!critical){++dropped_;return;}
      condition_.wait(lock,[&]{return failed_ || stopping_ || queue_.empty() || queued_+cost<=options_.queue_bytes;});if(failed_ || stopping_)return;
    }
    if(dropped_>reported_drops_) {
      Json dropped=context_;dropped.update({{"event","logger.events_dropped"},{"component","debug"},{"severity","WARN"},{"level","debug"},{"seq",++sequence_},{"ts",timestamp()},{"mono_ns",std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()},{"count",dropped_-reported_drops_}});
      auto n=dropped.dump().size();queue_.push_back({std::move(dropped),n,false});queued_+=n;reported_drops_=dropped_;
    }
    Json record=context_;record.update(safe);record["ts"]=timestamp();record["mono_ns"]=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();record["seq"]=++sequence_;record["level"]=debug_profile_name(minimum);record["severity"]=severity;record["component"]=component;record["event"]=event;record["thread_id"]=std::hash<std::thread::id>{}(std::this_thread::get_id());
    if(!parent_span.empty() && !record.contains("span_id"))record["span_id"]=parent_span;
    bool flush=critical && (event.ends_with("completed") || event.ends_with("committed") || event.ends_with("cancelled") || event.ends_with("failed") || event.ends_with("reset") || severity=="ERROR" || severity=="FATAL");
    queue_.push_back({std::move(record),cost,flush});queued_+=cost;condition_.notify_all();
  }catch(...){fail();}
}
void DebugLogger::observe(std::string_view event,const Json& fields) noexcept {
  // SearchContext records these events before forwarding them to the journal.
  if(event.starts_with("search.") && fields.contains("query_id") && fields.contains("event_id"))return;
  auto dot=event.find('.');auto component=std::string(event.substr(0,dot));
  DebugProfile level=DebugProfile::Trace;
  if(event.ends_with("started") || event.ends_with("completed") || event.ends_with("failed") || event.ends_with("cancelled") || event=="memory.write.committed" || event=="runtime.decision" || event=="generation.continuation" || event=="generation.retry" || event=="workspace.changed" || event=="context.compacted")level=DebugProfile::Debug;
  if(event=="model.reasoning_observed")level=DebugProfile::Forensic;
  if(event=="generation.classified" || event=="generation.output_limit" || event=="session.closed" || event=="fact.learned" || event=="fact.corrected")level=DebugProfile::Debug;
  Json payload=fields;
  if(event.ends_with("delta") || event=="tool.output" || event=="tool.execution.progress") {
    // Payload fragments are not independently safe to redact. Complete channel
    // snapshots and assembled wire streams are recorded at completion instead.
    for(auto key:{"content","reasoning","arguments","name"})if(payload.contains(key)){payload[std::string(key)+"_bytes"]=payload[key].dump().size();payload.erase(key);}
  }
  emit(component,event,payload,level,event.ends_with("failed")?"ERROR":"INFO",!event.ends_with("delta") && event!="tool.output");
}
void DebugLogger::fail() noexcept {
  {std::lock_guard lock(mutex_);if(failed_)return;failed_=true;queue_.clear();queued_=0;}
  condition_.notify_all();const char message[]="Saga logger.error: diagnostic recording disabled after an I/O or serialization failure. Agent execution continues.\n";auto ignored=::write(STDERR_FILENO,message,sizeof(message)-1);(void)ignored;
}
void DebugLogger::write_manifest(){std::lock_guard lock(mutex_);manifest_["events_written"]=written_;manifest_["events_dropped"]=dropped_;manifest_["bytes_written"]=bytes_;manifest_["sidecar_bytes"]=sidecar_bytes_;manifest_["errors"]=errors_;manifest_["end_time"]=timestamp();atomic_write(bundle_/"manifest.json",manifest_.dump(2));}
void DebugLogger::write_event(Json event,bool flush_event) {
  if(part_bytes_>=options_.rotate_bytes) {
    ::close(fd_);auto file=log_.filename().string()+"."+std::to_string(++part_);fd_=::open((bundle_/file).c_str(),O_CREAT|O_EXCL|O_APPEND|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);if(fd_<0)throw std::runtime_error("Cannot rotate diagnostic log");part_bytes_=0;manifest_["log_files"].push_back(file);
    if(options_.keep && manifest_["log_files"].size()>options_.keep){auto old=manifest_["log_files"][0].get<std::string>();fs::remove(bundle_/old);manifest_["log_files"].erase(0);}
  }
  // Sidecars contain only centrally redacted data. Hashes refer to the stored representation.
  for(auto it=event.begin();it!=event.end();++it){auto serialized=it.value().dump(-1,' ',false,Json::error_handler_t::replace);if(serialized.size()<=8192 && !(event["event"]=="prompt.snapshot" && it.key()=="payload") && !(event["event"]=="http.request.body" && it.key()=="payload") && !(event["event"]=="stream.raw_complete" && it.key()=="payload"))continue;private_dir(bundle_/"payloads");auto name="payloads/"+std::to_string(event["seq"].get<std::uint64_t>())+"-"+std::to_string(std::distance(event.begin(),it))+".json";private_file(bundle_/name,serialized);sidecar_bytes_+=serialized.size();manifest_["sidecars"].push_back(name);it.value()={{"path",name},{"sha256",debug_sha256(serialized)},{"bytes",serialized.size()},{"representation","redacted_json"}};}
  if(event["event"]=="turn.started")manifest_["turn_count"]=manifest_.value("turn_count",0)+1;
  if(event["event"]=="model.selected")manifest_["model"]=event.value("model",Json());
  auto line=event.dump(-1,' ',false,Json::error_handler_t::replace)+'\n';write_all(fd_,line);part_bytes_+=line.size();bytes_+=line.size();++written_;if(event["severity"]=="ERROR" || event["severity"]=="FATAL")++errors_;
  {std::lock_guard lock(mutex_);ring_.push_back(line);while(ring_.size()>options_.ring_events)ring_.pop_front();}
  if(flush_event){if(::fdatasync(fd_)!=0)throw std::runtime_error("Diagnostic flush failed");write_manifest();
    std::string ring;{std::lock_guard lock(mutex_);for(auto& line:ring_)ring+=line;}atomic_write(bundle_/"crash/flight-recorder.jsonl",ring);
  }
}
void DebugLogger::run() {
  try {
    // Each session repeats reproducibility metadata, including bootstrap-only recordings.
    while(true) {
      Pending pending;
      {std::unique_lock lock(mutex_);condition_.wait(lock,[&]{return stopping_ || failed_ || !queue_.empty();});if((stopping_ || failed_) && queue_.empty())break;pending=std::move(queue_.front());queue_.pop_front();queued_-=pending.bytes;busy_=true;condition_.notify_all();}
      write_event(std::move(pending.event),pending.flush);
      {std::lock_guard lock(mutex_);busy_=false;condition_.notify_all();}
    }
  }catch(...){ {std::lock_guard lock(mutex_);busy_=false;}fail();}
}
void DebugLogger::flush() noexcept {
  if(!enabled())return;
  try{std::unique_lock lock(mutex_);condition_.wait(lock,[&]{return failed_ || (queue_.empty() && !busy_);});}catch(...){}
}
void DebugLogger::crash(std::string_view reason) noexcept {
  emit("crash","crash.recorded",{{"reason",reason}},DebugProfile::Debug,"FATAL");flush();try{std::string data;{std::lock_guard lock(mutex_);for(auto &line:ring_)data+=line;}atomic_write(bundle_/"crash/flight-recorder.jsonl",data);}catch(...){}
}
void DebugLogger::sample() noexcept {
  if(!enabled(DebugProfile::Trace))return;
  auto current=std::chrono::steady_clock::now();{std::lock_guard lock(mutex_);if(current-sampled_<std::chrono::milliseconds(options_.sample_interval_ms))return;sampled_=current;}
  try {Json fields;rusage usage{};if(getrusage(RUSAGE_SELF,&usage)==0){fields["cpu_user_us"]=usage.ru_utime.tv_sec*1000000LL+usage.ru_utime.tv_usec;fields["cpu_system_us"]=usage.ru_stime.tv_sec*1000000LL+usage.ru_stime.tv_usec;}std::ifstream input("/proc/self/status");std::string key;while(input>>key){std::string value;std::getline(input,value);if(key=="VmRSS:" || key=="VmSize:" || key=="Threads:")fields[key.substr(0,key.size()-1)]=trim(value);}std::error_code ec;size_t fds=0;for(auto it=fs::directory_iterator("/proc/self/fd",ec);!ec && it!=fs::directory_iterator();it.increment(ec))++fds;fields["open_fds"]=fds;emit("process","process.sample",fields,DebugProfile::Trace,"TRACE",false);}catch(...){}
}
DebugLogger* debug_logger() noexcept{return active;}
TraceContext::TraceContext(const Json& fields):logger_(active){if(logger_){previous_=logger_->context();logger_->context(fields);}}
TraceContext::~TraceContext(){if(logger_)logger_->replace_context(previous_);}
DebugScope::DebugScope(DebugLogger* logger) noexcept:previous_(active){active=logger;}
DebugScope::~DebugScope(){active=previous_;}
void DebugScope::bind(DebugLogger* logger) noexcept{active=logger;}
void trace(std::string_view component,std::string_view event,Json fields,DebugProfile minimum,std::string_view severity,bool critical) noexcept{if(active)active->emit(component,event,std::move(fields),minimum,severity,critical);}
TraceSpan::TraceSpan(std::string_view component,std::string_view name,Json fields,DebugProfile minimum):logger_(active && active->enabled(minimum)?active:nullptr),parent_(parent_span),component_(component),name_(name),started_(std::chrono::steady_clock::now()),exceptions_(std::uncaught_exceptions()),minimum_(minimum){
  if(logger_){id_=logger_->next_span();fields["span_id"]=id_;fields["parent_span_id"]=parent_;fields["name"]=name_;logger_->emit(component_,"span.started",fields,minimum);parent_span=id_;}
}
TraceSpan::~TraceSpan(){if(logger_){parent_span=parent_;logger_->emit(component_,"span.completed",{{"span_id",id_},{"parent_span_id",parent_},{"name",name_},{"duration_ns",std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started_).count()},{"failed",std::uncaught_exceptions()>exceptions_}},minimum_,"TRACE",false);}}
Json debug_context_accounting(const Json& messages,const Json& tools) {
  Json result={{"tokenizer",{{"source","byte_estimate"},{"exact",false}}},{"messages",Json::array()},{"tools",Json::array()}};size_t total=0;Json components=Json::object();
  for(size_t i=0;i<messages.size();++i){auto serialized=messages[i].dump();auto cost=estimate_tokens(serialized);total+=cost;
    auto role=messages[i].value("role","");auto component=role=="system"?"system_persona_and_persistent_state":role=="tool"?"tool_results":"conversation_history";
    components[component]=components.value(component,size_t(0))+cost;
    result["messages"].push_back({{"index",i},{"role",messages[i].value("role","")},{"tokens",cost},{"bytes",serialized.size()},{"included",true},{"sha256",debug_sha256(serialized)}});}
  size_t tools_total=0;for(auto &tool:tools){auto cost=estimate_tokens(tool.dump());tools_total+=cost;result["tools"].push_back({{"name",tool.at("function").at("name")},{"tokens",cost}});}components["tool_schemas"]=tools_total;result["components"]=components;result["message_tokens"]=total;result["tool_schema_tokens"]=tools_total;result["total_tokens"]=total+tools_total;result["serialization_tokens"]=estimate_tokens(messages.dump())+estimate_tokens(tools.dump());return result;
}
void DebugLogger::record_context(const Json& messages,const Json& tools,Json fields) {
  auto accounting=debug_context_accounting(messages,tools);accounting.update(fields);
  auto safe_messages=redact_.apply(messages);auto safe_tools=redact_.apply(tools);
  for(size_t i=0;i<safe_messages.size();++i)accounting["messages"][i]["sha256"]=debug_sha256(safe_messages[i].dump());
  accounting["context_sha256"]=debug_sha256(safe_messages.dump()+safe_tools.dump());accounting["tool_schema_sha256"]=debug_sha256(safe_tools.dump());
  Json previous;{std::lock_guard lock(mutex_);previous=previous_context_;previous_context_=accounting;}
  if(!previous.empty()) {
    std::map<std::string,size_t> old,current;for(auto& m:previous["messages"])old[m["sha256"]]=m["tokens"];for(auto& m:accounting["messages"])current[m["sha256"]]=m["tokens"];
    Json added=Json::array(),removed=Json::array();size_t tokens_added=0,tokens_removed=0;
    for(auto& [hash,tokens]:old)if(!current.contains(hash)){removed.push_back(hash);tokens_removed+=tokens;}
    for(auto& [hash,tokens]:current)if(!old.contains(hash)){added.push_back(hash);tokens_added+=tokens;}
    Json diff={{"previous_context_id",previous.value("context_id","")},{"context_id",accounting.value("context_id","")},{"blocks_added",added},{"blocks_removed",removed},{"tokens_added",tokens_added},{"tokens_removed",tokens_removed},{"net_tokens",static_cast<Id>(tokens_added)-static_cast<Id>(tokens_removed)}};
    diff["total_tokens_before"]=previous["total_tokens"];diff["total_tokens_after"]=accounting["total_tokens"];diff["net_tokens"]=accounting["total_tokens"].get<Id>()-previous["total_tokens"].get<Id>();diff["tool_schemas_changed"]=previous["tool_schema_sha256"]!=accounting["tool_schema_sha256"];
    emit("context","context.diff",diff,DebugProfile::Trace);
    emit("context","context.rebuilt",{{"reason","next_generation"},{"old_context_id",diff["previous_context_id"]},{"new_context_id",diff["context_id"]}},DebugProfile::Trace);
  }
  emit("context","context.accounting",accounting,DebugProfile::Trace);
}
void install_debug_crash_handlers(){static_assert(std::atomic<int>::is_always_lock_free);std::call_once(emergency_init,[]{for(auto &entry:emergency_fds)entry.store(-1);});for(int signal:{SIGSEGV,SIGABRT,SIGBUS,SIGILL,SIGFPE})std::signal(signal,emergency);std::set_terminate([]{if(active)active->crash("std::terminate");std::abort();});}
}
