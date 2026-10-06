#pragma once
#include <saga/common.hpp>
struct sqlite3;
namespace saga {
class Database {
  sqlite3* handle_ = nullptr;
  fs::path path_;
  bool recording_transaction_=false;
  std::vector<std::pair<std::string,Json>> pending_diagnostics_;
  void diagnostic(std::string,const Json&);
public:
  explicit Database(const fs::path& path);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  void sql(std::string_view text);
  Json query(std::string_view sql, const std::vector<Json>& params = {});
  Id exec(std::string_view sql, const std::vector<Json>& params = {});
  void transaction(const std::function<void()>& operation);
  void migrate();
  Id event(std::string_view type, const Json& payload, Id session = 0, Id task = 0);
  int changes() const;
};
}
