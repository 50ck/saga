#pragma once
#include <saga/common.hpp>
namespace saga {
class Channel {
  int fd_ = -1;
  std::string pending_;
public:
  explicit Channel(int fd) : fd_(fd) {}
  ~Channel();
  Channel(Channel&& other) noexcept;
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  static std::unique_ptr<Channel> connect(const fs::path& socket);
  void send(const Json& message);
  std::optional<Json> receive(int timeout_ms = -1);
  int fd() const { return fd_; }
};
int listen_socket(const fs::path& path);
void start_daemon(const Paths& paths,const fs::path& executable);
}
