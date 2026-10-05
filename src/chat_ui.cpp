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
    {"project","","Work and continuity","Inspect this project's last work, artifacts, decisions and open questions.","/project"},
    {"state","","Work and continuity","Inspect continuity: recent life, unfinished work, commitments, goals and open loops.","/state"},
    {"compact","","Work and continuity","Persist a cognitive checkpoint and handoff, then rebuild working context. Original messages, events and typed memories stay available.","/compact\n/status"},
    {"fact","JSON","Knowledge edits","Record a user-supplied fact with provenance. Required JSON fields: subject, predicate and object. Optional scope: global or project.","/fact {\"subject\":\"machine\",\"predicate\":\"OS\",\"object\":\"FreeBSD\"}"},
    {"correct","JSON","Knowledge edits","Correct a fact using the same JSON fields as /fact. Keep the old validity interval and preserve correction history.","/correct {\"subject\":\"machine\",\"predicate\":\"OS\",\"object\":\"Void Linux\"}"},
    {"permissions","[1|2|3|4]","Runtime and permissions","Set permissions for this persona. 1 asks before guarded host operations (default); 2 automatically approves guarded host. 3 asks before unrestricted host operations; 4 allows unrestricted host without approval (DANGEROUS). Sandbox actions stay automatic. Unrestricted host removes Saga's filesystem, seccomp and no_new_privs restrictions; it can access private Saga storage and uses your OS user privileges. Aliases: ask, always, host-ask, host-always.","/permissions\n/permissions ask\n/permissions always\n/permissions 3\n/permissions 4\n/permissions host-ask\n/permissions host-always"},
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
  out << "\nUse **/help command** for details and examples. Mouse wheel or **Page Up/Down** scroll; **F2** toggles selection/copy mode; arrows edit; **Ctrl-U** clears input."; return out.str();
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
  if (type == "session.started") { name=p.value("name",name); model=p.value("model",model); context=p.value("context_length",context); }
  else if (type == "assistant.delta") partial += p.value("content","");
  else if(type=="help")entries.push_back({"Saga",p.at("content").get<std::string>(),false,0,0,{},true,true});
  else if (type == "assistant.completed") { entries.push_back({name,p.value("content",""),false,0,0,{},true}); partial.clear(); }
  else if (type == "tool.started") {
    if (!partial.empty()) { entries.push_back({name,std::exchange(partial,""),false,0,0,{},true}); }
    auto tool=p.value("tool",""); auto args=p.value("arguments",Json::object());
    std::string text=name+(tool == "shell_exec" ? " is opening a terminal..." : " is using "+tool+"...");
    if (tool != "shell_exec" && args.contains("path")) text += "\n"+args.at("path").get<std::string>();
    entries.push_back({"",text,true,p.value("run_id",0LL),now(),tool == "shell_exec" ? args.value("command","") : ""});
  } else if (type == "tool.completed" || type == "tool.failed") {
    auto run=p.value("run_id",0LL);
    for (auto it=entries.rbegin(); it!=entries.rend(); ++it) if (it->activity && it->run == run && it->started) {
      std::ostringstream s; s << std::fixed << std::setprecision(1) << (now()-it->started)/1000.0 << "s";
      auto& text=it->command.empty() ? it->text : it->command;
      text += "  "+s.str()+(type == "tool.failed" ? " · failed" : ""); it->started=0; break;
    }
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
    entries.push_back({"Approval","Approve "+p.at("tool").get<std::string>()+":\n"+p.at("arguments").dump(2)+"\nType y or n and press Enter."});
    phase="approval";
  } else if (type == "approval.resolved") {
    approval_id.clear(); phase="act";
    for (auto it=entries.rbegin(); it!=entries.rend(); ++it) if (!it->markdown && it->label == "Approval") { it->text += p.value("approved",false) ? "\nApproved." : "\nDeclined or expired."; break; }
    entries.push_back({"",p.value("approved",false) ? "Approved." : "Declined or expired.",true});
  } else if (type == "cognitive.mode") { phase=p.value("mode","respond"); agent_status["mode"]=phase;
  } else if (type == "agent.status") {
    agent_status=p; name=p.value("name",name); model=p.value("model",model); context=p.value("context_length",context); compactions=p.value("compactions",compactions);
    if (p.contains("usage") && !p["usage"].empty()) event("context.usage",p["usage"]);
  } else if (type == "permissions.menu") {
    permission_menu=true; agent_status["permissions"]=p;
    entries.push_back({"Saga","Execution permissions (this persona)\nCurrent: "+p.value("label","")+"\n\nGuarded host:\n1. Ask before guarded host operations (default).\n2. Automatically approve guarded host operations.\n\nUnrestricted host:\n3. Sandbox automatic; ask before unrestricted host operations.\n4. Allow unrestricted host operations without approval (DANGEROUS).\n\nUnrestricted host runs as your OS user without Saga isolation, including access to private Saga storage. System/container restrictions still apply.\nChoose 1, 2, 3 or 4 and press Enter. Sandbox actions stay automatic."});
  } else if (type == "permissions.changed") { agent_status["permissions"]=p; permission_menu=false;
  } else if (type == "notification") entries.push_back({"Saga",p.value("description","")});
  else if (type == "error") { approval_id.clear(); phase="respond";generating=false; if (!partial.empty()) entries.push_back({name,std::exchange(partial,""),false,0,0,{},true}); entries.push_back({"Saga",p.value("message","Runtime error")}); }
  else if (type == "result") {
    if (p.is_object() && p.contains("name")) name=p.at("name");
    entries.push_back({"Saga",p.is_string() ? p.get<std::string>() : p.dump(2)});
  } else if (type == "session.reset") { entries.clear(); partial.clear(); task.clear(); tokens=0; compactions=0;generated_tokens=0;generating=false; }
}
std::vector<ChatSpan> shell_highlight(std::wstring_view command) {
  std::vector<ChatSpan> result; bool command_word=true;
  for (size_t i=0; i<command.size();) {
    wchar_t c=command[i]; size_t begin=i; ChatColor color=ChatColor::Default;
    if (std::iswspace(c)) { ++i; continue; }
    if (c == L'\'' || c == L'"') {
      color=ChatColor::String; wchar_t quote=c; ++i;
      while (i<command.size()) { if (command[i] == L'\\' && quote == L'"' && i+1<command.size()) { i+=2; continue; } if (command[i++] == quote) break; }
    } else if (c == L'$') {
      color=ChatColor::Variable; ++i;
      if (i<command.size() && command[i] == L'{') { while (i<command.size() && command[i++] != L'}') {} }
      else while (i<command.size() && (std::iswalnum(command[i]) || command[i] == L'_')) ++i;
    } else if (c == L'#' && (i == 0 || std::iswspace(command[i-1]))) { color=ChatColor::Comment; i=command.size(); }
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
      for (auto& line : chat_wrap(chat_wide(entry.text),width-2)) { auto text=L"┊ "+line; result.push_back({text,{{0,text.size(),ChatColor::Activity}}}); }
      if (!entry.command.empty()) {
        auto command=chat_wide(entry.command); auto colors=shell_highlight(command); size_t offset=0;
        for (auto& line : chat_wrap(command,width-4)) {
          ChatLine row{(offset == 0 ? L"┊ $ " : L"┊   ")+line,{{0,2,ChatColor::Activity},{2,2,ChatColor::Prompt}}};
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
      if (entry.markdown) {
        bool streaming=index>entries.size();
        if(cache.text != entry.text || cache.width != width-4 || cache.streaming!=streaming) { cache.lines=markdown_lines(entry.text,width-4,streaming); cache.text=entry.text; cache.width=width-4; cache.streaming=streaming; }
      } else for(auto& line:word_wrap(chat_wide(entry.text),width-4)) literal.push_back({line,{}});
      for (auto& line : entry.markdown ? cache.lines : literal) {
        auto text=L"│ "+line.text+std::wstring(static_cast<size_t>(std::max(0,width-4-chat_columns(line.text))),L' ')+L" │";
        ChatLine row{text,{{0,1,color},{text.size()-1,1,color}}};
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
  if(!approval_id.empty())return L"Awaiting approval: y / n · Esc declines";
  if(!busy)return {};
  if(generating)return chat_wide(name+(generated_tokens ? " is typing..." : " is preparing context..."));
  return chat_wide(name+(phase=="act" ? " is running a tool..." : phase=="verify" ? " is verifying..." : phase=="reflect" ? " is saving memory..." : " is preparing context..."));
}
namespace {
struct Terminal {
  SCREEN* screen;
  void mouse(bool enabled){mousemask(enabled ? BUTTON4_PRESSED | BUTTON5_PRESSED : 0,nullptr);}
  Terminal() : screen(nullptr) {
    use_env(false);use_tioctl(true);screen=newterm(nullptr,stdout,stdin);
    if (!screen) throw std::runtime_error("Cannot initialize ncurses; check TERM or use --no-tui");
    raw(); noecho(); keypad(stdscr,true); wtimeout(stdscr,50); curs_set(1);
    mouse(true);
    if (has_colors()) {
      start_color(); use_default_colors();
      const short rich[]={-1,220,240,250,67,114,180,81,176,67,245,73,250,73,110,73};
      const short basic[]={-1,3,7,7,4,2,3,6,5,4,7,6,7,6,4,6};
      for (short i=1; i<=static_cast<short>(ChatColor::CodeFocus); ++i) {
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
                  const std::function<std::optional<Json>()>& receive,const std::function<void(const Json&)>& approve) {
  Terminal terminal;
  std::mutex mutex; std::vector<std::pair<std::string,Json>> pending;
  std::atomic_bool busy=false;
  Emit enqueue=[&](const std::string& type,const Json& p){ std::lock_guard lock(mutex); pending.emplace_back(type,p); };
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
  while (action == ChatAction::Continue) {
    std::vector<std::pair<std::string,Json>> events;
    if(!selecting){std::lock_guard lock(mutex);events.swap(pending);}
    for (auto& [type,p] : events) view.event(type,p);
    winsize size{};if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&size)==0 && size.ws_row && size.ws_col && (getmaxy(stdscr)!=size.ws_row || getmaxx(stdscr)!=size.ws_col))resizeterm(size.ws_row,size.ws_col);
    int height=getmaxy(stdscr),width=getmaxx(stdscr);
    if(height!=previous_height || width!=previous_width){clearok(stdscr,true);previous_height=height;previous_width=width;redraw=true;}
    if(!selecting || redraw) {
      werase(stdscr);
      if (height >= 7 && width >= 12) {
      auto lines=view.styled_lines(width); size_t room=static_cast<size_t>(height-5);
      scroll=std::min(scroll,lines.size() > room ? lines.size()-room : 0);
      size_t end=lines.size()-scroll,begin=end > room ? end-room : 0;
      for (size_t i=begin; i<end; ++i) draw(static_cast<int>(i-begin),lines[i],width);
      draw(height-5,selecting ? L"Select text · copy with terminal shortcut · F2/Esc returns" : view.activity(busy),width); draw(height-4,view.status(width),width);
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
    if((kind==KEY_CODE_YES && key==KEY_F(2)) || (selecting && key==27)) {
      selecting=!selecting;terminal.mouse(!selecting);redraw=true;continue;
    }
    if(selecting && kind!=KEY_CODE_YES)continue;
    if (kind == KEY_CODE_YES) {
      if (key == KEY_RESIZE) {clearok(stdscr,true);redraw=true;continue;}
      if (key == KEY_MOUSE) {
        MEVENT mouse{};
        if (getmouse(&mouse) != ERR) {
          if (mouse.bstate & BUTTON4_PRESSED) scroll += 3;
          if (mouse.bstate & BUTTON5_PRESSED) scroll=scroll > 3 ? scroll-3 : 0;
        }
        continue;
      }
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
      else if (!busy) action=ChatAction::Exit;
      continue;
    }
    if (key == 127 || key == 8) { if (cursor) input.erase(--cursor,1); continue; }
    if (key == 21) { input.clear(); cursor=0; continue; }
    if (key != 10 && key != 13) { if (key >= 32 && input.size() < 65536) input.insert(cursor++,1,static_cast<wchar_t>(key)); continue; }
    auto text=trim(chat_utf8(input)); if (text.empty()) continue;
    if (text == "/help" || text.starts_with("/help ")) { view.entries.push_back({"user",text}); view.event("help",{{"content",command_help(text.size()>5 ? text.substr(6) : "")}}); input.clear(); cursor=0; scroll=0; continue; }
    if (text == "/status" && busy) { view.entries.push_back({"user",text}); view.entries.push_back({"Saga",view.report()}); input.clear(); cursor=0; scroll=0; continue; }
    if (!view.approval_id.empty()) {
      if (lower(text) == "y" || lower(text) == "n") {
        try { approve({{"type","approval"},{"request_id",view.approval_request_id},{"payload",{{"approval_id",view.approval_id},{"approved",lower(text) == "y"}}}}); view.approval_id.clear(); view.phase="act"; input.clear(); cursor=0; }
        catch (const std::exception& e) { view.event("error",{{"message",e.what()}}); }
      }
      continue;
    }
    if (busy) continue;
    if (view.permission_menu && (text == "1" || text == "2" || text == "3" || text == "4")) text="/permissions "+text;
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
