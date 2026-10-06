#pragma once
#include <saga/generation.hpp>
#include <map>
namespace saga {
struct ModelInfo {
  std::string id;
  std::optional<std::uint64_t> context_length;
  bool supports_streaming = false, supports_tool_calls = false, supports_structured_output = false;
  Json json() const;
};
struct CapabilityReport { bool connected = false, completion = false, streaming = false, tool_calls = false; Json json() const; };
struct ChatRequest {
  Json messages = Json::array(), tools = Json::array();
  std::optional<std::string> forced_tool;
  std::uint64_t max_tokens = 8192;
  bool disable_thinking = false;
  std::uint64_t reasoning_soft_tokens=0,reasoning_hard_tokens=0;
  bool enable_reasoning_control=false;
};
// An empty object is a local keepalive while HTTP inference has no SSE bytes yet.
using StreamCallback = std::function<void(const Json&)>;
class ModelBackend {
protected:
  std::function<void()> control_;
public:
    virtual ~ModelBackend() = default;
    void control(std::function<void()> callback) { control_=std::move(callback); }
  virtual ModelInfo discover() = 0;
  virtual CapabilityReport probe() = 0;
  virtual void generate(const ChatRequest& request,ProviderCallback callback) = 0;
  virtual void prepare() {}
  virtual ProviderCapabilities capabilities() const { return {}; }
  virtual std::optional<std::uint64_t> max_output_tokens() const { return {}; }
};
class SseParser {
  std::string pending_, data_;
  std::function<void(std::string_view)> callback_;
  void line(std::string_view s);
public:
  explicit SseParser(std::function<void(std::string_view)> cb) : callback_(std::move(cb)) {}
  void feed(std::string_view bytes);
  void finish();
};
// OpenAI wire syntax lives here, never in the cognitive runtime.
class OpenAIStreamAdapter {
  ProviderCallback emit_;
  FinishReason finish_=FinishReason::Unknown;
  bool closed_=false;
  std::string provider_id_;
  std::map<std::string,int> tool_ids_;
  int next_tool_=0;
public:
  explicit OpenAIStreamAdapter(ProviderCallback emit) : emit_(std::move(emit)) {}
  void feed(const Json& chunk);
  void finish();
  bool has_finish() const { return finish_!=FinishReason::Unknown; }
  const std::string& provider_id() const { return provider_id_; }
};
class OpenAICompatibleBackend : public ModelBackend {
  Config config_;
  std::string base_, api_;
  bool discovered_ = false, llama_cpp_ = false;
  std::optional<std::uint64_t> output_limit_;
  ProviderCapabilities capabilities_;
  bool reasoning_control_checked_=false;
protected:
  void stream_chat(const ChatRequest&,StreamCallback);
  virtual Json request(std::string_view method, const std::string& url, const Json& body = Json(), StreamCallback cb = {});
public:
  explicit OpenAICompatibleBackend(Config config);
  static std::pair<std::string,std::string> normalize(std::string endpoint);
  std::vector<ModelInfo> models();
  ModelInfo discover() override;
  CapabilityReport probe() override;
  CapabilityReport probe(Emit progress);
  void generate(const ChatRequest&,ProviderCallback) override;
  void prepare() override;
  ProviderCapabilities capabilities() const override { return capabilities_; }
  std::optional<std::uint64_t> max_output_tokens() const override { return output_limit_; }
  const Config& config() const { return config_; }
};
Json function_tool(std::string name, std::string description, Json properties, Json required = Json::array());
}
