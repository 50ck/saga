#include <saga/chat_ui.hpp>
#include <md4c.h>
#include <algorithm>
#include <exception>
#include <climits>
#include <cctype>
#include <cwctype>
#include <regex>
#include <sstream>
#include <set>

namespace saga {
namespace {
struct Style { ChatColor color=ChatColor::Default; unsigned flags=0; };
// Preview only: close unfinished inline constructs without changing stored text.
// Finished responses are always parsed from the original, unmodified Markdown.
std::string preview(std::string_view text) {
  std::string source(text);size_t lead=source.size();while(lead && (static_cast<unsigned char>(source[lead-1])&0xc0)==0x80)--lead;
  if(lead){auto c=static_cast<unsigned char>(source[lead-1]);size_t needed=(c&0xf8)==0xf0?4:(c&0xf0)==0xe0?3:(c&0xe0)==0xc0?2:1;if(source.size()-(lead-1)<needed)source.resize(lead-1);}
  std::vector<std::string> stack;std::string fence,ticks;bool bracket=false;int url=0;
  for(size_t i=0;i<source.size();) {
    if(i==0 || source[i-1]=='\n') {
      size_t at=source.find_first_not_of(" >\t",i);if(at==std::string::npos)break;
      size_t end=source.find('\n',i);if(end==std::string::npos)end=source.size();
      if(at<end && (source.compare(at,3,"```")==0 || source.compare(at,3,"~~~")==0)) {
        size_t run=at;while(run<end && source[run]==source[at])++run;
        bool paired=source[at]=='~' && source.find("~~~",run)<end;
        if(!paired && ticks.empty()) {auto mark=source.substr(at,run-at);if(fence.empty())fence=mark;else if(mark[0]==fence[0] && mark.size()>=fence.size() && trim(std::string_view(source).substr(run,end-run)).empty())fence.clear();i=end<source.size()?end+1:end;continue;}
      }
      if(!fence.empty()){i=end<source.size()?end+1:end;continue;}
    }
    char c=source[i];if(c=='\\'){i+=2;continue;}
    if(c=='`') {size_t end=i;while(end<source.size() && source[end]=='`')++end;auto run=source.substr(i,end-i);if(ticks.empty() && end==source.size()){source.erase(i);break;}if(ticks.empty())ticks=run;else if(ticks==run)ticks.clear();i=end;continue;}
    if(!ticks.empty()){++i;continue;}
    if(url){if(c=='(')++url;else if(c==')')--url;++i;continue;}
    if(c=='['){bracket=true;++i;continue;}
    if(c==']'){bracket=false;if(i+1<source.size() && source[i+1]=='('){url=1;i+=2;}else ++i;continue;}
    if(c=='*' || c=='_' || c=='~') {
      size_t end=i;while(end<source.size() && source[end]==c)++end;auto run=source.substr(i,end-i);
      if(c=='~' && run.size()<2){i=end;continue;}
      bool before=i && !std::isspace(static_cast<unsigned char>(source[i-1]));bool after=end<source.size() && !std::isspace(static_cast<unsigned char>(source[end]));
      if(c=='_' && i && end<source.size() && std::isalnum(static_cast<unsigned char>(source[i-1])) && std::isalnum(static_cast<unsigned char>(source[end]))){i=end;continue;}
      if(!stack.empty() && stack.back()==run && before)stack.pop_back();
      else if(after)stack.push_back(run);
      else if(end==source.size() && !before){source.erase(i);break;}
      i=end;continue;
    }
    ++i;
  }
  if(!fence.empty())return source;
  if(!ticks.empty())source+=ticks;
  else {if(url)source.append(static_cast<size_t>(url),')');else if(bracket)source+="]()";for(auto it=stack.rbegin();it!=stack.rend();++it)source+=*it;}
  return source;
}
std::vector<ChatSpan> code_highlight(std::wstring_view line,std::string_view language) {
  if(language=="bash" || language=="sh" || language=="shell")return shell_highlight(line);
  if(language.empty())return {};
  static const std::set<std::wstring_view> keywords={L"def",L"return",L"class",L"import",L"from",L"if",L"else",L"elif",L"for",L"while",L"in",L"try",L"except",L"with",L"as",L"True",L"False",L"None",L"const",L"let",L"var",L"function",L"async",L"await",L"new",L"throw",L"true",L"false",L"null",L"int",L"auto",L"void",L"bool",L"public",L"static",L"include"};
  std::vector<ChatSpan> result;
  for(size_t i=0;i<line.size();) {
    size_t begin=i;auto c=line[i];auto color=ChatColor::Default;
    if((language=="python" && c==L'#') || (c==L'/' && i+1<line.size() && line[i+1]==L'/')) {result.push_back({i,line.size()-i,ChatColor::Comment});break;}
    if(c==L'\'' || c==L'"' || c==L'`'){color=ChatColor::String;++i;while(i<line.size()){if(line[i]==L'\\' && i+1<line.size()){i+=2;continue;}if(line[i++]==c)break;}}
    else if(std::iswdigit(c)){color=ChatColor::Option;++i;while(i<line.size() && (std::iswalnum(line[i]) || line[i]==L'.'))++i;}
    else if(std::iswalpha(c) || c==L'_'){++i;while(i<line.size() && (std::iswalnum(line[i]) || line[i]==L'_'))++i;if(keywords.contains(line.substr(begin,i-begin)))color=ChatColor::Variable;}
    else {++i;continue;}
    if(color!=ChatColor::Default)result.push_back({begin,i-begin,color});
  }
  return result;
}
void append(ChatLine& line,std::wstring_view text,Style style={}) {
  if (text.empty()) return;
  auto offset=line.text.size(); line.text += text;
  if (style.color != ChatColor::Default || style.flags) {
    if(!line.spans.empty() && line.spans.back().start+line.spans.back().length==offset && line.spans.back().color==style.color && line.spans.back().style==style.flags) line.spans.back().length+=text.size();
    else line.spans.push_back({offset,text.size(),style.color,style.flags});
  }
}
std::vector<ChatLine> wrap(const ChatLine& source,int width,bool prose=true) {
  std::vector<ChatLine> result; size_t start=0; width=std::max(1,width);
  do {
    size_t end=start,space=std::wstring::npos; int columns=0;
    while (end<source.text.size()) { auto n=chat_columns(std::wstring_view(source.text).substr(end,1)); if (columns+n>width) break; columns+=n; if (source.text[end]==L' ') space=end; ++end; }
    size_t next=end;
    if (prose && end<source.text.size() && space!=std::wstring::npos && space>start) { end=space; next=space+1; }
    if (end==start && start<source.text.size()) { result.push_back({L"?",{}}); ++start; continue; }
    ChatLine row{source.text.substr(start,end-start),{}};
    for (auto span : source.spans) { auto a=std::max(start,span.start),b=std::min(end,span.start+span.length); if (b>a) row.spans.push_back({a-start,b-a,span.color,span.style}); }
    result.push_back(std::move(row)); start=next;
  } while (start<source.text.size());
  return result;
}
// A small extension requested by the UI: paired inline triple tildes are strikes.
// Leave fenced/inline code untouched, including literal Markdown inside code.
std::string dialect(std::string_view source) {
  std::istringstream input{std::string(source)}; std::string line,result; std::string fence;
  while (std::getline(input,line)) {
    auto at=line.find_first_not_of(" >\t"); auto prefix=at==std::string::npos ? std::string() : line.substr(at);
    bool paired=prefix.starts_with("~~~") && prefix.find("~~~",3)!=std::string::npos;
    size_t run=0;if(!prefix.empty())while(run<prefix.size() && prefix[run]==prefix[0])++run;
    if (fence.empty() && !paired && (prefix.starts_with("```") || prefix.starts_with("~~~"))) fence=prefix.substr(0,run);
    else if (!fence.empty()) { if (prefix.starts_with(fence) && trim(std::string_view(prefix).substr(run)).empty()) fence.clear(); result+=line+'\n'; continue; }
    if (fence.empty()) {
      size_t i=0; std::string ticks;
      while (i<line.size()) {
        if (line[i]=='\\') { i+=2; continue; }
        if (line[i]=='`') { size_t end=i; while(end<line.size() && line[end]=='`') ++end; auto run=line.substr(i,end-i); if(ticks.empty()) ticks=run; else if(ticks==run) ticks.clear(); i=end; continue; }
        if(ticks.empty() && line.compare(i,3,"~~~")==0) { auto end=line.find("~~~",i+3); if(end!=std::string::npos && end>i+3) { line.erase(end,1); line.erase(i,1); i=end+1; continue; } }
        ++i;
      }
    }
    result+=line+'\n';
  }
  if (!source.ends_with('\n') && !result.empty()) result.pop_back();
  return result;
}
struct Renderer {
  int width; unsigned quote=0,heading=0; bool code=false,cell=false,flushing=false; unsigned focus=0;
  std::string code_text;
  std::string language,info; unsigned code_number=1; std::exception_ptr error;
  std::vector<Style> styles{{}};
  std::vector<bool> image_text;
  struct List { bool ordered; unsigned next; }; std::vector<List> lists;
  std::vector<ChatLine> lines{{}},cells; std::vector<std::vector<ChatLine>> rows; std::vector<MD_ALIGN> align;
  ChatLine current_cell;
  Style style() const { auto s=styles.back(); if(heading) {if(s.color==ChatColor::Default)s.color=ChatColor::Heading; s.flags|=Bold; if(heading<=2) s.flags|=Underline;} if(code) s.color=ChatColor::CodeBlock; return s; }
  void put(std::wstring_view text) {
    if(cell) {append(current_cell,text,style());return;}
    for (auto c:text) {
      if(c==L'\n') {lines.emplace_back();continue;}
      if(c==L'\t'){put(L"    ");continue;}
      auto& line=lines.back(); if(line.text.empty()) {for(unsigned i=0;i<quote;++i) append(line,L"│ ",{ChatColor::Activity}); if(!lists.empty()) append(line,std::wstring(lists.size()*2,L' '));}
      append(line,std::wstring_view(&c,1),style());
    }
  }
  void newline(bool gap=false) { if(!lines.back().text.empty()) lines.emplace_back(); if(gap && lines.size()>1 && !lines[lines.size()-2].text.empty()) lines.emplace_back(); }
  void table() {
    if(rows.empty()) return;
    size_t count=0; for(auto& row:rows) count=std::max(count,row.size());
    if(!count) return;
    std::vector<int> widths(count,1); for(auto& row:rows) for(size_t i=0;i<row.size();++i) widths[i]=std::max(widths[i],chat_columns(row[i].text));
    int available=std::max(static_cast<int>(count),width-static_cast<int>(count)*3-1);
    while(true) {int sum=0;for(auto n:widths)sum+=n;if(sum<=available)break;auto largest=std::max_element(widths.begin(),widths.end());if(*largest<=1)break;--*largest;}
    newline();
    for(size_t r=0;r<rows.size();++r) {
      std::vector<std::vector<ChatLine>> wrapped(count); size_t height=1;
      for(size_t i=0;i<count;++i) {wrapped[i]=wrap(i<rows[r].size()?rows[r][i]:ChatLine{},widths[i]);height=std::max(height,wrapped[i].size());}
      for(size_t y=0;y<height;++y) {
        ChatLine out;append(out,L"│",{ChatColor::Activity});
        for(size_t i=0;i<count;++i) {
          auto v=y<wrapped[i].size()?wrapped[i][y]:ChatLine{};int padding=widths[i]-chat_columns(v.text); auto a=i<align.size()?align[i]:MD_ALIGN_DEFAULT;
          int left=a==MD_ALIGN_RIGHT?padding:a==MD_ALIGN_CENTER?padding/2:0;
          append(out,std::wstring(static_cast<size_t>(left+1),L' '));auto offset=out.text.size();out.text+=v.text;for(auto s:v.spans){s.start+=offset;out.spans.push_back(s);}
          append(out,std::wstring(static_cast<size_t>(padding-left+1),L' '));append(out,L"│",{ChatColor::Activity});
        }
        lines.back()=std::move(out);lines.emplace_back();
      }
      if(r==0) {append(lines.back(),std::wstring(static_cast<size_t>(std::min(width,available+static_cast<int>(count)*3+1)),L'─'),{ChatColor::Activity});lines.emplace_back();}
    }
    rows.clear();align.clear();
  }
  void enter(MD_BLOCKTYPE type,void* detail) {
    switch(type) {
      case MD_BLOCK_QUOTE:newline();++quote;break;
      case MD_BLOCK_UL:newline();lists.push_back({false,0});break;
      case MD_BLOCK_OL:newline();lists.push_back({true,static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start});break;
      case MD_BLOCK_LI:{newline();auto* d=static_cast<MD_BLOCK_LI_DETAIL*>(detail);auto& row=lines.back();for(unsigned i=0;i<quote;++i)append(row,L"│ ",{ChatColor::Activity});append(row,std::wstring(lists.empty()?0:(lists.size()-1)*2,L' '));
        auto marker=d->is_task?(d->task_mark==' '?L"☐ ":L"☑ "):!lists.empty() && lists.back().ordered?std::to_wstring(lists.back().next++)+L". ":L"• ";append(row,marker,{ChatColor::Activity});break;}
      case MD_BLOCK_H:newline(true);heading=static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;break;
      case MD_BLOCK_P:newline();break;
      case MD_BLOCK_HR:newline();put(std::wstring(static_cast<size_t>(std::max(1,width-static_cast<int>(quote)*2)),L'─'));newline(true);break;
      case MD_BLOCK_CODE:{newline();auto* d=static_cast<MD_BLOCK_CODE_DETAIL*>(detail);language.assign(d->lang.text?d->lang.text:"",d->lang.size);info.assign(d->info.text?d->info.text:"",d->info.size);
        std::smatch m;focus=0;if(std::regex_search(info,m,std::regex(R"(\{([0-9]+)\})"))) {try{focus=static_cast<unsigned>(std::stoul(m[1]));}catch(...){}}
        std::string title;if(std::regex_search(info,m,std::regex(R"re(title=["']([^"']*)["'])re")))title=m[1];
        auto label=title.empty()?language:title+(language.empty()?"":" · "+language);
        if(!label.empty()) {put(chat_wide(label));newline();}code_number=1;code=true;break;}
      case MD_BLOCK_TABLE:newline();rows.clear();align.clear();break;
      case MD_BLOCK_TR:cells.clear();break;
      case MD_BLOCK_TH:case MD_BLOCK_TD:cell=true;current_cell={};if(rows.empty())align.push_back(static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align);if(type==MD_BLOCK_TH)styles.push_back({ChatColor::Default,Bold});break;
      default:break;
    }
  }
  void leave(MD_BLOCKTYPE type,void*) {
    switch(type) {
      case MD_BLOCK_QUOTE:newline();--quote;break;
      case MD_BLOCK_UL:case MD_BLOCK_OL:newline();lists.pop_back();break;
      case MD_BLOCK_LI:newline();break;
      case MD_BLOCK_H:heading=0;newline(true);break;
      case MD_BLOCK_P:newline(lists.empty());break;
      case MD_BLOCK_CODE:flushing=true;text(MD_TEXT_CODE,code_text);flushing=false;code_text.clear();code=false;newline(true);break;
      case MD_BLOCK_TH:styles.pop_back();[[fallthrough]];
      case MD_BLOCK_TD:cell=false;cells.push_back(std::move(current_cell));break;
      case MD_BLOCK_TR:rows.push_back(std::move(cells));break;
      case MD_BLOCK_TABLE:table();newline(true);break;
      default:break;
    }
  }
  void span(MD_SPANTYPE type,void* detail,bool entering) {
    if(!entering){if(type==MD_SPAN_IMG){auto* image=static_cast<MD_SPAN_IMG_DETAIL*>(detail);if(!image_text.back())put(L"image");image_text.pop_back();if(image->title.size){put(L" — ");put(chat_wide({image->title.text,image->title.size}));}}styles.pop_back();return;}
    auto s=style();switch(type){case MD_SPAN_EM:s.flags|=Italic;break;case MD_SPAN_STRONG:s.flags|=Bold;break;case MD_SPAN_DEL:s.flags|=Strike;break;case MD_SPAN_CODE:s.color=ChatColor::InlineCode;break;case MD_SPAN_A:s.color=ChatColor::Link;s.flags|=Underline;break;case MD_SPAN_IMG:s.color=ChatColor::Link;image_text.push_back(false);put(L"▧ ");break;default:break;}
    (void)detail;styles.push_back(s);
  }
  void text(MD_TEXTTYPE type,std::string_view value) {
    if(!trim(value).empty())for(size_t i=0;i<image_text.size();++i)image_text[i]=true;
    if(code && !flushing){code_text+=value;return;}
    if(type==MD_TEXT_BR || type==MD_TEXT_SOFTBR){put(L"\n");return;}
    if(type==MD_TEXT_NULLCHAR){put(L"�");return;}
    if(type==MD_TEXT_ENTITY) {
      static const std::pair<std::string_view,std::wstring_view> known[]={{"&amp;",L"&"},{"&lt;",L"<"},{"&gt;",L">"},{"&quot;",L"\""},{"&apos;",L"'"},{"&nbsp;",L" "}};
      for(auto [key,text]:known)if(value==key){put(text);return;}
      if(value.starts_with("&#")) {try{auto hex=value.size()>3&&(value[2]=='x'||value[2]=='X');auto cp=std::stoul(std::string(value.substr(hex?3:2)),nullptr,hex?16:10);if(cp>=32 && cp<=0x10ffff && !(cp>=0xd800 && cp<=0xdfff)){put(std::wstring(1,static_cast<wchar_t>(cp)));return;}}catch(...){}}
    }
    if(code) {
      std::istringstream stream{std::string(value)};std::string line;
      while(std::getline(stream,line)) {
        auto wide=chat_wide(line);for(size_t at=0;(at=wide.find(L'\t',at))!=std::wstring::npos;at+=4)wide.replace(at,1,L"    ");
        put(wide);auto& row=lines.back();auto prefix=row.text.size()-wide.size();
        for(auto s:code_highlight(wide,language)){s.start+=prefix;row.spans.push_back(s);}
        if(code_number++==focus)row.spans.push_back({0,row.text.size(),ChatColor::CodeFocus,Bold});
        put(L"\n");
      }
      return;
    }
    put(chat_wide(value));
  }
};
template<class F> int callback(void* context,F fn) noexcept { auto& r=*static_cast<Renderer*>(context);try{fn(r);return 0;}catch(...){r.error=std::current_exception();return 1;} }
}
std::vector<ChatLine> markdown_lines(std::string_view source,int width,bool streaming) {
  Renderer r; r.width=std::max(1,width);
  MD_PARSER parser{};parser.flags=0x0100|0x0200|0x0800|0x0004|0x0020|0x0040;
  parser.enter_block=[](MD_BLOCKTYPE t,void* d,void* p){return callback(p,[&](Renderer& r){r.enter(t,d);});};
  parser.leave_block=[](MD_BLOCKTYPE t,void* d,void* p){return callback(p,[&](Renderer& r){r.leave(t,d);});};
  parser.enter_span=[](MD_SPANTYPE t,void* d,void* p){return callback(p,[&](Renderer& r){r.span(t,d,true);});};
  parser.leave_span=[](MD_SPANTYPE t,void* d,void* p){return callback(p,[&](Renderer& r){r.span(t,d,false);});};
  parser.text=[](MD_TEXTTYPE t,const char* s,unsigned n,void* p){return callback(p,[&](Renderer& r){r.text(t,{s,n});});};
  // ponytail: reparse received text on updates; cache unchanged bubbles in the view.
  auto input=dialect(streaming?preview(source):std::string(source));if(input.size()>UINT_MAX)throw std::runtime_error("Markdown message too large");
  if(md_parse(input.data(),static_cast<unsigned>(input.size()),&parser,&r)!=0){if(r.error)std::rethrow_exception(r.error);throw std::runtime_error("Markdown parse failed");}
  while(r.lines.size()>1 && r.lines.back().text.empty())r.lines.pop_back();
  std::vector<ChatLine> result;for(auto& line:r.lines){bool code=std::any_of(line.spans.begin(),line.spans.end(),[](const ChatSpan& s){return s.color==ChatColor::CodeBlock || s.color==ChatColor::CodeFocus;});auto parts=wrap(line,width,!code);for(auto& part:parts)result.push_back(std::move(part));}return result;
}
}
