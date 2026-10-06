#include <saga/chat_ui.hpp>
#include <curses.h>
#include <algorithm>
#include <atomic>
#include <clocale>
#include <climits>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <sys/ioctl.h>
#include <unistd.h>

namespace saga {
std::string command_help(std::string_view query) {
  struct Help { const char* name; const char* args; const char* section; const char* description; const char* examples; };
  static constexpr Help commands[]={
    {"steer","PROMPT","Session commands","Queue instructions for the next model call in this conversation, after the current command and pending approval finish.","/steer Add tests before changing the UI"},
    {"stop","","Session commands","Stop active work and cancel queued steering. Completed edits remain. Ctrl+C or Esc also stop active work.","/stop"},
    {"new","","Session commands","Close this session, save its diary and begin a fresh conversation with the same persona.","/new"},
    {"persona","","Session commands","Save the current session and return to the persona selector.","/persona"},
    {"model","","Session commands","Close the session and configure the endpoint/model used by Saga. Memories remain intact.","/model"},
    {"exit","","Session commands","Save the session and leave Saga. The daemon retains the persona's state.","/exit"},
    {"name","[new name]","Identity","Show the persona name, or deliberately change its display name.","/name\n/name Researcher"},
    {"soul","[file]","Identity","Show SOUL.md. With a file path, replace the identity seed with that file's contents; this is a user edit.","/soul\n/soul /home/me/SOUL.md"},
    {"self","","Identity","Inspect evidence-backed self-beliefs, developed traits and relationship preferences.","/self"},
    {"memory","[query]","Memory and knowledge","Search autobiographical episodes and journals, including deep recall of previous work.","/memory the page we made\n/memory X11 DPI"},
    {"know","[query]","Memory and knowledge","Search semantic facts with their confidence and provenance; deep search can include historical facts.","/know operating system"},
    {"praxis","[query]","Memory and knowledge","Search learned procedures, their evidence and validation status.","/praxis visual testing"},
    {"artifacts","[query]","Memory and knowledge","Find files or things this persona created or modified, scoped to its own memory.","/artifacts waveform"},
    {"journal","","Memory and knowledge","Read recent autobiographical diary entries. Pending reflection may show a provisional entry.","/journal"},
    {"goals","","Work and continuity","List persistent goals and their current status.","/goals"},
    {"tasks","","Work and continuity","List tasks, including active, blocked, verifying and completed work.","/tasks"},
    {"project","[path]","Work and continuity","Inspect this project. With a path, select or create the explicitly requested workspace. Modes 1/3 ask; 2/4 approve automatically.","/project\n/project /home/me/code/example"},
    {"state","","Work and continuity","Inspect continuity: recent life, unfinished work, commitments, goals and open loops.","/state"},
    {"compact","","Work and continuity","Persist a cognitive checkpoint and handoff, then rebuild working context. Original messages, events and typed memories stay available.","/compact\n/status"},
    {"fact","JSON","Knowledge edits","Record a user-supplied fact with provenance. Required JSON fields: subject, predicate and object. Optional scope: global or project.","/fact {\"subject\":\"machine\",\"predicate\":\"OS\",\"object\":\"FreeBSD\"}"},
    {"correct","JSON","Knowledge edits","Correct a fact using the same JSON fields as /fact. Keep the old validity interval and preserve correction history.","/correct {\"subject\":\"machine\",\"predicate\":\"OS\",\"object\":\"Void Linux\"}"},
    {"permissions","[1|2|3|4]","Runtime and permissions","Set permissions for this persona. 1 asks before guarded host operations (default); 2 automatically approves guarded host. 3 asks before unrestricted host operations; 4 allows unrestricted host without approval (DANGEROUS). File edits and workspace changes also ask in modes 1/3 and run automatically in 2/4. Sandbox shell actions stay automatic. Unrestricted host removes Saga's filesystem, seccomp and no_new_privs restrictions; it can access private Saga storage and uses your OS user privileges. Aliases: ask, always, host-ask, host-always.","/permissions\n/permissions ask\n/permissions always\n/permissions 3\n/permissions 4\n/permissions host-ask\n/permissions host-always"},
    {"web","[on|off]","Runtime and permissions","Show or toggle public read-only web access for this persona. Searches and HTML/text reads run automatically, separately from host permissions. Switching off cancels active web requests; stored source excerpts remain accessible. Works while the agent is busy.","/web\n/web off\n/web on"},
    {"status","","Runtime and permissions","Show the agent's name, SOUL path, mode, task, context progress, compaction count, permission mode and memory counts. During generation, context usage is approximate.","/status"},
    {"help","[command]","Runtime and permissions","Show commands by section, or detailed help and examples for one command. /quit is an alias for /exit.","/help\n/help permissions\n/help /memory"}
  };
  auto command=lower(trim(query)); if (command.starts_with('/')) command.erase(0,1); if (command == "quit") command="exit";
  std::ostringstream out; std::string section;
  for (const auto& help : commands) {
    if (!command.empty() && command != help.name) continue;
    if (!command.empty()) { out << "**/" << help.name << (std::string_view(help.args).empty() ? "" : " ") << help.args << "**\n\n" << help.description << "\n\n### Examples:\n\n```bash\n" << help.examples << "\n```"; return out.str(); }
    if (section != help.section) { if (!section.empty()) out << '\n'; section=help.section; out << "## " << section << ":\n\n"; }
    out << "**/" << help.name << (std::string_view(help.args).empty() ? "" : " ") << help.args << "** — " << help.description << '\n';
  }
  if (!command.empty()) return "Unknown command: /"+command+". Use /help to see available commands.";
  out << "\nUse **/help command** for details and examples. **/help**, **/status**, **/permissions**, **/web** and inspection commands work while the agent is busy. Permission changes apply to subsequent host actions and edits; pending approvals still require **y/n**. Session changes, edits and **/compact** require the current turn to finish.\n\nMouse wheel or **Page Up/Down** scroll; **F2** toggles selection/copy mode; arrows edit; **Ctrl-U** clears input."; return out.str();
}
std::string format_agent_status(const Json& data) {
  auto usage=data.value("usage",Json::object()); auto used=usage.value("used_tokens",usage.value("input_tokens",0ULL)+usage.value("output_tokens",0ULL));
  auto context=data.value("context_length",0ULL); auto compact=data.value("compactions",0ULL);
  size_t filled=context ? static_cast<size_t>(std::min(1.0,static_cast<double>(used)/context)*24) : 0;
  std::ostringstream out; out << "Agent: " << data.value("name","") << "\nSOUL.md: " << data.value("soul_path","not available")
    << "\nModel: " << data.value("model","") << "\nMode: " << data.value("mode","respond") << "\nSession: " << data.value("session_id",0LL) << "\n\nContext: [";
  for (size_t i=0; i<24; ++i) out << (i<filled ? "█" : "░");
  out << "] " << (usage.value("approximate",true) ? "~" : "") << used << '/' << context;
  if (compact) out << " x" << compact;
  if(usage.contains("cached_input_tokens"))out << "\nPrompt cache: " << usage["cached_input_tokens"] << " tokens reused / " << usage.value("input_tokens",0ULL) << " input";
  out << "\nInput budget: " << data.value("input_budget",0ULL) << " · generation reserve: " << data.value("generation_reserve",0ULL) << " · safety: " << data.value("safety_margin",0ULL);
  auto permissions=data.value("permissions",Json::object()); out << "\nHost permissions: " << permissions.value("label","Ask always");
  if(data.contains("web")) {auto web=data["web"];out<<"\nPublic web: "<<(web.value("enabled",true) ? "on" : "off")<<" · "<<web.value("engine","duckduckgo")<<" · searches "<<web.value("searches",0)<<'/'<<web.value("search_limit",4)<<" · reads "<<web.value("reads",0)<<'/'<<web.value("read_limit",8);}
  if(data.contains("research")) {int unresolved=0;for(auto& q:data["research"])if(q.value("status","")!="supported")++unresolved;out<<"\nResearch questions: "<<data["research"].size()<<" · unresolved "<<unresolved;}
  auto tasks=data.value("task",Json::array()); if (!tasks.empty()) out << "\nTask: " << tasks[0].value("title","") << " (" << tasks[0].value("status","") << ')';
  if (data.contains("memory")) { out << "\n\nPersistent memory:\n"; for (auto& [key,value] : data["memory"].items()) out << "  " << key << ": " << value << '\n'; }
  return out.str();
}
std::wstring chat_wide(std::string_view text) {
  std::wstring result; std::mbstate_t state{};
  while (!text.empty()) {
    wchar_t c; auto n=std::mbrtowc(&c,text.data(),text.size(),&state);
    if (n == static_cast<size_t>(-1) || n == static_cast<size_t>(-2)) { c=L'�'; n=1; state={}; }
    if (!n) n=1;
    if (c == L'\n' || c == L'\t' || (c >= 32 && wcwidth(c) >= 0)) result += c;
    text.remove_prefix(n);
  }
  return result;
}
std::string chat_utf8(std::wstring_view text) {
  std::string result; std::mbstate_t state{}; char bytes[MB_LEN_MAX];
  for (auto c : text) { auto n=std::wcrtomb(bytes,c,&state); if (n != static_cast<size_t>(-1)) result.append(bytes,n); else state={}; }
  return result;
}
int chat_columns(std::wstring_view text) {
  int n=0; for (auto c : text) n += std::max(0,wcwidth(c)); return n;
}
static std::wstring fit(std::wstring_view text,int width) {
  std::wstring result; int used=0;
  for (auto c : text) { int n=std::max(0,wcwidth(c)); if (used+n > width) break; result += c; used += n; }
  return result;
}
std::vector<std::wstring> chat_wrap(std::wstring_view text,int width) {
  width=std::max(1,width); std::vector<std::wstring> lines(1); int used=0;
  for (auto c : text) {
    if (c == L'\n') { lines.emplace_back(); used=0; continue; }
    if (c == L'\t') c=L' ';
    int n=std::max(0,wcwidth(c));
    if (n > width) { c=L'?'; n=1; }
    if (used+n > width) { lines.emplace_back(); used=0; }
    lines.back() += c; used += n;
  }
  return lines;
}
void ChatView::event(const std::string& type,const Json& p) {
  if(!approval_id.empty() && approval.value("kind","")=="edit" && (type=="help" || type=="result" || type=="command.error" || type=="permissions.menu"))approval_hidden=true;
  if (type == "session.started") { name=p.value("name",name); model=p.value("model",model); context=p.value("context_length",context); task.clear(); agent_status["task"]=Json::array(); }
  else if(type=="generation.started"){generating=false;generated_tokens=0;phase="waiting_for_model";}
  else if(type=="turn.phase"){phase=p.value("phase","waiting_for_model");}
  else if(type=="reasoning.started"){phase="thinking";generating=true;}
  else if(type=="assistant.text.started"){phase="answering";generating=true;}
  else if(type=="tool.call.started"){phase="generating_tool";generating=true;}
  else if(type=="generation.completed" || type=="generation.interrupted"){generating=false;}
  else if(type=="commentary.completed" || type=="assistant.segment.completed") {
    auto text=p.value("content","");if(!trim(text).empty())entries.push_back({name,text,false,0,0,{},true});partial.clear();
  }
  else if (type == "assistant.delta") partial += p.value("content","");
  else if(type=="help")entries.push_back({"Saga",p.at("content").get<std::string>(),false,0,0,{},true,true});
  else if (type == "assistant.completed") { auto text=p.value("content","");if(!trim(text).empty())entries.push_back({name,text,false,0,0,{},true});else entries.push_back({"Saga","The model returned an empty response."});partial.clear(); }
  else if(type=="progress.updated") {
    auto id=p.at("message_id").get<std::string>();auto it=std::find_if(entries.begin(),entries.end(),[&](auto& entry){return entry.message_id==id;});
    if(it==entries.end()){ChatEntry entry{name,"",false,0,0,{},true};entry.message_id=id;entries.push_back(std::move(entry));it=std::prev(entries.end());}
    it->text=p.value("text","");it->streaming=!p.value("complete",false);if(p.value("interrupted",false))it->text+="\n*Interrupted.*";
  } else if(type=="operation.preparing") {operation=p.value("tool","");phase="prepare_operation";}
  else if(type=="tool.output") {
    auto id="output:"+std::to_string(p.value("run_id",0LL))+":"+p.value("stream","");
    auto it=std::find_if(entries.begin(),entries.end(),[&](auto& entry){return entry.message_id==id;});
    if(it==entries.end()){ChatEntry entry{p.value("stream","Output"),""};entry.message_id=id;entries.push_back(std::move(entry));it=std::prev(entries.end());}
    if(it->text.size()<65536) {it->text+=display_text(p.value("content",""));if(it->text.size()>65536)it->text=utf8_excerpt(it->text,65536)+"\n[Live preview truncated; captured output remains in tool history.]";}
  } else if(type=="edit.applied") {
    ChatEntry entry{display_text(p.value("path","File edit"))+" · +"+std::to_string(p.value("added",0ULL))+" / -"+std::to_string(p.value("removed",0ULL)),p.value("diff","")};
    entry.diff=true;entry.diff_event=p.value("event_id",0LL);entry.diff_loaded=!p.value("preview",false);entry.message_id=p.value("proposal_id",uuid());entries.push_back(std::move(entry));
  } else if(type=="diff.loaded") {
    for(auto& entry:entries)if(entry.diff && entry.diff_event==p.value("event_id",0LL)) {entry.text=p.value("diff","");entry.diff_loaded=true;entry.expanded=true;break;}
  } else if(type=="workspace.changed") {agent_status["project_path"]=p.value("path","");entries.push_back({"Saga","Workspace: "+p.value("path","")});}
  else if(type=="steering.delivered")entries.push_back({"Saga","Queued steering delivered. Replanning before further actions."});
  else if(type=="turn.started") {activity_started=now();phase="respond";}
  else if(type=="turn.finished") {activity_started=0;generating=false;}
  else if(type=="turn.cancelled") {
    approval_id.clear();approval=Json::object();generating=false;phase="wait";activity_started=0;
    if(!partial.empty())entries.push_back({name,std::exchange(partial,""),false,0,0,{},true});
    entries.push_back({"Saga",p.value("message","Stopped. Completed work is preserved.")});
  }
  else if (type == "tool.started") {
    auto tool=p.value("tool","");phase=tool.starts_with("web_") || tool.starts_with("research_") ? "research" : "act";generating=false;
    if (!partial.empty()) { entries.push_back({name,std::exchange(partial,""),false,0,0,{},true}); }
    auto args=p.value("arguments",Json::object());
    std::string text=name+(tool == "shell_exec" ? " is opening a terminal..." : " is using "+tool+"...");
    if(tool=="web_search"){text=name+" is searching the web...\n"+args.value("query","");}
    if(tool=="web_read" || tool=="web_fetch")text=name+" is reading a source...\n"+(args.contains("url") ? args["url"].get<std::string>() : "Source "+std::to_string(args.value("source_id",0LL)));
    if (tool != "shell_exec" && args.contains("path")) text += "\n"+args.at("path").get<std::string>();
    entries.push_back({"",text,true,p.value("run_id",0LL),now(),tool == "shell_exec" ? args.value("command","") : ""});
  } else if (type == "tool.completed" || type == "tool.failed" || type=="tool.cancelled") {
    auto run=p.value("run_id",0LL);
    for (auto it=entries.rbegin(); it!=entries.rend(); ++it) if (it->activity && it->run == run && it->started) {
      std::ostringstream s; s << std::fixed << std::setprecision(1) << (now()-it->started)/1000.0 << "s";
      auto& text=it->command.empty() ? it->text : it->command;
      text += "  "+s.str()+(type == "tool.failed" ? " · failed" : type=="tool.cancelled"?" · cancelled":""); it->started=0; break;
    }
    if(type=="tool.completed" && p.value("tool","")=="web_search") {
      auto result=p.value("result",Json::object());
      for(auto it=entries.rbegin();it!=entries.rend();++it)if(it->activity && it->run==run){
        auto query=result.value("submitted_query",result.value("query",""));
        if(!query.empty()) {
          auto first=it->text.find('\n'),elapsed=it->text.rfind("  ");
          auto suffix=elapsed==std::string::npos ? std::string() : it->text.substr(elapsed);
          auto engine=result.value("engine","");
          auto title=engine.empty() ? it->text.substr(0,first) : "Search completed through "+(engine=="duckduckgo" ? std::string("DuckDuckGo") : engine)+".";
          it->text=title+"\n"+query+suffix;
        }
        break;
      }
    }
    if(type=="tool.completed" && (p.value("tool","")=="web_read" || p.value("tool","")=="web_fetch")) {
      auto result=p.value("result",Json::object());
      for(auto it=entries.rbegin();it!=entries.rend();++it)if(it->activity && it->run==run){it->text="Visited: "+result.value("title","")+" ("+result.value("url","")+")";break;}
    }
    if(!std::any_of(entries.begin(),entries.end(),[](auto& entry){return entry.activity && entry.started;}))phase="respond";
    if (type == "tool.failed") {
      auto result=p.value("result",Json::object());
      auto error=result.value("error","");
      if (error.empty()) error=result.value("timed_out",false) ? "Tool timed out." : "Tool exited with code "+std::to_string(result.value("exit_code",-1))+".";
      auto stderr=result.value("stderr",""); if (!stderr.empty()) error += "\n"+utf8_excerpt(stderr,4096);
      entries.push_back({"Saga",error});
    }
  } else if (type == "context.usage") {
    tokens=p.value("used_tokens",p.value("input_tokens",0ULL)+p.value("output_tokens",0ULL)); context=p.value("context_length",context); approximate=p.value("approximate",true);
    generated_tokens=p.value("output_tokens",0ULL);generating=p.value("streaming",false);
    compactions=p.value("compactions",compactions); agent_status["usage"]=p; agent_status["compactions"]=compactions;
    if (p.contains("active_task")) { task=p["active_task"].empty() ? "" : p["active_task"][0].value("title",""); agent_status["task"]=p["active_task"]; }
  } else if (type == "approval.requested") {
    approval_id=p.at("approval_id"); approval_request_id=p.at("_request_id");
    approval=p.at("arguments");approval_hidden=false;approval_expanded=false;approval_scroll=0;approval_focus=0;
    if(approval.value("kind","")=="edit"){phase="approval";return;}
    auto prefix="Approve "+p.at("tool").get<std::string>()+":\n";
    auto arguments=p.at("arguments").dump(2);
    ChatEntry entry{"Approval",prefix+arguments+"\nType y or n and press Enter."};
    auto offset=chat_wide(prefix).size();
    entry.spans.push_back({0,offset-1,ChatColor::Approval,Bold});
    for(auto span:json_highlight(chat_wide(arguments))) {span.start+=offset;entry.spans.push_back(span);}
    entries.push_back(std::move(entry));
    phase="approval";
  } else if (type == "approval.resolved") {
    approval_id.clear(); approval=Json::object();phase="act";
    for (auto it=entries.rbegin(); it!=entries.rend(); ++it) if (!it->markdown && it->label == "Approval") { it->text += p.value("approved",false) ? "\nApproved." : "\nDeclined or expired."; break; }
    entries.push_back({"",p.value("approved",false) ? "Approved." : "Declined or expired.",true});
  } else if (type == "cognitive.mode") { phase=p.value("mode","respond"); agent_status["mode"]=phase;
  } else if (type == "agent.status") {
    agent_status=p; name=p.value("name",name); model=p.value("model",model); context=p.value("context_length",context); compactions=p.value("compactions",compactions);
    if (p.contains("usage") && !p["usage"].empty()) event("context.usage",p["usage"]);
  } else if (type == "permissions.menu") {
    permission_menu=true; agent_status["permissions"]=p;
    entries.push_back({"Saga","Execution permissions (this persona)\nCurrent: "+p.value("label","")+"\n\nGuarded host:\n1. Ask before guarded host operations (default).\n2. Automatically approve guarded host operations.\n\nUnrestricted host:\n3. Sandbox automatic; ask before unrestricted host operations.\n4. Allow unrestricted host operations without approval (DANGEROUS).\n\nUnrestricted host runs as your OS user without Saga isolation, including access to private Saga storage. System/container restrictions still apply.\nChoose 1, 2, 3 or 4 and press Enter. File edits/workspace changes also ask in 1/3 and are automatic in 2/4. Sandbox shell actions stay automatic."});
  } else if(type=="web.changed") {agent_status["web"]=p;
  } else if(type=="search.fallback") {entries.push_back({"",p.value("engine","")+" search unavailable ("+p.value("reason","")+"); trying another backend...",true});
  } else if(type=="research.warning") {entries.push_back({"Saga",p.value("content","")});
  } else if (type == "permissions.changed") { agent_status["permissions"]=p; permission_menu=false;
  } else if (type == "notification") entries.push_back({"Saga",p.value("description","")});
  else if (type == "command.error") entries.push_back({"Saga",p.value("message","Command failed")});
  else if (type == "error") { approval_id.clear();approval=Json::object();phase="respond";generating=false;activity_started=0; if (!partial.empty()) entries.push_back({name,std::exchange(partial,""),false,0,0,{},true}); entries.push_back({"Saga",p.value("message","Runtime error")}); }
  else if (type == "result") {
    if (p.is_object() && p.contains("name")) name=p.at("name");
    entries.push_back({"Saga",p.is_string() ? p.get<std::string>() : p.dump(2)});
  } else if (type == "session.reset") { entries.clear(); partial.clear(); task.clear(); tokens=0; compactions=0;generated_tokens=0;generating=false; }
}
std::vector<ChatSpan> shell_highlight(std::wstring_view command) {
  std::vector<ChatSpan> result; bool command_word=true;
  for (size_t i=0; i<command.size();) {
    wchar_t c=command[i]; size_t begin=i; ChatColor color=ChatColor::Default;
    if (std::iswspace(c)) { if(c==L'\n')command_word=true; ++i; continue; }
    if (c == L'\'' || c == L'"') {
      color=ChatColor::String; wchar_t quote=c; ++i;
      while (i<command.size()) { if (command[i] == L'\\' && quote == L'"' && i+1<command.size()) { i+=2; continue; } if (command[i++] == quote) break; }
    } else if (c == L'$') {
      color=ChatColor::Variable; ++i;
      if (i<command.size() && command[i] == L'{') { while (i<command.size() && command[i++] != L'}') {} }
      else while (i<command.size() && (std::iswalnum(command[i]) || command[i] == L'_')) ++i;
    } else if (c == L'#' && (i == 0 || std::iswspace(command[i-1]))) { color=ChatColor::Comment; while(i<command.size() && command[i]!=L'\n')++i; }
    else if (std::wstring_view(L";|&<>()").find(c) != std::wstring_view::npos) { color=ChatColor::Operator; ++i; if (c == L';' || c == L'|' || c == L'&') command_word=true; }
    else {
      while (i<command.size() && !std::iswspace(command[i]) && std::wstring_view(L";'\"$|&<>()").find(command[i]) == std::wstring_view::npos) ++i;
      if (i == begin) ++i;
      color=c == L'-' ? ChatColor::Option : command_word ? ChatColor::Command : ChatColor::Default;
      command_word=false;
    }
    if (color != ChatColor::Default) result.push_back({begin,i-begin,color});
  }
  return result;
}
std::vector<ChatSpan> json_highlight(std::wstring_view source) {
  std::vector<ChatSpan> result; std::string key;
  for(size_t i=0;i<source.size();) {
    auto begin=i; auto c=source[i];
    if(c==L'"') {
      ++i;
      while(i<source.size()) {if(source[i]==L'\\' && i+1<source.size()){i+=2;continue;}if(source[i++]==L'"')break;}
      auto next=i;while(next<source.size() && std::iswspace(source[next]))++next;
      bool property=next<source.size() && source[next]==L':';
      result.push_back({begin,i-begin,property?ChatColor::Link:ChatColor::String,property?Bold:0U});
      try {
        auto value=Json::parse(chat_utf8(source.substr(begin,i-begin))).get<std::string>();
        if(property) {key=value;continue;}
        if(key=="command") {
          // Keep the JSON representation exact; map decoded shell spans back
          // over escaped quotes, backslashes, newlines and Unicode sequences.
          std::wstring command;std::vector<std::pair<size_t,size_t>> positions;
          for(size_t at=begin+1;at+1<i;) {
            auto end=at+1;std::wstring decoded(1,source[at]);
            if(source[at]==L'\\') {
              end=at+2;
              if(source[at+1]==L'u') {
                end=at+6;auto code=std::stoul(std::wstring(source.substr(at+2,4)),nullptr,16);
                if(code>=0xd800 && code<=0xdbff)end+=6;
              }
              decoded=chat_wide(Json::parse("\""+chat_utf8(source.substr(at,end-at))+"\"").get<std::string>());
              if(decoded.empty())decoded=L" ";
            }
            for(auto ch:decoded){command+=ch;positions.emplace_back(at,end);}
            at=end;
          }
          result.push_back({begin+1,i-begin-2,ChatColor::Default});
          for(auto span:shell_highlight(command)) {
            auto start=positions.at(span.start).first,end=positions.at(span.start+span.length-1).second;
            result.push_back({start,end-start,span.color,span.style});
          }
        }
      } catch(const Json::exception&) { /* Incomplete JSON stays string-colored. */ }
      key.clear();
    } else if(std::wstring_view(L"{}[]:,").find(c)!=std::wstring_view::npos) {
      result.push_back({i++,1,ChatColor::Operator});if(c!=L':')key.clear();
    } else if(std::iswdigit(c) || c==L'-') {
      ++i;while(i<source.size() && std::wstring_view(L"0123456789.eE+-").find(source[i])!=std::wstring_view::npos)++i;
      result.push_back({begin,i-begin,ChatColor::Option});key.clear();
    } else if(std::iswalpha(c)) {
      ++i;while(i<source.size() && std::iswalpha(source[i]))++i;
      auto word=source.substr(begin,i-begin);
      if(word==L"true" || word==L"false" || word==L"null")result.push_back({begin,i-begin,ChatColor::Variable});
      key.clear();
    } else ++i;
  }
  return result;
}
static std::vector<ChatLine> diff_lines(std::string_view source,int width) {
  std::vector<ChatLine> lines;size_t at=0;
  while(at<source.size()) {
    auto end=source.find('\n',at);if(end==std::string_view::npos)end=source.size();auto text=chat_wide(display_text(std::string(source.substr(at,end-at))));
    auto color=text.starts_with(L"+++") || text.starts_with(L"---") ? ChatColor::Link : text.starts_with(L"+") ? ChatColor::Addition : text.starts_with(L"-") ? ChatColor::Deletion : ChatColor::Activity;
    for(auto& part:chat_wrap(text,width))lines.push_back({part,{{0,part.size(),color}}});
    at=end+1;
  }
  return lines;
}
void ChatView::activate(const std::string& target) {
  if(target=="approval:toggle"){approval_expanded=!approval_expanded;approval_scroll=0;return;}
  for(auto& entry:entries)if(entry.diff && target=="diff:"+entry.message_id){entry.expanded=!entry.expanded;return;}
}
std::vector<ChatLine> ChatView::approval_lines(int width,int height) const {
  if(approval_hidden || approval_id.empty() || approval.value("kind","")!="edit" || width<12 || height<15)return {};
  auto body=diff_lines(approval.value("diff",""),width-4);size_t room=static_cast<size_t>(std::max(1,height-13));
  if(!approval_expanded)room=std::min<size_t>(10,room);
  size_t begin=approval_expanded?std::min(approval_scroll,body.size()>room?body.size()-room:0):0,end=std::min(body.size(),begin+room);
  std::vector<ChatLine> result;
  auto add=[&](ChatLine line){auto text=L"│ "+fit(line.text,width-4);auto n=text.size();text+=std::wstring(static_cast<size_t>(std::max(0,width-2-chat_columns(text))),L' ')+L" │";ChatLine row{text,{{0,1,ChatColor::Approval},{text.size()-1,1,ChatColor::Approval}},line.target};for(auto span:line.spans){span.start+=2;span.length=std::min(span.length,n>span.start?n-span.start:0);row.spans.push_back(span);}result.push_back(std::move(row));};
  auto label=fit(L"Approve file edit",width-5);std::wstring title=L"╭─ "+label+L" "+std::wstring(static_cast<size_t>(width-chat_columns(label)-5),L'─')+L"╮";result.push_back({title,{{0,title.size(),ChatColor::Approval,Bold}}});
  auto path=fit(chat_wide(display_text(approval.value("path",""))),width-4);add({path,{{0,path.size(),ChatColor::Link,Bold}}});
  auto counts=L"+"+std::to_wstring(approval.value("added",0ULL))+L" / -"+std::to_wstring(approval.value("removed",0ULL))+L" · "+std::to_wstring(begin+1)+L"–"+std::to_wstring(end)+L"/"+std::to_wstring(body.size())+L" rows";add({counts,{{0,counts.size(),ChatColor::Activity}}});
  for(size_t i=begin;i<end;++i)add(body[i]);
  const std::string targets[]={"approval:yes","approval:no","approval:toggle"};
  const std::wstring labels[]={L"Approve",L"Reject",approval_expanded?L"See less":L"See more"};
  for(unsigned i=0;i<3;++i){auto text=(approval_focus==i?L"› [ ":L"  [ ")+labels[i]+L" ]";add({text,{{0,text.size(),ChatColor::Approval,approval_focus==i?Bold:0U}},targets[i]});}
  auto bottom=L"╰"+std::wstring(static_cast<size_t>(width-2),L'─')+L"╯";result.push_back({bottom,{{0,bottom.size(),ChatColor::Approval}}});return result;
}
static std::vector<std::wstring> word_wrap(std::wstring_view text,int width) {
  auto lines=chat_wrap(text,width);
  // Wrap prose on spaces when possible; very long words still hard-wrap.
  for (size_t i=0; i+1<lines.size(); ++i) {
    if (lines[i].empty() || chat_columns(lines[i]) < width || lines[i+1].empty() || lines[i+1][0] == L' ') continue;
    auto space=lines[i].find_last_of(L' ');
    if (space == std::wstring::npos || space < lines[i].size()/2) continue;
    auto carry=lines[i].substr(space+1); lines[i].erase(space);
    auto following=chat_wrap(carry+lines[i+1],width);
    lines[i+1]=following[0];
    if (following.size()>1) lines.insert(lines.begin()+static_cast<std::ptrdiff_t>(i+2),following.begin()+1,following.end());
  }
  return lines;
}
std::vector<ChatLine> ChatView::styled_lines(int width) const {
  std::vector<ChatLine> result; width=std::max(6,width);
  markdown_cache.resize(entries.size()+1); size_t index=0;
  auto append=[&](const ChatEntry& entry) {
    auto& cache=markdown_cache[index++];
    if (entry.activity) {
      for (auto& line : chat_wrap(chat_wide(entry.text),width-2)) { auto text=L"• "+line; result.push_back({text,{{0,text.size(),ChatColor::Activity}}}); }
      if (!entry.command.empty()) {
        auto command=chat_wide(entry.command); auto colors=shell_highlight(command); size_t offset=0;
        for (auto& line : chat_wrap(command,width-4)) {
          ChatLine row{(offset == 0 ? L"• $ " : L"•   ")+line,{{0,2,ChatColor::Activity},{2,2,ChatColor::Prompt}}};
          for (auto& span : colors) { auto begin=std::max(span.start,offset),end=std::min(span.start+span.length,offset+line.size()); if (end>begin) row.spans.push_back({4+begin-offset,end-begin,span.color}); }
          result.push_back(std::move(row)); offset += line.size(); if (offset<command.size() && command[offset] == L'\n') ++offset;
        }
      }
    } else {
      auto color=entry.markdown && !entry.help ? ChatColor::Default : entry.label == "Approval" ? ChatColor::Approval : entry.label == "Saga" ? ChatColor::Saga : ChatColor::Default;
      auto label=fit(chat_wide(entry.label),width-5);
      auto top=L"╭─ "+label+L" "+std::wstring(static_cast<size_t>(width-chat_columns(label)-5),L'─')+L"╮";
      result.push_back({top,{{0,top.size(),color}}});
      std::vector<ChatLine> literal;
      if(entry.diff) {
        literal=diff_lines(entry.text,width-4);auto total=literal.size();if(!entry.expanded && total>10)literal.resize(10);
        auto label=entry.expanded ? L"See less" : L"See more";auto text=std::wstring(L"[ ")+label+L" ] · "+(entry.diff_loaded?std::to_wstring(total)+L" rows":L"full diff on expansion");
        text=fit(text,width-4);literal.push_back({text,{{0,text.size(),ChatColor::Link,Bold}},"diff:"+entry.message_id});
      } else if (entry.markdown) {
        bool streaming=entry.streaming || index>entries.size();
        if(cache.text != entry.text || cache.width != width-4 || cache.streaming!=streaming) { cache.lines=markdown_lines(entry.text,width-4,streaming); cache.text=entry.text; cache.width=width-4; cache.streaming=streaming; }
      } else if(!entry.spans.empty()) {
        auto body=chat_wide(entry.text);size_t offset=0;
        for(auto& text:chat_wrap(body,width-4)) {
          ChatLine line{text,{}};
          for(auto span:entry.spans){auto begin=std::max(span.start,offset),end=std::min(span.start+span.length,offset+text.size());if(end>begin)line.spans.push_back({begin-offset,end-begin,span.color,span.style});}
          literal.push_back(std::move(line));offset+=text.size();if(offset<body.size() && body[offset]==L'\n')++offset;
        }
      } else for(auto& line:word_wrap(chat_wide(entry.text),width-4)) literal.push_back({line,{}});
      for (auto& line : entry.markdown ? cache.lines : literal) {
        auto text=L"│ "+line.text+std::wstring(static_cast<size_t>(std::max(0,width-4-chat_columns(line.text))),L' ')+L" │";
        ChatLine row{text,{{0,1,color},{text.size()-1,1,color}},line.target};
        if(entry.help)row.spans.push_back({2,line.text.size(),ChatColor::Activity});
        for(auto span:line.spans){span.start+=2;if(entry.help && span.color==ChatColor::Default && (span.style&Bold))span.color=ChatColor::Link;row.spans.push_back(span);}result.push_back(std::move(row));
      }
      auto bottom=L"╰"+std::wstring(static_cast<size_t>(width-2),L'─')+L"╯"; result.push_back({bottom,{{0,bottom.size(),color}}});
    }
    result.push_back({L"",{}});
  };
  for (auto& entry : entries) append(entry);
  if (!partial.empty()) append({name,partial,false,0,0,{},true});
  return result;
}
std::vector<std::wstring> ChatView::lines(int width) const {
  std::vector<std::wstring> result; for (auto& line : styled_lines(width)) result.push_back(std::move(line.text)); return result;
}
std::wstring ChatView::status(int width) const {
  auto id=chat_wide(model); int model_width=std::min(35,std::max(8,width/3));
  if (chat_columns(id) > model_width) id=fit(id,model_width-1)+L"…";
  std::ostringstream usage; usage << (approximate ? "~" : "") << std::fixed << std::setprecision(1) << tokens/1000.0 << "K/" << context/1000.0 << "K";
  auto text=id+L" │ "+chat_wide(usage.str());
  if(generating)text+=L" │ +"+std::wstring(approximate?L"~":L"")+std::to_wstring(generated_tokens);
  if (compactions) text += L" x"+std::to_wstring(compactions);
  if (!task.empty()) text += L" │ "+chat_wide(task);
  return fit(text,width);
}
std::string ChatView::report() const {
  auto data=agent_status; data["name"]=name; data["model"]=model; data["context_length"]=context; data["compactions"]=compactions; data["mode"]=phase;
  data["usage"]=agent_status.value("usage",Json::object());data["usage"]["used_tokens"]=tokens;data["usage"]["approximate"]=approximate;
  return format_agent_status(data);
}
std::wstring ChatView::activity(bool busy) const {
  if(approval_hidden && !approval_id.empty())return L"Approval pending · Tab returns to diff · y/n decides · Esc stops";
  if(!approval_id.empty())return L"Awaiting approval: y / n · Tab/Enter selects · Esc stops";
  if(!busy && !activity_started)return {};
  if(phase=="prepare_operation")return chat_wide(name+" is preparing "+operation+"...");
  if(phase=="waiting_for_model")return chat_wide(name+" is waiting for the model...");
  if(phase=="thinking")return chat_wide(name+" is thinking...");
  if(phase=="generating_tool")return chat_wide(name+" is generating a tool call...");
  if(generating)return chat_wide(name+(generated_tokens ? " is typing..." : " is preparing context..."));
  return chat_wide(name+(phase=="research" ? " is researching..." : phase=="act" ? " is running a tool..." : phase=="verify" ? " is verifying..." : phase=="reflect" ? " is saving memory..." : " is preparing context..."));
}
namespace {
struct Terminal {
  SCREEN* screen;
  void mouse(bool enabled){mousemask(enabled ? BUTTON4_PRESSED | BUTTON5_PRESSED | BUTTON1_CLICKED : 0,nullptr);}
  Terminal() : screen(nullptr) {
    use_env(false);use_tioctl(true);screen=newterm(nullptr,stdout,stdin);
    if (!screen) throw std::runtime_error("Cannot initialize ncurses; check TERM or use --no-tui");
    raw(); noecho(); keypad(stdscr,true); wtimeout(stdscr,50); curs_set(1);
    mouse(true);
    if (has_colors()) {
      start_color(); use_default_colors();
      const short rich[]={-1,220,240,250,67,114,180,81,176,67,245,73,250,73,110,73,114,174};
      const short basic[]={-1,3,7,7,4,2,3,6,5,4,7,6,7,6,4,6,2,1};
      for (short i=1; i<=static_cast<short>(ChatColor::Deletion); ++i) {
        auto c=static_cast<ChatColor>(i); bool background=c==ChatColor::InlineCode || c==ChatColor::CodeFocus;
        init_pair(i,COLORS >= 256 ? rich[i] : basic[i],background ? (COLORS>=256?236:0) : -1);
      }
    }
  }
  ~Terminal() { mouse(false); endwin(); delscreen(screen); }
};
void draw(int row,const ChatLine& line,int width) {
  auto text=fit(line.text,width); wmove(stdscr,row,0);
  for (size_t i=0; i<text.size();) {
    auto style_at=[&](size_t at){ChatColor color=ChatColor::Default;unsigned flags=0;for(auto& span:line.spans)if(at>=span.start && at<span.start+span.length){color=span.color;flags|=span.style;}return std::pair{color,flags};};
    auto [color,flags]=style_at(i);
    size_t end=i+1;
    while (end<text.size() && style_at(end)==std::pair{color,flags}) ++end;
    unsigned attributes=0;if(flags&Bold)attributes|=A_BOLD;if(flags&Underline)attributes|=A_UNDERLINE;
#ifdef A_ITALIC
    if(flags&Italic)attributes|=A_ITALIC;
#else
    if(flags&Italic)attributes|=A_UNDERLINE;
#endif
    wattr_set(stdscr,attributes,has_colors()?static_cast<short>(color):0,nullptr);
    auto segment=std::wstring(std::wstring_view(text).substr(i,end-i));
    if(flags&Strike){std::wstring struck;for(auto c:segment){struck+=c;if(c!=L' ' && wcwidth(c)>0)struck+=L'\u0336';}segment=std::move(struck);}
    auto bytes=chat_utf8(segment); waddnstr(stdscr,bytes.data(),static_cast<int>(bytes.size())); i=end;
  }
  wattr_set(stdscr,0,0,nullptr);
}
void draw(int row,std::wstring_view text,int width) { draw(row,ChatLine{std::wstring(text),{}},width); }
}
ChatAction chat_ui(ChatView view,const std::function<void(const std::string&,Emit)>& submit,
                  const std::function<std::optional<Json>()>& receive,const std::function<void(const Json&)>& approve,
                  const std::function<void(const std::string&,Emit)>& live_submit) {
  Terminal terminal;
  std::mutex mutex; std::vector<std::pair<std::string,Json>> pending;size_t output_bytes=0;bool output_overflow=false;
  std::atomic_bool busy=false;
  Emit enqueue=[&](const std::string& type,const Json& p){ std::lock_guard lock(mutex);
    if(type=="edit.applied" && p.contains("event_id")) {
      auto preview=p;auto diff=preview.value("diff","");size_t end=0;
      for(int line=0;line<10 && end<diff.size();++line){auto next=diff.find('\n',end);end=next==std::string::npos?diff.size():next+1;}
      preview["diff"]=utf8_excerpt(diff.substr(0,end),16384);preview["preview"]=true;pending.emplace_back(type,std::move(preview));return;
    }
    if(type=="tool.completed" || type=="tool.failed") {
      auto summary=p;if(summary.contains("result") && summary["result"].is_object())for(auto key:{"stdout","stderr","content"})if(summary["result"].contains(key) && summary["result"][key].is_string())summary["result"][key]=utf8_excerpt(summary["result"][key].get<std::string>(),4096);
      pending.emplace_back(type,std::move(summary));return;
    }
    if(type=="tool.started") {
      auto summary=p;auto args=p.value("arguments",Json::object());summary["arguments"]=Json::object();
      for(auto key:{"path","command","query","url","source_id"})if(args.contains(key)){if(args[key].is_string())summary["arguments"][key]=utf8_excerpt(args[key].get<std::string>(),16384);else summary["arguments"][key]=args[key];}
      pending.emplace_back(type,std::move(summary));return;
    }
    if(type=="assistant.delta") {
      auto text=p.value("content","");
      if(output_bytes+text.size()>4*1024*1024)return;
      output_bytes+=text.size();
      if(!pending.empty() && pending.back().first==type){pending.back().second["content"]=pending.back().second.value("content","")+text;return;}
    }
    if(type=="tool.output") {
      auto text=p.value("content","");
      if(output_bytes+text.size()>512*1024){if(!output_overflow){pending.emplace_back("notification",Json{{"description","Live output display truncated while rendering was paused; captured output remains in tool history."}});output_overflow=true;}return;}
      output_bytes+=text.size();
      for(auto it=pending.rbegin();it!=pending.rend();++it)if(it->first==type && it->second.value("run_id",0LL)==p.value("run_id",0LL) && it->second.value("stream","")==p.value("stream","")){it->second["content"]=it->second.value("content","")+text;return;}
    }
    if(type=="context.usage" || type=="progress.updated" || type=="operation.preparing") {
      for(auto it=pending.rbegin();it!=pending.rend();++it)if(it->first==type && (type!="progress.updated" || it->second.value("message_id","")==p.value("message_id",""))) {it->second=p;return;}
    }
    pending.emplace_back(type,p); };
  std::jthread worker;
  std::jthread idle([&](std::stop_token stop){
    while (!stop.stop_requested()) {
      if (busy) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); continue; }
      try { if (auto frame=receive()) { auto type=frame->value("type",""); auto payload=frame->value("payload",Json::object()); if (type == "approval.requested") payload["_request_id"]=frame->value("request_id",""); enqueue(type,payload); } }
      catch (const std::exception& e) { enqueue("error",{{"message",e.what()}}); return; }
    }
  });
  std::wstring input; size_t cursor=0,scroll=0;
  ChatAction action=ChatAction::Continue;
  bool selecting=false,redraw=true;
  int previous_height=0,previous_width=0;
  std::vector<std::pair<int,std::string>> targets;
  auto activate=[&](const std::string& target){
    for(auto& entry:view.entries)if(entry.diff && target=="diff:"+entry.message_id && !entry.diff_loaded && entry.diff_event) {
      try{if(live_submit)live_submit("/diff "+std::to_string(entry.diff_event),enqueue);else submit("/diff "+std::to_string(entry.diff_event),enqueue);}catch(const std::exception& e){view.event("command.error",{{"message",e.what()}});}return;
    }
    view.activate(target);
  };
  auto respond=[&](bool accepted){try{approve({{"type","approval"},{"request_id",view.approval_request_id},{"payload",{{"approval_id",view.approval_id},{"approved",accepted}}}});view.approval_id.clear();view.approval=Json::object();view.phase="act";input.clear();cursor=0;}catch(const std::exception& e){view.event("error",{{"message",e.what()}});}};
  while (action == ChatAction::Continue) {
    std::vector<std::pair<std::string,Json>> events;
    if(!selecting){std::lock_guard lock(mutex);events.swap(pending);output_bytes=0;output_overflow=false;}
    for (auto& [type,p] : events) view.event(type,p);
    winsize size{};if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&size)==0 && size.ws_row && size.ws_col && (getmaxy(stdscr)!=size.ws_row || getmaxx(stdscr)!=size.ws_col))resizeterm(size.ws_row,size.ws_col);
    int height=getmaxy(stdscr),width=getmaxx(stdscr);bool working=busy || view.activity_started;
    if(height!=previous_height || width!=previous_width){clearok(stdscr,true);previous_height=height;previous_width=width;redraw=true;}
    if(!selecting || redraw) {
      werase(stdscr);targets.clear();
      if (height >= 7 && width >= 12) {
      auto lines=view.styled_lines(width); size_t room=static_cast<size_t>(height-5);
      scroll=std::min(scroll,lines.size() > room ? lines.size()-room : 0);
      size_t end=lines.size()-scroll,begin=end > room ? end-room : 0;
      for (size_t i=begin; i<end; ++i) {draw(static_cast<int>(i-begin),lines[i],width);if(!lines[i].target.empty())targets.emplace_back(static_cast<int>(i-begin),lines[i].target);}
      auto popup=view.approval_lines(width,height);
      if(!popup.empty() && !selecting){targets.clear();int top=std::max(0,(height-5-static_cast<int>(popup.size()))/2);for(size_t i=0;i<popup.size();++i){draw(top+static_cast<int>(i),popup[i],width);if(!popup[i].target.empty())targets.emplace_back(top+static_cast<int>(i),popup[i].target);}}
      draw(height-5,selecting ? L"Select text · copy with terminal shortcut · F2/Esc returns" : view.activity(working)+(view.activity_started?L" · "+std::to_wstring((now()-view.activity_started)/1000)+L"s":L""),width); draw(height-4,view.status(width),width);
      draw(height-3,std::wstring(static_cast<size_t>(width),L'─'),width);
      size_t start=0;
      while (start < cursor && chat_columns(std::wstring_view(input).substr(start,cursor-start)) >= width-3) ++start;
      auto visible=fit(std::wstring_view(input).substr(start),width-2);
      draw(height-2,L"❯ "+(input.empty() ? fit(chat_wide("chat with "+view.name),width-2) : visible),width);
      draw(height-1,std::wstring(static_cast<size_t>(width),L'─'),width);
      wmove(stdscr,height-2,2+chat_columns(std::wstring_view(input).substr(start,cursor-start)));
      } else { draw(0,L"Resize terminal",width); }
      wnoutrefresh(stdscr);doupdate();redraw=false;
    }
    wint_t key=0; int kind=wget_wch(stdscr,&key);
    if (kind == ERR) continue;
    if((busy || view.activity_started) && kind!=KEY_CODE_YES && (key==3 || key==27)) {
      if(selecting){selecting=false;terminal.mouse(true);redraw=true;}
      try {if(live_submit)live_submit("/stop",enqueue);}catch(const std::exception& e){view.event("command.error",{{"message",e.what()}});}
      continue;
    }
    if((kind==KEY_CODE_YES && key==KEY_F(2)) || (selecting && key==27)) {
      selecting=!selecting;terminal.mouse(!selecting);redraw=true;continue;
    }
    if(selecting && kind!=KEY_CODE_YES)continue;
    if (kind == KEY_CODE_YES) {
      if (key == KEY_RESIZE) {clearok(stdscr,true);redraw=true;continue;}
      if (key == KEY_MOUSE) {
        MEVENT mouse{};
        if (getmouse(&mouse) != ERR) {
          if(!selecting && (mouse.bstate & BUTTON1_CLICKED))for(auto& [row,target]:targets)if(row==mouse.y){if(target=="approval:yes")respond(true);else if(target=="approval:no")respond(false);else activate(target);redraw=true;break;}
          bool popup=!view.approval_hidden && !view.approval_id.empty() && view.approval.value("kind","")=="edit";
          if(popup) {if(view.approval_expanded){if(mouse.bstate & BUTTON4_PRESSED)view.approval_scroll=view.approval_scroll>3?view.approval_scroll-3:0;if(mouse.bstate & BUTTON5_PRESSED)view.approval_scroll+=3;}}
          else {if(mouse.bstate & BUTTON4_PRESSED)scroll+=3;if(mouse.bstate & BUTTON5_PRESSED)scroll=scroll>3?scroll-3:0;}
          redraw=true;
        }
        continue;
      }
      if((key==KEY_PPAGE || key==KEY_NPAGE) && !view.approval_hidden && !view.approval_id.empty() && view.approval.value("kind","")=="edit") {view.approval_expanded=true;if(key==KEY_NPAGE)view.approval_scroll+=static_cast<size_t>(std::max(1,height-13));else view.approval_scroll=view.approval_scroll>static_cast<size_t>(std::max(1,height-13))?view.approval_scroll-static_cast<size_t>(std::max(1,height-13)):0;redraw=true;continue;}
      if (key == KEY_PPAGE) { scroll += static_cast<size_t>(std::max(1,height-6));redraw=true;continue; }
      if (key == KEY_NPAGE) { size_t page=static_cast<size_t>(std::max(1,height-6)); scroll=scroll > page ? scroll-page : 0;redraw=true;continue; }
      if(selecting)continue;
      if (key == KEY_LEFT) { if (cursor) --cursor; continue; }
      if (key == KEY_RIGHT) { if (cursor < input.size()) ++cursor; continue; }
      if (key == KEY_HOME) { cursor=0; continue; }
      if (key == KEY_END) { cursor=input.size(); continue; }
      if (key == KEY_DC) { if (cursor < input.size()) input.erase(cursor,1); continue; }
      if (key == KEY_BACKSPACE) key=127;
      else if (key == KEY_ENTER) key=10;
      else continue;
    }
    if (key == 27 || key == 3 || (key == 4 && input.empty())) {
      if (!view.approval_id.empty()) { try { approve({{"type","approval"},{"request_id",view.approval_request_id},{"payload",{{"approval_id",view.approval_id},{"approved",false}}}}); view.approval_id.clear(); view.phase="act"; input.clear(); cursor=0; } catch(const std::exception& e) {view.event("error",{{"message",e.what()}});} }
      else if (!working) action=ChatAction::Exit;
      continue;
    }
    if(key==9 && !view.approval_id.empty() && view.approval.value("kind","")=="edit") {if(view.approval_hidden)view.approval_hidden=false;else view.approval_focus=(view.approval_focus+1)%3;redraw=true;continue;}
    if((key==10 || key==13) && input.empty() && !view.approval_id.empty() && view.approval.value("kind","")=="edit") {if(view.approval_hidden)view.approval_hidden=false;else if(view.approval_focus==2)view.activate("approval:toggle");else respond(view.approval_focus==0);redraw=true;continue;}
    if (key == 127 || key == 8) { if (cursor) input.erase(--cursor,1); continue; }
    if (key == 21) { input.clear(); cursor=0; continue; }
    if (key != 10 && key != 13) { if (key >= 32 && input.size() < 65536) input.insert(cursor++,1,static_cast<wchar_t>(key)); continue; }
    auto text=trim(chat_utf8(input)); if (text.empty()) continue;
    if (text == "/help" || text.starts_with("/help ")) { view.entries.push_back({"user",text}); view.event("help",{{"content",command_help(text.size()>5 ? text.substr(6) : "")}}); input.clear(); cursor=0; scroll=0; continue; }
    if (view.permission_menu && (text == "1" || text == "2" || text == "3" || text == "4")) text="/permissions "+text;
    if (working && text.starts_with('/') && live_submit) {
      view.entries.push_back({"user",text}); input.clear(); cursor=0; scroll=0;
      try { live_submit(text,enqueue); }
      catch (const std::exception& e) { view.event("command.error",{{"message",e.what()}}); }
      continue;
    }
    if (text == "/status" && busy) { view.entries.push_back({"user",text}); view.entries.push_back({"Saga",view.report()}); input.clear(); cursor=0; scroll=0; continue; }
    if (!view.approval_id.empty()) {
      if (lower(text) == "y" || lower(text) == "n") {
        try { approve({{"type","approval"},{"request_id",view.approval_request_id},{"payload",{{"approval_id",view.approval_id},{"approved",lower(text) == "y"}}}}); view.approval_id.clear(); view.phase="act"; input.clear(); cursor=0; }
        catch (const std::exception& e) { view.event("error",{{"message",e.what()}}); }
      }
      continue;
    }
    if (working) continue;
    input.clear(); cursor=0; scroll=0;
    if (text == "/exit" || text == "/quit") { action=ChatAction::Exit; continue; }
    if (text == "/persona") { action=ChatAction::Selector; continue; }
    if (text == "/model") { action=ChatAction::ModelSetup; continue; }
    view.entries.push_back({"user",text}); busy=true;
    worker=std::jthread([&,text]{
      try { submit(text,enqueue); }
      catch (const std::exception& e) { enqueue("error",{{"message",e.what()}}); }
      busy=false;
    });
  }
  idle.request_stop(); idle.join(); if (worker.joinable()) worker.join();
  return action;
}
}
