#include "check.hpp"
#include <saga/chat_ui.hpp>
#include <algorithm>
#include <clocale>
#include <atomic>
#include <iostream>
#include <thread>
using namespace saga;
int main(int argc,char** argv) {
  std::setlocale(LC_ALL,"C.UTF-8");
  try {
    ChatView view; view.name="Persona 東京"; view.model="Long-local-model-name-for-status-testing"; view.context=65536;
    if (argc == 2 && std::string(argv[1]) == "--mouse-demo") {
      std::string history="HISTORY_OLDEST\n";
      for(int i=0;i<60;++i) history += "History line "+std::to_string(i)+"\n";
      history += "HISTORY_LATEST";
      view.entries.push_back({view.name,history});
      auto action=chat_ui(view,[](const std::string& input,Emit emit){
        CHECK(input=="background");std::this_thread::sleep_for(std::chrono::milliseconds(400));
        emit("notification",{{"description","COPY_QUEUED_UPDATE"}});
      },
        []{std::this_thread::sleep_for(std::chrono::milliseconds(10));return std::optional<Json>{};},
        [](const Json&){CHECK(false);});
      CHECK(action==ChatAction::Exit);std::cout<<"MOUSE_EXIT_OK\n";return 0;
    }
    if(argc==2 && std::string(argv[1])=="--diff-demo") {
      std::atomic_int decision=0;std::atomic_bool stopped=false;int turns=0;
      std::string full_diff="--- a/example.c\n+++ b/example.c\n@@ -1,16 +1,16 @@\n";for(int i=0;i<16;++i)full_diff+="-old "+std::to_string(i)+"\n+new "+std::to_string(i)+"\n";
      auto action=chat_ui(view,[&](const std::string& input,Emit emit){
        if(input=="cancel") {
          emit("turn.started",{{"session_id",1}});
          emit("tool.output",{{"run_id",9},{"stream","stdout"},{"content","LIVE_OUTPUT_STARTED"}});
          for(int i=0;i<100 && !stopped;++i)std::this_thread::sleep_for(std::chrono::milliseconds(50));
          CHECK(stopped);
          emit("turn.cancelled",{{"message","STOPPED_COPY_WORK"}});emit("turn.finished",Json::object());return;
        }
        ++turns;decision=0;auto diff=full_diff;
        emit("approval.requested",{{"approval_id","diff-approval"},{"_request_id","diff-request"},{"tool","file_edit"},{"arguments",{{"kind","edit"},{"proposal_id","diff-"+std::to_string(turns)},{"path","example.c"},{"diff",diff},{"added",16},{"removed",16}}}});
        for(int i=0;i<100 && !decision;++i)std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(decision);
        emit("approval.resolved",{{"approved",decision==1}});
        if(decision==1)emit("edit.applied",{{"event_id",turns},{"proposal_id","diff-"+std::to_string(turns)},{"path","example.c"},{"diff",diff},{"added",16},{"removed",16}});
        emit("notification",{{"description",decision==1?"DIFF_ACCEPTED":"DIFF_REJECTED"}});
      },[]{std::this_thread::sleep_for(std::chrono::milliseconds(10));return std::optional<Json>{};},[&](const Json& frame){decision=frame["payload"]["approved"].get<bool>()?1:2;},[&](const std::string& command,Emit emit){if(command.starts_with("/diff "))emit("diff.loaded",{{"event_id",std::stoll(command.substr(6))},{"diff",full_diff}});else if(command=="/status")emit("result","LIVE_INSPECTION_VISIBLE");else {CHECK(command=="/stop");stopped=true;emit("result","Stopping");}});
      CHECK(action==ChatAction::Exit && turns==2 && stopped);std::cout<<"DIFF_EXIT_OK\n";return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--demo") {
      view.entries.push_back({view.name,"Hello! **Bold** *italic* ~~strike~~ `inline code`. Resize the terminal while I respond.",false,0,0,{},true});
      std::atomic_bool accepted=false; int live_changes=0;
      auto action=chat_ui(view,[&](const std::string&,Emit emit){
        emit("assistant.delta",{{"content","Checking the terminal..."}});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        emit("tool.started",{{"run_id",1},{"tool","shell_exec"},{"arguments",{{"command","tmux capture-pane -t 0:0 -p -S -300"}}}});
        emit("approval.requested",{{"approval_id","demo-nonce"},{"_request_id","demo-request"},{"tool","shell_exec"},{"arguments",{{"command","tmux capture-pane -t 0:0 -p -S -300"}}}});
        for (int i=0; i<100 && !accepted; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(accepted);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        emit("tool.completed",{{"run_id",1},{"tool","shell_exec"},{"result",Json::object()}});
        emit("context.usage",{{"input_tokens",21900},{"context_length",65536},{"approximate",true},{"active_task",Json::array({{{"title","Inspect tmux"}}})}});
        emit("approval.resolved",{{"approved",true},{"approval_id","demo-nonce"}});
        emit("assistant.completed",{{"content","Terminal checked. Unicode: español, 東京.\n**All done.** `inline code`"}});
      },[]{ std::this_thread::sleep_for(std::chrono::milliseconds(10)); return std::optional<Json>{}; },[&](const Json& frame){
        CHECK(frame["type"] == "approval" && frame["request_id"] == "demo-request");
        CHECK(frame["payload"]["approval_id"] == "demo-nonce" && frame["payload"]["approved"] == true);
        accepted=true;
      },[&](const std::string& input,Emit emit){
        if (input == "/permissions") emit("permissions.menu",{{"label","Ask for guarded host operations"}});
        else if (input == "/permissions 3") { ++live_changes; emit("permissions.changed",{{"label","Ask for unrestricted host operations"}}); emit("result","LIVE_PERMISSION_UPDATED"); }
        else if (input == "/status") emit("result",view.report());
        else if (input == "/tasks") emit("result","LIVE_TASKS_INSPECTED");
        else throw std::runtime_error("LIVE_COMMAND_REJECTED");
      });
      CHECK(action == ChatAction::Exit && accepted && live_changes==1); std::cout << "NCURSES_EXIT_OK\n"; return 0;
    }
    view.entries.push_back({"user","Unicode width: 東京 and é. A long message that wraps when the terminal shrinks.\nA second paragraph."});
    view.event("assistant.delta",{{"content","A streamed response."}});
    CHECK(view.partial == "A streamed response.");
    view.event("tool.started",{{"run_id",7},{"tool","shell_exec"},{"arguments",{{"command","tmux capture-pane -t 0:0 -p -S -300"}}}});
    CHECK(view.partial.empty()); CHECK(view.entries[1].label == view.name);
    view.event("tool.completed",{{"run_id",7},{"result",Json::object()}});
    CHECK(view.entries.back().command.find("s") != std::string::npos);
    view.event("assistant.completed",{{"content","Final answer."}});
    view.event("context.usage",{{"input_tokens",21900},{"context_length",65536},{"approximate",true},{"active_task",Json::array({{{"title","Inspect tmux"}}})}});
    CHECK(view.tokens == 21900 && view.task == "Inspect tmux");
    CHECK(chat_utf8(view.status(120)).find("~21.9K/65.5K") != std::string::npos);
    view.event("context.usage",{{"input_tokens",10000},{"output_tokens",17},{"used_tokens",10017},{"streaming",true},{"approximate",false}});
    CHECK(chat_utf8(view.status(120)).find("+17")!=std::string::npos);
    CHECK(chat_utf8(view.activity(true))==view.name+" is typing...");
    view.event("context.usage",{{"input_tokens",10000},{"output_tokens",17},{"cached_input_tokens",8500},{"used_tokens",10017},{"streaming",true},{"approximate",false}});
    CHECK(view.report().find("Prompt cache: 8500 tokens reused / 10000 input")!=std::string::npos);
    view.event("context.usage",{{"input_tokens",10000},{"output_tokens",18},{"used_tokens",10018},{"streaming",false},{"approximate",false}});
    CHECK(chat_utf8(view.status(120)).find("+18")==std::string::npos);
    CHECK(chat_utf8(view.activity(true))==view.name+" is preparing context...");
    view.event("context.usage",{{"input_tokens",12000},{"output_tokens",0},{"streaming",true}});
    CHECK(chat_utf8(view.activity(true))==view.name+" is preparing context...");
    view.event("context.usage",{{"input_tokens",12500},{"output_tokens",0},{"streaming",true}});
    CHECK(chat_utf8(view.activity(true))==view.name+" is preparing context...");
    view.phase="act";
    view.event("context.usage",{{"output_tokens",1},{"streaming",true}});
    CHECK(chat_utf8(view.activity(true))==view.name+" is typing...");
    view.event("context.usage",{{"output_tokens",1},{"streaming",false}});
    CHECK(chat_utf8(view.activity(true))==view.name+" is running a tool...");
    CHECK(view.activity(false).empty());view.phase="respond";
    view.event("session.started",{{"session_id",2}});
    CHECK(view.task.empty() && view.agent_status["task"].empty());
    CHECK(chat_utf8(view.status(120)).find("Inspect tmux")==std::string::npos);
    view.event("generation.started",{{"generation_id",99}});
    view.event("reasoning.started",{{"generation_id",99}});
    CHECK(chat_utf8(view.activity(true))==view.name+" is thinking...");
    view.event("context.usage",{{"output_tokens",8192},{"streaming",true}});
    CHECK(chat_utf8(view.activity(true))==view.name+" is thinking...");
    view.event("tool.call.started",{{"tool_call_id","prepared"}});
    CHECK(chat_utf8(view.activity(true))==view.name+" is generating a tool call...");
    view.event("commentary.completed",{{"content","A verified stage finished; moving to the next action."}});
    CHECK(view.entries.back().text=="A verified stage finished; moving to the next action.");
    view.event("turn.finished",Json::object());
    CHECK(view.activity(false).empty());
    auto wide=view.lines(100); auto narrow=view.lines(20); CHECK(narrow.size() > wide.size());
    for (int width : {6,12,20,40,80,120}) {
      for (const auto& line : view.lines(width)) CHECK(chat_columns(line) <= width);
      CHECK(chat_columns(view.status(width)) <= width);
    }
    CHECK(chat_columns(chat_wide("東京")) == 4);
    CHECK(chat_columns(chat_wide("é")) == 1);
    CHECK(chat_utf8(chat_wide("español 東京")) == "español 東京");
    CHECK(chat_wrap(L"東京",1) == std::vector<std::wstring>({L"?",L"?"}));
    view.event("approval.requested",{{"approval_id","nonce"},{"_request_id","request"},{"tool","shell_exec"},{"arguments",{{"command","echo ok"}}}});
    CHECK(view.approval_id == "nonce" && view.approval_request_id == "request");
    view.partial="Still writing"; view.generating=true;
    view.event("command.error",{{"message","Wait for the turn to finish"}});
    CHECK(view.partial=="Still writing" && view.generating && view.approval_id=="nonce");
    view.partial.clear();
    view.event("session.reset",Json::object()); CHECK(view.entries.empty() && view.tokens == 0);
    auto detailed=command_help("/permissions"); CHECK(detailed.find("Examples:") != std::string::npos && detailed.find("/permissions always") != std::string::npos);
    CHECK(detailed.find("/permissions 3") != std::string::npos && detailed.find("/permissions 4") != std::string::npos && detailed.find("DANGEROUS") != std::string::npos);
    view.event("permissions.menu",{{"mode","host_ask"},{"label","Ask for unrestricted host operations"}});
    CHECK(view.permission_menu && view.entries.back().text.find("3. Sandbox automatic") != std::string::npos && view.entries.back().text.find("4. Allow unrestricted") != std::string::npos);
    view.event("permissions.changed",{{"mode","host_always"},{"label","Unrestricted host without approval (DANGEROUS)"}});
    CHECK(!view.permission_menu && view.agent_status["permissions"]["mode"] == "host_always");
    CHECK(command_help("web").find("/web off")!=std::string::npos);
    view.event("web.changed",{{"enabled",false},{"engine","duckduckgo"}});CHECK(view.agent_status["web"]["enabled"]==false);
    view.event("tool.started",{{"tool","web_search"},{"run_id",54321},{"arguments",{{"query","site:example.com rules"}}}});
    CHECK(view.phase=="research" && view.entries.back().text.find("site:example.com rules")!=std::string::npos);
    view.event("tool.completed",{{"tool","web_search"},{"run_id",54321},{"result",{{"results",Json::array()}}}});
    view.event("tool.started",{{"tool","web_search"},{"run_id",54322},{"arguments",{{"query","rotation rules"}}}});
    view.event("tool.completed",{{"tool","web_search"},{"run_id",54322},{"result",{{"query","rotation rules"},{"submitted_query","rotation rules site:example.com"}}}});
    CHECK(view.entries.back().text.find("rotation rules site:example.com")!=std::string::npos);
    view.event("tool.started",{{"tool","web_fetch"},{"run_id",54323},{"arguments",{{"url","https://example.com/manual"}}}});
    view.event("tool.completed",{{"tool","web_fetch"},{"run_id",54323},{"result",{{"url","https://example.com/manual"},{"title","Manual"}}}});
    CHECK(view.entries.back().text=="Visited: Manual (https://example.com/manual)");
    view.event("tool.started",{{"tool","web_read"},{"run_id",54324},{"arguments",{{"source_id",0}}}});
    view.event("tool.completed",{{"tool","web_read"},{"run_id",54324},{"result",{{"url","https://example.com/rules"},{"title","Rules"}}}});
    CHECK(view.entries.back().text=="Visited: Rules (https://example.com/rules)");
    auto web_lines=view.lines(100);CHECK(std::any_of(web_lines.begin(),web_lines.end(),[](auto& line){return line.starts_with(L"• ");}));
    CHECK(std::none_of(web_lines.begin(),web_lines.end(),[](auto& line){return line.find(L"┊")!=std::wstring::npos;}));
    view.event("research.warning",{{"content","Synthetic uncertainty warning"}});CHECK(view.entries.back().text=="Synthetic uncertainty warning");
    CHECK(command_help().find("Memory and knowledge:") != std::string::npos);
    view.event("context.usage",{{"used_tokens",22700},{"input_tokens",21900},{"output_tokens",800},{"context_length",65536},{"compactions",2},{"approximate",true}});
    CHECK(view.tokens == 22700 && view.compactions == 2); CHECK(view.report().find("x2") != std::string::npos);
    view.compactions=0; CHECK(view.report().find(" x0") == std::string::npos);
    auto syntax=shell_highlight(L"notify-send --icon dialog \"hello world\" $USER | cat # comment");
    for (auto color : {ChatColor::Command,ChatColor::Option,ChatColor::String,ChatColor::Variable,ChatColor::Operator,ChatColor::Comment}) CHECK(std::any_of(syntax.begin(),syntax.end(),[&](const ChatSpan& span){ return span.color == color; }));
    Json approval_args={{"command","printf --help \"Hello 東京 😀\" $USER | cat # first line\npwd"},{"execution","host"},{"timeout_seconds",30},{"enabled",true},{"optional",nullptr}};
    for(bool ascii:{false,true}) {
      auto json=chat_wide(approval_args.dump(2,' ',ascii));auto spans=json_highlight(json);
      auto color_at=[&](std::wstring_view token){auto at=json.find(token);CHECK(at!=std::wstring::npos);ChatColor color=ChatColor::Default;for(auto span:spans){CHECK(span.start+span.length<=json.size());if(at>=span.start && at<span.start+span.length)color=span.color;}return color;};
      CHECK(color_at(L"\"command\"")==ChatColor::Link);
      CHECK(color_at(L"printf")==ChatColor::Command && color_at(L"--help")==ChatColor::Option);
      CHECK(color_at(L"Hello")==ChatColor::String && color_at(L"$USER")==ChatColor::Variable);
      CHECK(color_at(L"|")==ChatColor::Operator && color_at(L"# first")==ChatColor::Comment);
      CHECK(color_at(L"pwd")==ChatColor::Command && color_at(L"\"host\"")==ChatColor::String);
      CHECK(color_at(L"30")==ChatColor::Option && color_at(L"true")==ChatColor::Variable && color_at(L"null")==ChatColor::Variable);
    }
    ChatView approval;approval.event("approval.requested",{{"approval_id","color-nonce"},{"_request_id","color-request"},{"tool","shell_exec"},{"arguments",approval_args}});
    CHECK(approval.entries[0].text.find(approval_args.dump(2))!=std::string::npos);
    for(int width:{6,12,20,40,80,120}) {
      bool key=false,command=false,option=false,variable=false;
      for(auto line:approval.styled_lines(width)){CHECK(chat_columns(line.text)<=width);for(auto span:line.spans){CHECK(span.start+span.length<=line.text.size());key|=span.color==ChatColor::Link && (span.style&Bold);command|=span.color==ChatColor::Command;option|=span.color==ChatColor::Option;variable|=span.color==ChatColor::Variable;}}
      CHECK(key && command && option && variable);
    }
    approval.event("approval.resolved",{{"approved",true}});CHECK(approval.entries[0].text.ends_with("Approved."));
    CHECK(!json_highlight(L"{\"command\":\"unfinished").empty());
    view.entries={{"Approval","Approve?"},{"Saga","Status"},{"", "Working",true,1,0,"printf \"hello\""}};
    bool yellow=false,gray=false,blue=false;
    for (auto& line : view.styled_lines(80)) for (auto& span : line.spans) { yellow=yellow || span.color == ChatColor::Approval; gray=gray || span.color == ChatColor::Saga; blue=blue || span.color == ChatColor::Prompt; }
    CHECK(yellow && gray && blue);
    ChatView diffs;std::string diff="--- a/x.c\n+++ b/x.c\n@@ -1 +1 @@\n-old\n";for(int i=0;i<30;++i)diff+="+new line "+std::to_string(i)+"\n";
    Json edit={{"kind","edit"},{"proposal_id","proposed"},{"path","x.c"},{"diff",diff},{"added",30},{"removed",1}};
    diffs.event("approval.requested",{{"approval_id","edit-nonce"},{"_request_id","edit-request"},{"tool","file_edit"},{"arguments",edit}});
    CHECK(diffs.entries.empty());diffs.event("result","Live inspection");CHECK(diffs.approval_hidden && diffs.approval_lines(80,40).empty());diffs.approval_hidden=false;auto popup=diffs.approval_lines(80,40);CHECK(popup.size()==17);
    CHECK(std::none_of(popup.begin(),popup.end(),[](auto& line){return line.text.find(L"new line 29")!=std::wstring::npos;}));
    CHECK(std::count_if(popup.begin(),popup.end(),[](auto& line){return !line.target.empty();})==3);
    diffs.activate("approval:toggle");CHECK(diffs.approval_expanded);diffs.approval_scroll=100;
    popup=diffs.approval_lines(80,40);CHECK(std::any_of(popup.begin(),popup.end(),[](auto& line){return line.text.find(L"new line 29")!=std::wstring::npos;}));
    for(int width:{12,20,40,80})for(int height:{15,20,35})for(auto& line:diffs.approval_lines(width,height)){CHECK(chat_columns(line.text)<=width);for(auto span:line.spans)CHECK(span.start+span.length<=line.text.size());}
    diffs.event("approval.resolved",{{"approved",true}});CHECK(diffs.approval_lines(80,40).empty());
    diffs.event("edit.applied",edit);auto collapsed=diffs.lines(80);diffs.activate("diff:proposed");CHECK(diffs.entries.back().expanded);auto expanded=diffs.lines(80);CHECK(expanded.size()>collapsed.size());
    ChatView archived;edit["event_id"]=123;edit["preview"]=true;archived.event("edit.applied",edit);CHECK(!archived.entries[0].diff_loaded && archived.entries[0].diff_event==123);archived.event("diff.loaded",edit);CHECK(archived.entries[0].diff_loaded && archived.entries[0].expanded);
    bool additions=false,deletions=false;for(auto& line:diffs.styled_lines(80))for(auto span:line.spans){additions|=span.color==ChatColor::Addition;deletions|=span.color==ChatColor::Deletion;}CHECK(additions && deletions);
    ChatView public_progress;public_progress.event("progress.updated",{{"message_id","stable"},{"text","**Working**"},{"complete",false}});public_progress.event("progress.updated",{{"message_id","stable"},{"text","**Working** now"},{"complete",true}});CHECK(public_progress.entries.size()==1 && !public_progress.entries[0].streaming);
    public_progress.event("tool.output",{{"run_id",4},{"stream","stdout"},{"content","hello\x1b[31m"}});public_progress.event("tool.output",{{"run_id",4},{"stream","stdout"},{"content"," world"}});CHECK(public_progress.entries.size()==2 && public_progress.entries[1].text.find('\x1b')==std::string::npos);
    ChatView help;help.name="test";help.event("help",{{"content",command_help()}});CHECK(help.entries[0].label=="Saga");
    bool accent=false,description=false,heading=false;std::wstring help_text;
    for(auto& line:help.styled_lines(80)){help_text+=line.text+L"\n";for(auto span:line.spans){accent|=span.color==ChatColor::Link && (span.style&Bold);description|=span.color==ChatColor::Activity;heading|=span.color==ChatColor::Heading && (span.style&Bold);}}
    CHECK(accent && description && heading && help_text.find(L"**/new")==std::wstring::npos && help_text.find(L"## Session")==std::wstring::npos);
    auto fixture=read_file(SAGA_MARKDOWN_FIXTURE);
    auto rendered=markdown_lines(fixture,110); std::wstring visible;
    bool bold=false,italic=false,strike=false,inline_code=false,link=false,focus=false;
    for(auto& line:rendered){visible+=line.text+L"\n";for(auto span:line.spans){bold|=(span.style&Bold)!=0;italic|=(span.style&Italic)!=0;strike|=(span.style&Strike)!=0;inline_code|=span.color==ChatColor::InlineCode;link|=span.color==ChatColor::Link;focus|=span.color==ChatColor::CodeFocus;}}
    CHECK(bold && italic && strike && inline_code && link && focus);
    for(auto text:{L"H1: Titulo principal",L"H6: Detalle minimo",L"`backtick literal`",L"def hola(mundo: str)",L"☑ tarea completada",L"☐ tarea pendiente",L"│ Encabezado dentro de cita",L"triple tilde",L"fin del documento"})CHECK(visible.find(text)!=std::wstring::npos);
    for(auto text:{L"**Negrita",L"___Negrita",L"~~Texto",L"[texto de enlace]",L"![alt text]",L"```",L"backslash\\",L"~~~strike~~~"})CHECK(visible.find(text)==std::wstring::npos);
    for(int width:{1,2,6,20,40,80})for(auto& line:markdown_lines(fixture,width))CHECK(chat_columns(line.text)<=width);
    for(size_t end=1;end<fixture.size();end+=7)for(auto& line:markdown_lines(std::string_view(fixture).substr(0,end),40))CHECK(chat_columns(line.text)<=40);
    std::string fragmented="### Streaming 東京\n**bold** *italic* ~~strike~~ `inline` [link](https://example.com)\n\n```bash\nprintf '**literal** `code`'\n```";
    ChatView streaming;streaming.name="parser";
    for(char byte:fragmented){streaming.event("assistant.delta",{{"content",std::string(1,byte)}});for(auto& line:streaming.styled_lines(42))CHECK(chat_columns(line.text)<=42);}
    streaming.event("assistant.completed",{{"content",fragmented}});
    std::wstring completed;for(auto& line:streaming.lines(100))completed+=line+L"\n";
    CHECK(completed.find(L"**bold**")==std::wstring::npos && completed.find(L"**literal** `code`")!=std::wstring::npos && completed.find(L"東京")!=std::wstring::npos);
    auto nested=markdown_lines("~~**both**~~",80);CHECK(std::any_of(nested[0].spans.begin(),nested[0].spans.end(),[](auto s){return (s.style&(Bold|Strike))==(Bold|Strike);}));
    auto provisional=markdown_lines("**unfinished bold",80,true);CHECK(provisional[0].text==L"unfinished bold" && (provisional[0].spans[0].style&Bold));
    CHECK(markdown_lines("**unfinished bold",80)[0].text==L"**unfinished bold");
    CHECK(markdown_lines("`unfinished code",80,true)[0].text==L"unfinished code");
    CHECK(markdown_lines("[label](https://exa",80,true)[0].text==L"label");
    CHECK(markdown_lines("**",80,true)[0].text.empty());
    CHECK(markdown_lines("`",80,true)[0].text.empty());
    for(size_t end=1;end<fixture.size();end+=11)for(auto& line:markdown_lines(std::string_view(fixture).substr(0,end),40,true))CHECK(chat_columns(line.text)<=40);
    for(auto& line:markdown_lines("**safe**\x1b[31m\x1b]0;injected\a",80))CHECK(line.text.find(L'\x1b')==std::wstring::npos);
    auto escaped=markdown_lines("\\*literal\\* &amp; &#65;\n\n```\n~~~literal~~~\n**raw**\n```",80);std::wstring raw;for(auto& line:escaped)raw+=line.text+L"\n";CHECK(raw.find(L"*literal* & A")!=std::wstring::npos && raw.find(L"~~~literal~~~")!=std::wstring::npos && raw.find(L"**raw**")!=std::wstring::npos);
    auto literal_example=markdown_lines("````markdown\n# h1\n**bold**\n```bash\n~~~literal~~~\n```\n````\n\n# rendered header\n**formatted**",80);std::wstring literal_text;for(auto& line:literal_example)literal_text+=line.text+L"\n";CHECK(literal_text.find(L"# h1")!=std::wstring::npos && literal_text.find(L"**bold**")!=std::wstring::npos && literal_text.find(L"~~~literal~~~")!=std::wstring::npos && literal_text.find(L"# rendered header")==std::wstring::npos && literal_text.find(L"**formatted**")==std::wstring::npos);
    CHECK(markdown_lines("![](https://example.com/image.png)",80)[0].text==L"▧ image");
    ChatView named_user;named_user.name="user";named_user.entries.push_back({"user","**literal user input**"});named_user.event("assistant.completed",{{"content","**formatted assistant**"}});std::wstring separate;for(auto& line:named_user.lines(80))separate+=line+L"\n";CHECK(separate.find(L"**literal user input**")!=std::wstring::npos && separate.find(L"**formatted assistant**")==std::wstring::npos);
    std::cout << "PASS ncurses chat layout, resize, Unicode, streaming, tools, usage and approvals\n"; return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL chat UI: " << e.what() << '\n'; return 1; }
}
