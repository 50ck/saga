#pragma once
#include <saga/tools.hpp>
namespace saga {
enum class CognitiveMode { Respond, Research, Recall, Deliberate, Plan, Act, Verify, Reflect, Learn, Ask, Wait };
std::string mode_name(CognitiveMode mode);
class ExecutiveController {
public:
  static CognitiveMode route(std::string_view input);
  static bool research_requested(std::string_view input);
  static bool needs_review(const Json& task,const Json& self);
};
class ContextBuilder {
  PersonaContext& p_;
  Memory& memory_;
  Config config_;
  Id wake_session_=0;
  Id attention_session_=0,attention_cutoff_=0;
  Json wake_state_=Json::object();
public:
  ContextBuilder(PersonaContext& p,Memory& m,Config c) : p_(p),memory_(m),config_(std::move(c)) {}
  ChatRequest build(const Json& attention = Json::object(),const std::function<void()>& before_compact = {});
  static std::string core_prompt();
};
class Runtime {
  std::unique_ptr<PersonaContext> p_;
  Config config_;
  std::unique_ptr<ModelBackend> backend_;
  Memory memory_;
  Tools tools_;
  ContextBuilder context_;
  bool closed_ = false;
  Json usage_ = Json::object();
  CognitiveMode current_mode_ = CognitiveMode::Respond;
  std::function<void()> service_;
  bool active_=false,cancelled_=false;
  std::optional<TurnState> turn_;
  GenerationState last_generation_;
  void journal(std::string type,Json payload,Emit emit = {},bool durable=true);
  void phase(TurnPhase next,Emit emit);
  void finish_turn(TurnStatus status,Emit emit);
  void chat_turn(std::string input,Emit emit);
  void cancel_turn(Emit emit);
  bool steering_pending();
  void deliver_steering(Emit emit);
  GenerationState call(ChatRequest request,const std::string& purpose,Emit emit);
  Json structured(std::string name,std::string prompt,Json properties,Json required,Emit emit = {});
  void consolidate(Id session,bool use_model);
  void extract_memory(Id session,bool final = true,Emit emit = {});
  void continuity();
  void review(Emit emit);
  void mode(CognitiveMode mode,Emit emit);
public:
  Runtime(std::unique_ptr<PersonaContext> persona,Config config,std::unique_ptr<ModelBackend> backend,Approve approve,WebTransport transport = fetch_public_web);
  ~Runtime();
  void start(Emit emit = {});
  void chat(std::string input,Emit emit,std::string user_message_id = {});
  Json command(std::string name,const Json& args = Json::object(),Emit emit = {});
  void close(std::string reason,Emit emit = {});
  void discard_for_erase();
  void tick(Emit emit = {},bool use_model = true);
  PersonaContext& persona() { return *p_; }
  void service(std::function<void()> callback);
  void check_control();
  void check_cancelled() { if(cancelled_)throw TurnCancelled(); }
};
}
