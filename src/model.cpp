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
  if (pending_.size() + data_.size() > 4*1024*1024) throw ProviderError(ProviderErrorKind::StreamParse,"SSE frame exceeds limit");
  size_t begin = 0, end;
  while ((end = pending_.find('\n',begin)) != std::string::npos) { line(std::string_view(pending_).substr(begin,end-begin)); begin = end+1; }
  pending_.erase(0,begin);
}
void SseParser::finish() { if (!pending_.empty()) { line(pending_); pending_.clear(); } line(""); }
void OpenAIStreamAdapter::feed(const Json& chunk) {
  if(closed_)throw ProviderError(ProviderErrorKind::StreamParse,"Data after provider termination");
  if(!chunk.is_object())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid model stream chunk");
  if(chunk.empty()){emit_({ProviderEventKind::Heartbeat});return;}
  if(chunk.contains("error"))throw ProviderError(ProviderErrorKind::Semantic,"Model returned a provider error");
  if(chunk.contains("id") && chunk["id"].is_string())provider_id_=chunk["id"];
  emit_({ProviderEventKind::Activity});
  auto count=[](const Json& object,const char* key)->std::optional<std::uint64_t>{
    auto it=object.find(key);if(it==object.end() || !it->is_number_integer() || (!it->is_number_unsigned() && it->get<std::int64_t>()<0))return {};
    return it->get<std::uint64_t>();
  };
  Json usage=Json::object();
  auto put=[&](const Json& object,const char* source,const char* target){if(auto n=count(object,source))usage[target]=*n;};
  if(chunk.contains("timings") && chunk["timings"].is_object()) {
    auto& t=chunk["timings"];auto prompt=count(t,"prompt_n"),cache=count(t,"cache_n");
    if(prompt && cache && *prompt<=UINT64_MAX-*cache)usage["input_tokens"]=*prompt+*cache;
    put(t,"cache_n","cached_tokens");put(t,"predicted_n","output_tokens");
  }
  if(chunk.contains("prompt_progress") && chunk["prompt_progress"].is_object()) {
    auto& p=chunk["prompt_progress"];put(p,"total","input_tokens");put(p,"cache","cached_tokens");
  }
  if(chunk.contains("usage") && chunk["usage"].is_object()) {
    auto& u=chunk["usage"];put(u,"prompt_tokens","input_tokens");put(u,"completion_tokens","output_tokens");
    if(u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object())put(u["prompt_tokens_details"],"cached_tokens","cached_tokens");
    if(u.contains("completion_tokens_details") && u["completion_tokens_details"].is_object())put(u["completion_tokens_details"],"reasoning_tokens","reasoning_tokens");
  }
  if(!usage.empty())emit_({ProviderEventKind::Usage,"",usage});
  if(!chunk.contains("choices"))return;
  if(!chunk["choices"].is_array())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid model choices");
  for(auto& choice:chunk["choices"]) {
    if(!choice.is_object())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid model choice");
    if(choice.contains("index") && !choice["index"].is_number_integer())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid model choice index");
    if(choice.contains("index") && choice["index"]!=0)continue; // Saga requests exactly one candidate.
    bool full=!choice.contains("delta");
    auto delta=full ? choice.value("message",Json::object()) : choice["delta"];
    if(!delta.is_object())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid model delta");
    // Full messages are normalized as snapshots. The state owner reconciles
    // them against its canonical buffers instead of duplicating those buffers.
    {
      for(auto [key,kind]:{std::pair{"reasoning_content",ProviderEventKind::Reasoning},{"reasoning",ProviderEventKind::Reasoning},{"content",ProviderEventKind::Text},{"commentary",ProviderEventKind::Commentary}}) {
        auto it=delta.find(key);if(it==delta.end() || it->is_null())continue;
        if(!it->is_string())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid model text delta");
        if(key==std::string_view("reasoning") && delta.contains("reasoning_content"))continue;
        if(!it->get_ref<const std::string&>().empty()){emit_({kind,it->get<std::string>(),{{"snapshot",full}}});}
      }
      if(delta.contains("tool_calls")) {
        if(!delta["tool_calls"].is_array())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid model tool delta");
        int fallback=0;
        for(auto& part:delta["tool_calls"]) {
          int ordinal=fallback++;
          if(!part.is_object())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid model tool fragment");
          int index=0;
          auto id=part.contains("id") && part["id"].is_string()?part["id"].get<std::string>():std::string();
          if(part.contains("index")) {
            auto& value=part["index"];
            if(!value.is_number_integer() || value<0 || value>=64)throw ProviderError(ProviderErrorKind::StreamParse,"Invalid tool call index");
            index=value.get<int>();
          }
          else if(!id.empty() && tool_ids_.contains(id))index=tool_ids_.at(id);
          else if(full)index=ordinal;
          else if(!id.empty())index=next_tool_;
          else if(tool_ids_.size()==1)index=tool_ids_.begin()->second;
          else throw ProviderError(ProviderErrorKind::StreamParse,"Ambiguous tool call fragment without index or id");
          if(index<0 || index>=64)throw ProviderError(ProviderErrorKind::StreamParse,"Too many tool calls");
          next_tool_=std::max(next_tool_,index+1);if(!id.empty())tool_ids_[id]=index;
          Json data={{"index",index},{"snapshot",full}};
          if(part.contains("type") && !part["type"].is_null() && part["type"]!="function")throw ProviderError(ProviderErrorKind::StreamParse,"Unsupported tool call type");
          if(part.contains("id") && !part["id"].is_null()){if(!part["id"].is_string())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid tool id");data["id"]=part["id"];}
          if(part.contains("function") && !part["function"].is_null()) {
            if(!part["function"].is_object())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid tool function");
            for(auto key:{"name","arguments"})if(part["function"].contains(key) && !part["function"][key].is_null()){if(!part["function"][key].is_string())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid tool string");data[key]=part["function"][key];}
          }
          emit_({ProviderEventKind::ToolDelta,"",data});
        }
      }
    }
    if(choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
      if(!choice["finish_reason"].is_string())throw ProviderError(ProviderErrorKind::StreamParse,"Invalid finish reason");
      auto reason=choice["finish_reason"].get<std::string>();
      finish_=reason=="stop"?FinishReason::Stop:reason=="tool_calls" || reason=="function_call"?FinishReason::ToolCall:reason=="length"?FinishReason::Length:reason=="content_filter"?FinishReason::ContentFilter:FinishReason::Unknown;
    }
  }
}
void OpenAIStreamAdapter::finish() {
  if(closed_)throw std::logic_error("Provider stream terminated twice");
  if(!has_finish())throw ProviderError(ProviderErrorKind::Interrupted,"Interrupted provider stream: no finish reason",true);
  closed_=true;emit_({ProviderEventKind::Finished,"",Json::object(),finish_});
}
Json GenerationState::message() const {
  auto public_text=commentary.empty()?content:commentary+(content.empty()?"":"\n\n"+content);
  Json m = {{"role","assistant"},{"content",public_text.empty() && !calls.empty() ? Json() : Json(public_text)}};
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
    std::function<void()> control;
    bool done = false, finished = false;
    CURL* handle=nullptr;
    std::chrono::steady_clock::time_point last_activity=std::chrono::steady_clock::now();
    int idle_timeout=600;
    SseParser parser;
    explicit State(StreamCallback callback_arg) : callback(std::move(callback_arg)), parser([this](std::string_view data){
      if (data == "[DONE]") { done = true; return; }
      if (done) return;
      Json chunk;try{chunk=Json::parse(data,[](int depth,Json::parse_event_t,Json&){if(depth>128)throw ProviderError(ProviderErrorKind::StreamParse,"Model JSON nesting exceeds limit");return true;});}catch(const Json::exception&){throw ProviderError(ProviderErrorKind::StreamParse,"Invalid JSON in model stream");}
      if(chunk.contains("choices") && chunk["choices"].is_array())for(auto& choice:chunk["choices"])if(choice.is_object() && choice.contains("finish_reason") && choice["finish_reason"].is_string())finished=true;
      callback(chunk);
    }) {}
  } state(cb);
  state.control=control_;state.handle=curl.get();state.idle_timeout=config_.timeout_seconds;
  auto write_cb = +[](char* bytes, size_t size, size_t count, void* data)->size_t {
    auto& state_ref = *static_cast<State*>(data); size_t n = size*count;
    try {
      state_ref.last_activity=std::chrono::steady_clock::now();
      long status=0;curl_easy_getinfo(state_ref.handle,CURLINFO_RESPONSE_CODE,&status);
      if (state_ref.body.size()+n > 16*1024*1024) throw std::runtime_error("HTTP response exceeds limit");
      if(!state_ref.callback || status>=300)state_ref.body.append(bytes,n);
      if (state_ref.callback && status>=200 && status<300) state_ref.parser.feed({bytes,n});
      return n;
    } catch (...) { state_ref.error = std::current_exception(); return 0; }
  };
  auto set = [&](CURLoption option,auto value){ if (curl_easy_setopt(curl.get(),option,value) != CURLE_OK) throw std::runtime_error("Cannot configure HTTP request"); };
  set(CURLOPT_URL,url.c_str()); set(CURLOPT_HTTPHEADER,headers.get());
  set(CURLOPT_WRITEFUNCTION,write_cb); set(CURLOPT_WRITEDATA,&state);
  if (cb || control_) {
    // Service local controls even when inference has not sent any SSE bytes yet.
    auto progress = +[](void* data,curl_off_t,curl_off_t,curl_off_t,curl_off_t)->int {
      auto& state_ref=*static_cast<State*>(data);
      try { if(state_ref.control)state_ref.control();
        if(state_ref.callback && std::chrono::steady_clock::now()-state_ref.last_activity>std::chrono::seconds(state_ref.idle_timeout))throw ProviderError(ProviderErrorKind::Timeout,"Model stream idle timeout",true);
        if(state_ref.callback)state_ref.callback(Json::object());
        return 0; }
      catch (...) { state_ref.error=std::current_exception(); return 1; }
    };
    set(CURLOPT_NOPROGRESS,0L); set(CURLOPT_XFERINFOFUNCTION,progress); set(CURLOPT_XFERINFODATA,&state);
  }
  set(CURLOPT_NOSIGNAL,1L); set(CURLOPT_CONNECTTIMEOUT,10L);
  // Non-streaming inference sends no response bytes until generation finishes.
  // A low-speed limit mistakes legitimate model computation for a dead connection.
  long timeout = method == "GET" ? std::min(config_.timeout_seconds,30) : config_.timeout_seconds;
  set(CURLOPT_TIMEOUT,cb ? 0L : timeout);
  set(CURLOPT_SSL_VERIFYPEER,config_.insecure_tls ? 0L : 1L);
  set(CURLOPT_SSL_VERIFYHOST,config_.insecure_tls ? 0L : 2L);
  set(CURLOPT_PROTOCOLS_STR,"http,https");
  std::string serialized;
  if (method == "POST") { serialized = body.dump(); set(CURLOPT_POST,1L); set(CURLOPT_POSTFIELDS,serialized.data()); set(CURLOPT_POSTFIELDSIZE,static_cast<long>(serialized.size())); }
  auto rc = curl_easy_perform(curl.get());
  if (state.error) std::rethrow_exception(state.error);
  if (rc == CURLE_OPERATION_TIMEDOUT) throw ProviderError(ProviderErrorKind::Timeout,"Model request timed out (idle/response limit " + std::to_string(timeout) + "s, connection limit 10s)",true);
  if (rc != CURLE_OK) throw ProviderError(ProviderErrorKind::Connection,std::string("Model connection failed: ")+curl_easy_strerror(rc),true);
  long status = 0; curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status);
  // Never include server bodies (may echo credentials) in error logs.
  if (status < 200 || status >= 300) throw ProviderError(ProviderErrorKind::Http,"Model HTTP status " + std::to_string(status),status==408 || status==429 || status>=500);
  if (cb) {
    state.parser.finish();
    if (!state.done && !state.finished) throw ProviderError(ProviderErrorKind::Interrupted,"Interrupted SSE response: missing terminal event",true);
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
    ModelInfo info; info.id = m["id"]; info.context_length = context_metadata(m);
    if(info.id==config_.model && m.contains("max_output_tokens") && m["max_output_tokens"].is_number_unsigned())output_limit_=m["max_output_tokens"].get<std::uint64_t>();
    out.push_back(std::move(info));
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
  if(required.is_null())required=Json::array();
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
  GenerationState full; OpenAIStreamAdapter basic_adapter([&](auto& e){full.accept(e);});basic_adapter.feed(basic);basic_adapter.finish(); report.completion = !trim(full.content).empty();
  if (!report.completion) throw std::runtime_error("Incompatible backend: completion probe returned no answer");
  status("completion","Basic completion",true);
  body["messages"][0]["content"] = "Invoke saga_probe with ok=true.";
  body["tools"] = Json::array({function_tool("saga_probe","Verify function calling",{{"ok",{{"type","boolean"}}}},{"ok"})});
  body["tool_choice"] = "required";
  body["max_tokens"] = llama_cpp_ ? 64 : 512;
  status("tools","Testing function calling");
  GenerationState tool;OpenAIStreamAdapter tool_adapter([&](auto& e){tool.accept(e);});tool_adapter.feed(request("POST",api_ + "/chat/completions",body));tool_adapter.finish();
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
  GenerationState stream; generate(streaming,[&](auto& e){ stream.accept(e); });
  report.streaming = stream.saw_activity && !stream.content.empty() && stream.finish_reason!=FinishReason::Unknown;
  if (!report.completion || !report.streaming || !report.tool_calls) throw std::runtime_error("Incompatible backend: basic completion, streaming, and valid tool calling are required");
  status("streaming","Streaming",true);
  return report;
}
void OpenAICompatibleBackend::prepare() {
  if(discovered_)return;
  try{models();}catch(const TurnCancelled&){throw;}catch(const std::exception&){}
  discovered_=true;
}
void OpenAICompatibleBackend::stream_chat(const ChatRequest& r,StreamCallback cb) {
  if (config_.model.empty()) throw std::runtime_error("No selected model");
  // Metadata only, once per backend. No generated capability tests on startup.
  prepare();
  Json body = {{"model",config_.model},{"messages",r.messages},{"stream",true},{"stream_options",{{"include_usage",true}}},{"max_tokens",r.max_tokens}};
  if(llama_cpp_){body["timings_per_token"]=true;body["return_progress"]=true;body["cache_prompt"]=true;body["chat_template_kwargs"]={{"preserve_thinking",true}};}
  if(llama_cpp_ && r.enable_reasoning_control) {
    // The documented control endpoint is probed with an impossible completion
    // id. It performs no inference and cannot stop any real user generation.
    if(!reasoning_control_checked_) {
      reasoning_control_checked_=true;
      auto previous_timeout=config_.timeout_seconds;config_.timeout_seconds=2;
      try {auto result=request("POST",api_+"/chat/completions/control",{{"id","saga-capability-"+uuid()},{"model",config_.model},{"action","reasoning_end"}});capabilities_.reasoning_control=result.contains("success") && result["success"].is_boolean();}
      catch(const TurnCancelled&){config_.timeout_seconds=previous_timeout;throw;}
      catch(const std::exception&){}
      config_.timeout_seconds=previous_timeout;
    }
    if(capabilities_.reasoning_control)body["reasoning_control"]=true;
  }
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
  auto wrapped = [&](const Json& chunk){ if (!chunk.empty()) emitted = true; cb(chunk); };
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

namespace saga {
void OpenAICompatibleBackend::generate(const ChatRequest& r,ProviderCallback cb) {
  std::uint64_t reasoning_bytes=0;
  bool soft_sent=false,hard_sent=false;
  OpenAIStreamAdapter adapter([&](const ProviderEvent& event){
    cb(event);
    if(event.kind!=ProviderEventKind::Reasoning)return;
    reasoning_bytes=event.data.value("snapshot",false)?event.text.size():reasoning_bytes+event.text.size();auto tokens=(reasoning_bytes+2)/3;
    if(r.reasoning_soft_tokens && tokens>=r.reasoning_soft_tokens && !soft_sent) {
      soft_sent=true;cb({ProviderEventKind::Warning,"Reasoning soft budget reached",{{"tokens_estimate",tokens},{"threshold",r.reasoning_soft_tokens}}});
    }
    if(r.reasoning_hard_tokens && tokens>=r.reasoning_hard_tokens && !hard_sent) {
      hard_sent=true;
      bool ended=false;
      if(r.enable_reasoning_control && capabilities_.reasoning_control && !adapter.provider_id().empty()) {
        auto previous_timeout=config_.timeout_seconds;config_.timeout_seconds=2;
        try {auto result=request("POST",api_+"/chat/completions/control",{{"id",adapter.provider_id()},{"model",config_.model},{"action","reasoning_end"}});ended=result.value("success",false);}
        catch(const TurnCancelled&){config_.timeout_seconds=previous_timeout;throw;}
        catch(const std::exception&){}
        config_.timeout_seconds=previous_timeout;
      }
      cb({ProviderEventKind::Warning,ended?"Requested end of reasoning":"Reasoning hard budget reached; preserving the active stream",{{"tokens_estimate",tokens},{"control_requested",ended}}});
    }
  });
  try {stream_chat(r,[&](const Json& chunk){adapter.feed(chunk);});}
  catch(const ProviderError& error) {
    if(!adapter.has_finish() || (error.kind!=ProviderErrorKind::Connection && error.kind!=ProviderErrorKind::Interrupted))throw;
    cb({ProviderEventKind::Warning,"Transport ended after a valid terminal marker; retaining the generation"});
  }
  adapter.finish();
}
}
