#pragma once
#include <saga/common.hpp>
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
};
using StreamCallback = std::function<void(const Json&)>;
class ModelBackend {
public:
  virtual ~ModelBackend() = default;
  virtual ModelInfo discover() = 0;
  virtual CapabilityReport probe() = 0;
  virtual void chat(const ChatRequest& request, StreamCallback callback) = 0;
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
struct Completion {
  std::string content, finish_reason,reasoning;
  std::map<int, Json> calls;
  Json usage = Json::object();
  std::optional<std::uint64_t> input_tokens,output_tokens,cache_tokens;
  bool saw_chunk = false;
  void accept(const Json& chunk);
  Json message() const;
};
class OpenAICompatibleBackend : public ModelBackend {
  Config config_;
  std::string base_, api_;
  bool discovered_ = false, llama_cpp_ = false;
protected:
  virtual Json request(std::string_view method, const std::string& url, const Json& body = Json(), StreamCallback cb = {});
public:
  explicit OpenAICompatibleBackend(Config config);
  static std::pair<std::string,std::string> normalize(std::string endpoint);
  std::vector<ModelInfo> models();
  ModelInfo discover() override;
  CapabilityReport probe() override;
  CapabilityReport probe(Emit progress);
  void chat(const ChatRequest&,StreamCallback) override;
  const Config& config() const { return config_; }
};
Json function_tool(std::string name, std::string description, Json properties, Json required = Json::array());
}
