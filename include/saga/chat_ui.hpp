#pragma once
#include <saga/common.hpp>
namespace saga {
enum class ChatAction { Continue, Selector, ModelSetup, Exit, NewSession };
enum class ChatColor : short { Default, Approval, Saga, Activity, Prompt, Command, String, Option, Variable, Operator, Comment, InlineCode, CodeBlock, Link, Heading, CodeFocus };
enum ChatStyle : unsigned { Bold=1, Italic=2, Underline=4, Strike=8 };
struct ChatSpan { size_t start,length; ChatColor color; unsigned style=0; };
struct ChatLine { std::wstring text; std::vector<ChatSpan> spans; };
std::vector<ChatLine> markdown_lines(std::string_view source,int width,bool streaming=false);
std::vector<ChatSpan> shell_highlight(std::wstring_view command);
std::vector<ChatSpan> json_highlight(std::wstring_view source);
std::string command_help(std::string_view command = {});
std::string format_agent_status(const Json& status);
std::wstring chat_wide(std::string_view text);
std::string chat_utf8(std::wstring_view text);
int chat_columns(std::wstring_view text);
std::vector<std::wstring> chat_wrap(std::wstring_view text,int width);
struct ChatEntry {
  std::string label,text;
  bool activity = false;
  Id run = 0,started = 0;
  std::string command{};
  bool markdown=false;
  bool help=false;
  std::vector<ChatSpan> spans{};
  std::string message_id{};
  bool streaming=false;
};
struct ChatView {
  std::string name,model,task,partial,approval_id,approval_request_id;
  std::uint64_t tokens = 0,context = 0;
  std::uint64_t generated_tokens=0;
  bool generating=false;
  bool approximate = true;
  bool permission_menu = false;
  size_t compactions = 0;
  std::string phase = "respond",operation;
  Json agent_status = Json::object();
  std::vector<ChatEntry> entries;
  struct MarkdownCache { std::string text; int width=0; bool streaming=false; std::vector<ChatLine> lines; };
  mutable std::vector<MarkdownCache> markdown_cache;
  void event(const std::string& type,const Json& payload);
  std::vector<std::wstring> lines(int width) const;
  std::vector<ChatLine> styled_lines(int width) const;
  std::wstring status(int width) const;
  std::wstring activity(bool busy) const;
  std::string report() const;
};
ChatAction chat_ui(ChatView view,
  const std::function<void(const std::string&,Emit)>& submit,
  const std::function<std::optional<Json>()>& receive,
  const std::function<void(const Json&)>& approve,
  const std::function<void(const std::string&,Emit)>& live_submit = {});
}
