#pragma once
#include <saga/persona.hpp>
namespace saga {
class Memory {
  PersonaContext& p_;
  Config config_;
public:
  Memory(PersonaContext& persona, Config config) : p_(persona), config_(std::move(config)) {}
  Json search(std::string kind, std::string query, bool deep = false);
  Json recall(const std::string& kind, Id id, Id offset = 0, Id limit = 12000);
  Id episode(const Json& data, Id session);
  Id fact(const std::string& subject, const std::string& predicate, const std::string& object, Id source, bool correction = false, Id project = 0);
  Id evidence(std::string type, Id id, std::string kind, bool support, Id source, double weight = 1, double reliability = 0.8);
  double confidence(std::string type, Id id, double prior = 0.5);
  Id candidate(const Json& data, Id source);
  void praxis_outcome(Id praxis, Id task, bool success, Id source);
  void artifact(const fs::path& path, std::string description, std::string_view contents, bool content_hash = true);
  void maintain();
  Json wake();
  Json project_context();
  Json self();
  Json checkpoint(std::string reason,Id through,const Json& working = Json::object());
  Json handoff();
  void entity_links(Id episode, const Json& entities);
};
}
