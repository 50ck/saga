#include <saga/ipc.hpp>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace saga {
Channel::~Channel() { if (fd_ >= 0) close(fd_); }
Channel::Channel(Channel&& other) noexcept : fd_(other.fd_),pending_(std::move(other.pending_)) { other.fd_ = -1; }
static sockaddr_un address(const fs::path& path) {
  sockaddr_un addr{}; addr.sun_family = AF_UNIX;
  auto str = path.string(); if (str.size() >= sizeof addr.sun_path) throw std::runtime_error("Unix socket path is too long");
  std::memcpy(addr.sun_path,str.c_str(),str.size()+1); return addr;
}
std::unique_ptr<Channel> Channel::connect(const fs::path& path) {
  auto addr = address(path);
  int fd = socket(AF_UNIX,SOCK_STREAM | SOCK_CLOEXEC,0);
  if (fd < 0) throw std::runtime_error("Cannot create Unix socket");
  if (::connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof addr) != 0) { close(fd); throw std::runtime_error("Cannot connect to sagad"); }
  ucred cred{}; socklen_t len = sizeof cred;
  if (getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&len) != 0 || cred.uid != getuid()) { close(fd); throw std::runtime_error("Daemon identity mismatch"); }
  auto channel=std::make_unique<Channel>(fd);
  auto token=read_file(path.parent_path()/"control.token",256);
  auto id=uuid(); channel->send({{"type","authenticate"},{"request_id",id},{"payload",{{"token",token}}}});
  auto reply=channel->receive(2000);
  if (!reply || reply->value("type","") != "result" || reply->value("request_id","") != id || !reply->at("payload").value("authenticated",false))
    throw std::runtime_error("Daemon authentication failed; restart sagad after upgrading");
  return channel;
}
void Channel::send(const Json& message) {
  struct PipeSignalGuard {
    sigset_t blocked{},old{};
    bool already_pending = false;
    PipeSignalGuard() {
      sigemptyset(&blocked); sigaddset(&blocked,SIGPIPE);
      sigset_t pending{}; sigpending(&pending); already_pending = sigismember(&pending,SIGPIPE);
      pthread_sigmask(SIG_BLOCK,&blocked,&old);
    }
    ~PipeSignalGuard() {
      if (!already_pending) { timespec zero{}; sigtimedwait(&blocked,nullptr,&zero); }
      pthread_sigmask(SIG_SETMASK,&old,nullptr);
    }
  } guard;
  std::string bytes = message.dump() + '\n';
  if (bytes.size() > 4*1024*1024) throw std::runtime_error("Protocol frame exceeds limit");
  size_t sent = 0;
  while (sent < bytes.size()) {
    auto n = write(fd_,bytes.data()+sent,bytes.size()-sent);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error(n == 0 ? "Client disconnected" : std::string("Socket send failed: ") + std::strerror(errno));
    sent += static_cast<size_t>(n);
  }
}
std::optional<Json> Channel::receive(int timeout_ms) {
  auto begin = std::chrono::steady_clock::now();
  while (true) {
    auto newline = pending_.find('\n');
    if (newline != std::string::npos) {
      auto line = pending_.substr(0,newline); pending_.erase(0,newline+1);
      if (line.empty()) continue;
      auto j = Json::parse(line); if (!j.is_object()) throw std::runtime_error("Protocol expects a JSON object"); return j;
    }
    int wait = timeout_ms;
    if (timeout_ms >= 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count();
      wait = std::max(0,timeout_ms-static_cast<int>(elapsed));
    }
    pollfd p{fd_,POLLIN,0}; int rc = poll(&p,1,wait);
    if (rc < 0 && errno == EINTR) continue;
    if (rc < 0) throw std::runtime_error("Socket poll failed");
    if (!rc) return {};
    char buffer[8192]; auto n = read(fd_,buffer,sizeof buffer);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error(n == 0 ? "Client disconnected" : std::string("Socket receive failed: ") + std::strerror(errno));
    pending_.append(buffer,static_cast<size_t>(n));
    if (pending_.size() > 4*1024*1024) throw std::runtime_error("Protocol frame exceeds limit");
  }
}
int listen_socket(const fs::path& path) {
  auto addr = address(path);
  struct stat st{};
  if (lstat(path.c_str(),&st) == 0) {
    if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) throw std::runtime_error("Refusing unsafe socket path");
    try { auto existing = Channel::connect(path); throw std::runtime_error("Daemon already running"); }
    catch (const std::exception& e) { if (std::string(e.what()) != "Cannot connect to sagad") throw; }
    fs::remove(path);
  }
  int fd = socket(AF_UNIX,SOCK_STREAM | SOCK_CLOEXEC,0);
  if (fd < 0) throw std::runtime_error("Cannot create listener");
  if (bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof addr) != 0 || chmod(path.c_str(),0600) != 0 || listen(fd,16) != 0) { close(fd); throw std::runtime_error("Cannot bind sagad socket"); }
  return fd;
}
void start_daemon(const Paths& paths,const fs::path& exe) {
  try { auto connection = Channel::connect(paths.socket()); return; } catch (const std::exception&) {}
  pid_t pid = fork(); if (pid < 0) throw std::runtime_error("Cannot start sagad");
  if (pid == 0) {
    setsid();
    pid_t second = fork(); if (second < 0) _exit(1); if (second > 0) _exit(0);
    int log = open((paths.state / "logs/sagad.log").c_str(),O_CREAT | O_WRONLY | O_APPEND | O_NOFOLLOW,0600);
    int null = open("/dev/null",O_RDONLY);
    if (null >= 0) dup2(null,0);
    if (log >= 0) { dup2(log,1); dup2(log,2); }
    syscall(SYS_close_range,3,~0U,0);
    execl(exe.c_str(),exe.c_str(),static_cast<char*>(nullptr)); _exit(127);
  }
  int status; while (waitpid(pid,&status,0) < 0 && errno == EINTR) {}
  for (int attempt = 0; attempt < 60; ++attempt) {
    try { auto connected = Channel::connect(paths.socket()); return; } catch (const std::exception&) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("sagad did not start; see XDG state logs");
}
}
