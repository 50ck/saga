#pragma once
#include <saga/common.hpp>
#include <map>

namespace saga {
enum class FinishReason { Unknown, Stop, ToolCall, Length, ContentFilter };
enum class GenerationStatus { Created, Streaming, Completed, OutputLimit, Empty, ReasoningOnly, Cancelled, ProviderError, TransportError, Malformed };
enum class TurnStatus { Active, Completed, Failed, Cancelled };
enum class TurnPhase { Waiting, Thinking, Commentary, Answering, PreparingTool, ExecutingTool, Finalizing };
enum class ProviderEventKind { Heartbeat, Activity, Reasoning, Commentary, Text, ToolDelta, Usage, Finished, Warning };
struct ProviderEvent {
  ProviderEventKind kind;
  std::string text{};
  Json data = Json::object();
  FinishReason finish = FinishReason::Unknown;
};
using ProviderCallback = std::function<void(const ProviderEvent&)>;
struct ProviderCapabilities {
  bool streaming=true, reasoning=true, tool_calls=true, usage_in_stream=true;
  bool native_commentary=false, reasoning_control=false;
};
enum class ProviderErrorKind { Http, Connection, Timeout, StreamParse, Semantic, Interrupted };
struct ProviderError : std::runtime_error {
  ProviderErrorKind kind;
  bool transient;
  ProviderError(ProviderErrorKind k,std::string message,bool retryable=false)
    : std::runtime_error(std::move(message)),kind(k),transient(retryable) {}
};
std::string finish_name(FinishReason reason);
std::string generation_status_name(GenerationStatus status);
std::string turn_phase_name(TurnPhase phase);
struct GenerationState {
  Id id=0,started_at=0,last_activity=0,first_delta_at=0,first_public_at=0;
  GenerationStatus status=GenerationStatus::Created;
  FinishReason finish_reason=FinishReason::Unknown;
  std::string content,reasoning,commentary;
  std::map<int,Json> calls;
  Json usage=Json::object();
  std::optional<std::uint64_t> input_tokens,output_tokens,cache_tokens,reasoning_tokens;
  bool saw_activity=false,terminal=false;
  void accept(const ProviderEvent& event);
  void interrupt(GenerationStatus reason);
  Json message() const;
  Json checkpoint() const;
  size_t generated_bytes() const;
};
// A logical operator turn owns its model segments and tool executions.
struct TurnState {
  std::string id,user_message_id;
  TurnStatus status=TurnStatus::Active;
  TurnPhase phase=TurnPhase::Waiting;
  std::uint64_t sequence=0,input_tokens=0,output_tokens=0;
  unsigned generations=0,continuations=0,retries=0;
};
}
