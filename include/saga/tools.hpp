#pragma once
#include <saga/memory.hpp>
#include <saga/model.hpp>
namespace saga {
struct ProcessResult {
  int exit_code = -1;
  bool timed_out = false;
  std::string out, err;
  std::uint64_t stdout_size = 0, stderr_size = 0;
  bool stdout_truncated = false, stderr_truncated = false;
  Json json() const;
};
bool shell_sandbox_available();
ProcessResult run_process(const fs::path& cwd, const fs::path& scratch, const std::vector<std::string>& argv, int timeout_seconds = 30,
                          bool host = false,const std::vector<fs::path>& private_roots = {},const Json& environment = Json::object(),bool unrestricted = false);
using Approve = std::function<bool(const std::string&,const Json&)>;
class Tools {
  PersonaContext& p_;
  Memory& memory_;
  Approve approve_;
  fs::path safe_path(const std::string& path, bool write) const;
  Json dispatch(const std::string& name, const Json& args);
public:
  Tools(PersonaContext& p,Memory& memory,Approve approve) : p_(p),memory_(memory),approve_(std::move(approve)) {}
  static Json definitions();
  static void validate(const Json& args, const Json& schema);
  Json execute(const std::string& name,const Json& args,Emit emit = {});
  Json environment();
  Json permissions(const std::optional<std::string>& mode = {});
};
}
