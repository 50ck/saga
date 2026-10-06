#pragma once
#include <saga/model.hpp>
// Synthetic endpoints use the same wire adapter as real OpenAI-compatible SSE.
class MockChatBackend : public saga::ModelBackend {
public:
  virtual void chat(const saga::ChatRequest&,saga::StreamCallback)=0;
  void generate(const saga::ChatRequest& request,saga::ProviderCallback callback) override {
    saga::OpenAIStreamAdapter adapter(std::move(callback));
    chat(request,[&](const saga::Json& chunk){adapter.feed(chunk);});
    adapter.finish();
  }
};
