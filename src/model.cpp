#include <saga/model.hpp>
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <mutex>

namespace saga {
Json ModelInfo::json() const { return {{"id",id},{"context_length",context_length ? Json(*context_length) : Json()}, {"supports_streaming",supports_streaming},{"supports_tool_calls",supports_tool_calls},{"supports_structured_output",supports_structured_output}}; }
Json CapabilityReport::json() const { return {{"connected",connected},{"completion",completion},{"streaming",streaming},{"tool_calls",tool_calls}}; }
void SseParser::line(std::string_view s) {
  if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
  if (s.empty()) {
    if (!data_.empty()) { if (data_.back() == '\n') data_.pop_back(); callback_(data_); data_.clear(); }
  } else if (s.starts_with("data:")) {
    s.remove_prefix(5); if (s.starts_with(' ')) s.remove_prefix(1);
    data_.append(s); data_ += '\n';
  }
}
void SseParser::feed(std::string_view s) {
  pending_.append(s);
  if (pending_.size() + data_.size() > 4*1024*1024) throw std::runtime_error("SSE frame exceeds limit");
  size_t begin = 0, end;
  while ((end = pending_.find('\n',begin)) != std::string::npos) { line(std::string_view(pending_).substr(begin,end-begin)); begin = end+1; }
  pending_.erase(0,begin);
}
void SseParser::finish() { if (!pending_.empty()) { line(pending_); pending_.clear(); } line(""); }
void Completion::accept(const Json& chunk) {
  if (chunk.contains("error")) throw std::runtime_error("Model returned an error");
  auto count=[](const Json& object,std::string_view key)->std::optional<std::uint64_t>{
    auto it=object.find(std::string(key));if(it==object.end() || !it->is_number_integer() || (!it->is_number_unsigned() && it->get<std::int64_t>()<0))return {};
    return it->get<std::uint64_t>();
  };
  if(chunk.contains("timings") && chunk["timings"].is_object()) {
    auto& timings=chunk["timings"];auto prompt=count(timings,"prompt_n"),cache=count(timings,"cache_n");
    if(prompt && cache && *prompt<=UINT64_MAX-*cache)input_tokens=*prompt+*cache;
    if(cache)cache_tokens=*cache;
    if(auto output=count(timings,"predicted_n"))output_tokens=*output;
  }
  if(chunk.contains("prompt_progress") && chunk["prompt_progress"].is_object()) {
    if(auto total=count(chunk["prompt_progress"],"total"))input_tokens=*total;
    if(auto cache=count(chunk["prompt_progress"],"cache"))cache_tokens=*cache;
  }
  if (chunk.contains("usage") && chunk["usage"].is_object()) {
    usage.update(chunk["usage"]);
    if(auto input=count(usage,"prompt_tokens"))input_tokens=*input;
    if(auto output=count(usage,"completion_tokens"))output_tokens=*output;
    if(usage.contains("prompt_tokens_details") && usage["prompt_tokens_details"].is_object())if(auto cache=count(usage["prompt_tokens_details"],"cached_tokens"))cache_tokens=*cache;
  }
  if (!chunk.contains("choices") || !chunk["choices"].is_array()) return;
  for (auto& choice : chunk["choices"]) {
    if (choice.value("index",0) != 0) continue;
    saw_chunk = true;
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) finish_reason = choice["finish_reason"];
    const auto& delta = choice.contains("delta") ? choice["delta"] : choice.value("message",Json::object());
    if (delta.contains("content") && delta["content"].is_string()) content += delta["content"].get<std::string>();
    for(auto* key:{"reasoning_content","reasoning"})if(delta.contains(key) && delta[key].is_string()){reasoning+=delta[key].get<std::string>();break;}
    if (delta.contains("tool_calls")) for (auto& part : delta["tool_calls"]) {
      int index = part.value("index",static_cast<int>(calls.size()));
      if (index < 0 || index >= 64) throw std::runtime_error("Too many tool calls");
      auto [it, inserted] = calls.try_emplace(index,Json{{"id",""},{"type","function"},{"function",{{"name",""},{"arguments",""}}}});
      auto& call = it->second;
      if (part.contains("id") && part["id"].is_string()) call["id"] = call["id"].get<std::string>() + part["id"].get<std::string>();
      if (part.contains("function")) for (const auto* key : {"name","arguments"})
        if (part["function"].contains(key) && part["function"][key].is_string())
          call["function"][key] = call["function"][key].get<std::string>() + part["function"][key].get<std::string>();
      (void)inserted;
    }
  }
  if (content.size() > 4*1024*1024) throw std::runtime_error("Completion exceeds limit");
  if (reasoning.size() > 4*1024*1024) throw std::runtime_error("Reasoning exceeds limit");
  for (auto& [_,call] : calls) if (call.dump().size() > 1024*1024) throw std::runtime_error("Tool arguments exceed limit");
}
Json Completion::message() const {
  Json m = {{"role","assistant"},{"content",content.empty() && !calls.empty() ? Json() : Json(content)}};
  if(!reasoning.empty())m["reasoning_content"]=reasoning;
  if (!calls.empty()) {
    m["tool_calls"] = Json::array();
    for (auto& [_,call] : calls) {
      if (call["id"].get<std::string>().empty() || call["function"]["name"].get<std::string>().empty())
        throw std::runtime_error("Incomplete streamed tool call");
      m["tool_calls"].push_back(call);
    }
  }
  return m;
}
std::pair<std::string,std::string> OpenAICompatibleBackend::normalize(std::string e) {
  e = trim(e);
  if (!(e.starts_with("https://") || e.starts_with("http://")) || e.find_first_of("\r\n\t ?#") != std::string::npos)
    throw std::runtime_error("Endpoint must be an HTTP(S) URL without a query or fragment");
  auto host_start = e.find("://")+3;
  auto slash = e.find('/',host_start);
  auto host = e.substr(host_start,slash-host_start);
  if (host.empty() || host.find('@') != std::string::npos) throw std::runtime_error("Invalid endpoint host");
  while (e.ends_with('/')) e.pop_back();
  if (e.ends_with("/v1")) e.resize(e.size()-3);
  if (e.ends_with("/v1")) throw std::runtime_error("Endpoint has repeated /v1");
  return {e,e+"/v1"};
}
OpenAICompatibleBackend::OpenAICompatibleBackend(Config c) : config_(std::move(c)) {
  static std::once_flag init;
  std::call_once(init,[]{ if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("libcurl initialization failed"); });
  auto urls = normalize(config_.endpoint); base_ = urls.first; api_ = urls.second;
  if (config_.api_key.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Invalid API key");
}
Json OpenAICompatibleBackend::request(std::string_view method, const std::string& url, const Json& body, StreamCallback cb) {
  std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
  if (!curl) throw std::runtime_error("Cannot create HTTP connection");
  curl_slist* raw_headers = nullptr;
  raw_headers = curl_slist_append(raw_headers,"Content-Type: application/json");
  raw_headers = curl_slist_append(raw_headers,"Accept: application/json, text/event-stream");
  if (!config_.api_key.empty()) raw_headers = curl_slist_append(raw_headers,("Authorization: Bearer " + config_.api_key).c_str());
  std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> headers(raw_headers,curl_slist_free_all);
  struct State {
    std::string body;
    std::exception_ptr error;
    StreamCallback callback;
    bool done = false;
    SseParser parser;
    explicit State(StreamCallback callback_arg) : callback(std::move(callback_arg)), parser([this](std::string_view data){
      if (data == "[DONE]") { done = true; return; }
      if (done) return;
      callback(Json::parse(data));
    }) {}
  } state(cb);
  auto write_cb = +[](char* bytes, size_t size, size_t count, void* data)->size_t {
    auto& state_ref = *static_cast<State*>(data); size_t n = size*count;
    try {
      if (state_ref.body.size()+n > 16*1024*1024) throw std::runtime_error("HTTP response exceeds limit");
      state_ref.body.append(bytes,n);
      if (state_ref.callback) state_ref.parser.feed({bytes,n});
      return n;
    } catch (...) { state_ref.error = std::current_exception(); return 0; }
  };
  auto set = [&](CURLoption option,auto value){ if (curl_easy_setopt(curl.get(),option,value) != CURLE_OK) throw std::runtime_error("Cannot configure HTTP request"); };
  set(CURLOPT_URL,url.c_str()); set(CURLOPT_HTTPHEADER,headers.get());
  set(CURLOPT_WRITEFUNCTION,write_cb); set(CURLOPT_WRITEDATA,&state);
  set(CURLOPT_NOSIGNAL,1L); set(CURLOPT_CONNECTTIMEOUT,10L);
  // Non-streaming inference sends no response bytes until generation finishes.
  // A low-speed limit mistakes legitimate model computation for a dead connection.
  long timeout = method == "GET" ? std::min(config_.timeout_seconds,30) : config_.timeout_seconds;
  set(CURLOPT_TIMEOUT,timeout);
  set(CURLOPT_SSL_VERIFYPEER,config_.insecure_tls ? 0L : 1L);
  set(CURLOPT_SSL_VERIFYHOST,config_.insecure_tls ? 0L : 2L);
  set(CURLOPT_PROTOCOLS_STR,"http,https");
  std::string serialized;
  if (method == "POST") { serialized = body.dump(); set(CURLOPT_POST,1L); set(CURLOPT_POSTFIELDS,serialized.data()); set(CURLOPT_POSTFIELDSIZE,static_cast<long>(serialized.size())); }
  auto rc = curl_easy_perform(curl.get());
  if (state.error) std::rethrow_exception(state.error);
  if (rc == CURLE_OPERATION_TIMEDOUT) throw std::runtime_error("Model request timed out (response limit " + std::to_string(timeout) + "s, connection limit 10s). Increase timeout_seconds for slow local models.");
  if (rc != CURLE_OK) throw std::runtime_error(std::string("Model connection failed: ")+curl_easy_strerror(rc));
  long status = 0; curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status);
  // Never include server bodies (may echo credentials) in error logs.
  if (status < 200 || status >= 300) throw std::runtime_error("Model HTTP status " + std::to_string(status));
  if (cb) {
    state.parser.finish();
    if (!state.done) throw std::runtime_error("Interrupted SSE response: missing [DONE]");
    return Json::object();
  }
  try { return Json::parse(state.body); }
  catch (const Json::exception&) { throw std::runtime_error("Invalid JSON from model backend"); }
}
static std::optional<std::uint64_t> context_metadata(const Json& value) {
  // Training context is deliberately excluded. Only effective serving metadata.
  for (auto* name : {"n_ctx","context_length","max_context_length"})
    if (value.contains(name) && value[name].is_number_unsigned()) return value[name].get<std::uint64_t>();
  for (auto* name : {"meta","default_generation_settings","params"})
    if (value.contains(name) && value[name].is_object()) if (auto n = context_metadata(value[name])) return n;
  return {};
}
std::vector<ModelInfo> OpenAICompatibleBackend::models() {
  auto r = request("GET",api_ + "/models");
  std::vector<ModelInfo> out;
  if (!r.contains("data") || !r["data"].is_array()) throw std::runtime_error("Invalid /v1/models response");
  for (auto& m : r["data"]) {
    if (!m.contains("id") || !m["id"].is_string()) continue;
    if (m.value("owned_by","") == "llamacpp" && (m["id"] == config_.model || (config_.model.empty() && r["data"].size() == 1))) llama_cpp_ = true;
    ModelInfo info; info.id = m["id"]; info.context_length = context_metadata(m); out.push_back(std::move(info));
  }
  if (out.empty()) throw std::runtime_error("Endpoint exposes no models");
  return out;
}
ModelInfo OpenAICompatibleBackend::discover() {
  auto list = models();
  auto it = std::find_if(list.begin(),list.end(),[&](auto& info){ return info.id == config_.model; });
  if (config_.model.empty() && list.size() == 1) it = list.begin();
  if (it == list.end()) throw std::runtime_error("Select one of the endpoint's models");
  auto info = *it;
  if (!info.context_length) {
    try {
      auto props = request("GET",base_ + "/props");
      info.context_length = context_metadata(props);
      if (props.contains("default_generation_settings")) llama_cpp_ = true;
    } catch (const std::exception&) { /* optional endpoint */ }
  }
  config_.model = info.id;
  if (config_.context_length == 0 && info.context_length) config_.context_length = *info.context_length;
  discovered_ = true;
  return info;
}
Json function_tool(std::string name,std::string description,Json properties,Json required) {
  return {{"type","function"},{"function",{{"name",std::move(name)},{"description",std::move(description)},
    {"parameters",{{"type","object"},{"properties",std::move(properties)},{"required",std::move(required)},{"additionalProperties",false}}}}}};
}
CapabilityReport OpenAICompatibleBackend::probe() { return probe({}); }
CapabilityReport OpenAICompatibleBackend::probe(Emit progress) {
  if (!discovered_) discover();
  auto status = [&](std::string stage,std::string message,bool passed = false){
    if (progress) progress("backend.progress",{{"stage",stage},{"message",message},{"status",passed ? "passed" : "running"}});
  };
  CapabilityReport report;
  Json body = {{"model",config_.model},{"messages",Json::array({{{"role","user"},{"content","Reply only OK."}}})},{"max_tokens",llama_cpp_ ? 32 : 256},{"stream",false}};
  if (llama_cpp_) {
    body["chat_template_kwargs"] = {{"enable_thinking",false}};
    body["reasoning_effort"] = "none";
  }
  status("completion","Testing basic completion");
  auto basic = request("POST",api_ + "/chat/completions",body);
  report.connected = true;
  Completion full; full.accept(basic); report.completion = !trim(full.content).empty();
  if (!report.completion) throw std::runtime_error("Incompatible backend: completion probe returned no answer");
  status("completion","Basic completion",true);
  body["messages"][0]["content"] = "Invoke saga_probe with ok=true.";
  body["tools"] = Json::array({function_tool("saga_probe","Verify function calling",{{"ok",{{"type","boolean"}}}},{"ok"})});
  body["tool_choice"] = "required";
  body["max_tokens"] = llama_cpp_ ? 64 : 512;
  status("tools","Testing function calling");
  Completion tool; tool.accept(request("POST",api_ + "/chat/completions",body));
  if (tool.calls.size() == 1) {
    auto call = tool.message()["tool_calls"][0];
    auto args = Json::parse(call["function"]["arguments"].get<std::string>());
    report.tool_calls = call["function"]["name"] == "saga_probe" && args.is_object() && args.value("ok",false);
  }
  if (!report.tool_calls) throw std::runtime_error("Incompatible backend: probe did not return a valid saga_probe function call");
  status("tools","Function calling",true);
  ChatRequest streaming;
  streaming.messages = Json::array({{{"role","user"},{"content","Reply only OK."}}}); streaming.max_tokens = llama_cpp_ ? 32 : 256;
  streaming.disable_thinking = llama_cpp_;
  status("streaming","Testing streamed completion");
  Completion stream; chat(streaming,[&](auto& chunk){ stream.accept(chunk); });
  report.streaming = stream.saw_chunk && !stream.content.empty() && !stream.finish_reason.empty();
  if (!report.completion || !report.streaming || !report.tool_calls) throw std::runtime_error("Incompatible backend: basic completion, streaming, and valid tool calling are required");
  status("streaming","Streaming",true);
  return report;
}
void OpenAICompatibleBackend::chat(const ChatRequest& r,StreamCallback cb) {
  if (config_.model.empty()) throw std::runtime_error("No selected model");
  // Metadata only, once per backend. No generated capability tests on startup.
  if(!discovered_){try{models();}catch(const std::exception&){}discovered_=true;}
  Json body = {{"model",config_.model},{"messages",r.messages},{"stream",true},{"stream_options",{{"include_usage",true}}},{"max_tokens",r.max_tokens}};
  if(llama_cpp_){body["timings_per_token"]=true;body["return_progress"]=true;body["cache_prompt"]=true;body["chat_template_kwargs"]={{"preserve_thinking",true}};}
  if (llama_cpp_ && r.disable_thinking) { body["chat_template_kwargs"]["enable_thinking"]=false; body["reasoning_effort"] = "none"; }
  if (!r.tools.empty()) body["tools"] = r.tools;
  if (r.forced_tool) {
    Json selected = Json::array();
    for (const auto& tool : r.tools) if (tool.at("function").at("name") == *r.forced_tool) selected.push_back(tool);
    if (selected.size() != 1) throw std::runtime_error("Forced tool must have exactly one matching definition");
    body["tools"] = std::move(selected);
    body["tool_choice"] = "required";
  }
  bool emitted = false;
  auto wrapped = [&](const Json& chunk){ emitted = true; cb(chunk); };
  try { request("POST",api_ + "/chat/completions",body,wrapped); }
  catch (const std::exception& e) {
    if (!emitted && std::string(e.what()) == "Model HTTP status 400") {
      body.erase("stream_options");body.erase("timings_per_token");body.erase("return_progress");body.erase("cache_prompt");
      if(body.contains("chat_template_kwargs")){body["chat_template_kwargs"].erase("preserve_thinking");if(body["chat_template_kwargs"].empty())body.erase("chat_template_kwargs");}
      request("POST",api_ + "/chat/completions",body,wrapped);
    } else throw;
  }
}
}
