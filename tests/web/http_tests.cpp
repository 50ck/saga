#include "../check.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <mutex>
#include <poll.h>
#include <saga/web/acquisition.hpp>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using namespace saga;
using namespace saga::web;
namespace {
struct Server {
  int listener = -1;
  std::jthread worker;
  std::mutex mutex;
  std::vector<std::string> requests;
  unsigned port = 0;
  Server() {
    listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0)
      throw std::runtime_error("Socket unavailable");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address) < 0) {
      close(listener);
      listener = -1;
      throw std::runtime_error("Socket unavailable");
    }
    socklen_t n = sizeof address;
    getsockname(listener, reinterpret_cast<sockaddr *>(&address), &n);
    port = ntohs(address.sin_port);
    listen(listener, 8);
    worker = std::jthread([this](std::stop_token stop) {
      while (!stop.stop_requested()) {
        pollfd item{listener, POLLIN, 0};
        if (poll(&item, 1, 100) <= 0)
          continue;
        int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0)
          continue;
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
          auto count = recv(fd, buffer, sizeof buffer, 0);
          if (count <= 0)
            break;
          request.append(buffer, static_cast<size_t>(count));
          if (request.size() > 65536)
            break;
        }
        {
          std::lock_guard lock(mutex);
          requests.push_back(request);
        }
        std::string
            body = "<main><h1>Native transport</h1><p>Evidence from conditional HTTP.</p></main>",
            headers = "Content-Type: text/html\r\nETag: fixture-tag\r\nLast-Modified: Tue, 06 Oct "
                      "2026 12:00:00 GMT\r\n";
        int status = 200;
        if (request.starts_with("GET /redirect ")) {
          status = 302;
          headers += "Location: /page\r\n";
          body.clear();
        } else if (request.starts_with("GET /large "))
          body.assign(4096, 'x');
        else if (request.find("If-None-Match: fixture-tag") != std::string::npos) {
          status = 304;
          body.clear();
        }
        auto reply = "HTTP/1.1 " + std::to_string(status) + " OK\r\n" + headers +
                     "Content-Length: " + std::to_string(body.size()) +
                     "\r\nConnection: close\r\n\r\n" + body;
        std::string_view bytes(reply);
        while (!bytes.empty()) {
          auto count = send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
          if (count <= 0)
            break;
          bytes.remove_prefix(static_cast<size_t>(count));
        }
        close(fd);
      }
    });
  }
  ~Server() {
    worker.request_stop();
    if (worker.joinable())
      worker.join();
    if (listener >= 0)
      close(listener);
  }
  std::string url(std::string path) { return "http://127.0.0.1:" + std::to_string(port) + path; }
};
} // namespace
int main() {
  try {
    std::unique_ptr<Server> server;
    try {
      server = std::make_unique<Server>();
    } catch (const std::exception &) {
      std::cout << "Local HTTP sockets unavailable\n";
      return 77;
    }
    auto root = fs::temp_directory_path() / ("saga-web-http-" + uuid());
    fs::create_directories(root);
    struct Clean {
      fs::path path;
      ~Clean() {
        std::error_code e;
        fs::remove_all(path, e);
      }
    } clean{root};
    Database db(root / "agent.db");
    db.migrate();
    WebLimits limits;
    limits.allow_private_network = true;
    limits.max_response_bytes = 1024;
    WebAcquisitionEngine engine(&db, {}, limits);
    auto first = engine.acquire({server->url("/page"), {}, 1024});
    engine.begin_operation();
    auto second = engine.acquire({server->url("/page"), {}, 1024});
    CHECK(second.extraction.http_cache_hit);
    CHECK(first.rendered == second.rendered);
    {
      std::lock_guard lock(server->mutex);
      CHECK(server->requests.size() == 2);
      CHECK(server->requests[1].find("If-None-Match: fixture-tag") != std::string::npos);
      CHECK(server->requests[1].find("If-Modified-Since:") != std::string::npos);
      CHECK(server->requests[0].find("Accept: text/markdown") != std::string::npos);
      CHECK(server->requests[0].find("Authorization:") == std::string::npos);
      CHECK(server->requests[0].find("Accept-Encoding:") != std::string::npos);
    }
    auto redirected = engine.acquire({server->url("/redirect"), {}, 1024});
    CHECK(redirected.document.source.final_url == server->url("/page"));
    CHECK(redirected.document.source.requested_url == server->url("/redirect"));
    bool oversized = false;
    try {
      engine.acquire({server->url("/large"), {}, 1024});
    } catch (const Error &e) {
      oversized = e.code == ErrorCode::ResponseTooLarge;
    }
    CHECK(oversized);
    limits.max_redirects = 0;
    WebAcquisitionEngine no_redirect(nullptr, {}, limits);
    bool too_many = false;
    try {
      no_redirect.acquire({server->url("/redirect"), {}, 1024});
    } catch (const Error &e) {
      too_many = e.code == ErrorCode::TooManyRedirects;
    }
    CHECK(too_many);
    WebAcquisitionEngine public_only;
    bool blocked = false;
    try {
      public_only.acquire({server->url("/page"), {}, 1024});
    } catch (const Error &e) {
      blocked = e.code == ErrorCode::BlockedAddress;
    }
    CHECK(blocked);
    std::cout << "Native HTTP negotiation, validators, redirects and limits passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
