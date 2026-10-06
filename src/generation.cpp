#include <saga/generation.hpp>
#include <set>

namespace saga {
std::string finish_name(FinishReason r) {
  switch(r) {case FinishReason::Stop:return "stop";case FinishReason::ToolCall:return "tool_call";case FinishReason::Length:return "length";case FinishReason::ContentFilter:return "content_filter";default:return "unknown";}
}
std::string generation_status_name(GenerationStatus s) {
  switch(s) {case GenerationStatus::Created:return "created";case GenerationStatus::Streaming:return "streaming";case GenerationStatus::Completed:return "completed";case GenerationStatus::OutputLimit:return "output_limit";case GenerationStatus::Empty:return "empty";case GenerationStatus::ReasoningOnly:return "reasoning_only";case GenerationStatus::Cancelled:return "cancelled";case GenerationStatus::ProviderError:return "provider_error";case GenerationStatus::TransportError:return "transport_error";default:return "malformed";}
}
std::string turn_phase_name(TurnPhase p) {
  switch(p) {case TurnPhase::Thinking:return "thinking";case TurnPhase::Commentary:return "commentary";case TurnPhase::Answering:return "answering";case TurnPhase::PreparingTool:return "generating_tool";case TurnPhase::ExecutingTool:return "executing_tool";case TurnPhase::Finalizing:return "finalizing";default:return "waiting_for_model";}
}
void GenerationState::accept(const ProviderEvent& e) {
  if(terminal)throw std::logic_error("Event after generation termination");
  if(e.kind==ProviderEventKind::Heartbeat)return;
  last_activity=now();saw_activity=true;status=GenerationStatus::Streaming;
  auto delta=e.kind==ProviderEventKind::Reasoning || e.kind==ProviderEventKind::Commentary || e.kind==ProviderEventKind::Text || e.kind==ProviderEventKind::ToolDelta;
  if(delta && !first_delta_at)first_delta_at=last_activity;
  if((e.kind==ProviderEventKind::Text || e.kind==ProviderEventKind::Commentary) && !e.text.empty() && !first_public_at)first_public_at=last_activity;
  auto append=[&](std::string& target,const std::string& value) {
    if(e.data.value("snapshot",false)) {
      if(!value.starts_with(target))throw ProviderError(ProviderErrorKind::StreamParse,"Final model snapshot disagrees with streamed deltas");
      target.append(value,target.size(),std::string::npos);
    }else target+=value;
  };
  switch(e.kind) {
    case ProviderEventKind::Reasoning:append(reasoning,e.text);break;
    case ProviderEventKind::Commentary:append(commentary,e.text);break;
    case ProviderEventKind::Text:append(content,e.text);break;
    case ProviderEventKind::ToolDelta: {
      int index=e.data.at("index");
      if(index<0 || index>=64)throw ProviderError(ProviderErrorKind::StreamParse,"Too many tool calls");
      auto [it,_]=calls.try_emplace(index,Json{{"id",""},{"type","function"},{"function",{{"name",""},{"arguments",""}}}});
      auto& call=it->second;
      auto id=e.data.value("id","");
      if(!id.empty() && call["id"]!=id){append(call["id"].get_ref<std::string&>(),id);}
      for(auto key:{"name","arguments"})if(e.data.contains(key)){append(call["function"][key].get_ref<std::string&>(),e.data.at(key).get_ref<const std::string&>());}
      if(call["id"].get_ref<const std::string&>().size()+call["function"]["name"].get_ref<const std::string&>().size()+call["function"]["arguments"].get_ref<const std::string&>().size()>1024*1024)throw ProviderError(ProviderErrorKind::StreamParse,"Tool call exceeds limit");
      break;
    }
    case ProviderEventKind::Usage: {
      usage.update(e.data);
      auto set=[&](const char* key,std::optional<std::uint64_t>& value){auto it=e.data.find(key);if(it!=e.data.end() && it->is_number_integer() && (it->is_number_unsigned() || it->get<std::int64_t>()>=0))value=it->get<std::uint64_t>();};
      set("input_tokens",input_tokens);set("output_tokens",output_tokens);set("cached_tokens",cache_tokens);set("reasoning_tokens",reasoning_tokens);
      break;
    }
    case ProviderEventKind::Finished: {
      finish_reason=e.finish;
      if(finish_reason==FinishReason::Unknown)throw ProviderError(ProviderErrorKind::Interrupted,"Stream ended without a finish reason",true);
      if(finish_reason==FinishReason::Length)status=GenerationStatus::OutputLimit;
      else if(finish_reason==FinishReason::ContentFilter)status=GenerationStatus::ProviderError;
      else {
        if(finish_reason==FinishReason::ToolCall && calls.empty())throw ProviderError(ProviderErrorKind::StreamParse,"Tool termination without a tool call");
        std::set<std::string> identifiers;
        for(auto& [_,call]:calls) {
          auto& function=call["function"];
          auto arguments=Json::parse(function["arguments"].get<std::string>(),[](int depth,Json::parse_event_t,Json&){if(depth>128)throw ProviderError(ProviderErrorKind::StreamParse,"Tool JSON nesting exceeds limit");return true;},false);
          auto identifier=call["id"].get<std::string>();
          if(identifier.empty() || !identifiers.insert(identifier).second || function["name"].get<std::string>().empty() || !arguments.is_object())
            throw ProviderError(ProviderErrorKind::StreamParse,"Incomplete, duplicated or invalid streamed tool call");
        }
        status=!calls.empty() || !trim(content).empty() || !trim(commentary).empty() ? GenerationStatus::Completed : !reasoning.empty() ? GenerationStatus::ReasoningOnly : GenerationStatus::Empty;
      }
      terminal=true;break;
    }
    default:break;
  }
  if(generated_bytes()>8*1024*1024 || content.size()>4*1024*1024 || reasoning.size()>4*1024*1024 || commentary.size()>1024*1024)throw ProviderError(ProviderErrorKind::StreamParse,"Generation exceeds buffer limit");
}
void GenerationState::interrupt(GenerationStatus reason) {if(terminal)return;status=reason;terminal=true;last_activity=now();}
size_t GenerationState::generated_bytes() const {size_t n=content.size()+reasoning.size()+commentary.size();for(auto& [_,c]:calls)n+=c["id"].get_ref<const std::string&>().size()+c["function"]["name"].get_ref<const std::string&>().size()+c["function"]["arguments"].get_ref<const std::string&>().size();return n;}
Json GenerationState::checkpoint() const {
  Json tools=Json::array();for(auto& [index,call]:calls)tools.push_back({{"index",index},{"call",call}});
  return {{"id",id},{"status",generation_status_name(status)},{"finish_reason",finish_name(finish_reason)},{"reasoning",reasoning},{"commentary",commentary},{"content",content},{"tool_calls",tools},{"usage",usage},{"saw_activity",saw_activity},{"started_at",started_at},{"last_activity",last_activity},{"time_to_first_delta_ms",first_delta_at?Json(first_delta_at-started_at):Json()},{"time_to_first_public_ms",first_public_at?Json(first_public_at-started_at):Json()}};
}
}
