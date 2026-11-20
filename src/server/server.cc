#include "server.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#include <array>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "disk.h"
#include "protocol.h"
#include "thread_pool.h"

namespace strata::server {
namespace {

constexpr std::uint64_t kListen = 1;
constexpr std::uint64_t kWake = 2;

void Wake(int fd) {
  const std::uint64_t one = 1;
  while (write(fd, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
}

struct Connection {
  explicit Connection(disk::File socket) : fd(std::move(socket)) {}

  disk::File fd;
  Input input;
  std::string output;
  std::size_t sent = 0;
  bool busy = false;
  bool eof = false;
  bool close_after_write = false;
};

struct Completion {
  std::uint64_t id;
  std::string response;
};

class Loop {
 public:
  Loop(DB& db, std::size_t threads, VectorStore* vectors)
      : db_(db), vectors_(vectors), workers_(threads) {}

  Status Run(std::uint16_t port, std::stop_token stop,
             const std::function<void(std::uint16_t)>& ready) {
    if (!listener_.valid() || !epoll_.valid() || !wake_.valid()) {
      return {StatusCode::kIOError, "could not create server descriptors"};
    }
    const int reuse = 1;
    if (setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
      return disk::Error("set listening socket options");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    socklen_t size = sizeof(address);
    if (bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), size) < 0 ||
        listen(listener_.get(), SOMAXCONN) < 0 ||
        getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &size) < 0 ||
        !Watch(EPOLL_CTL_ADD, listener_.get(), kListen, EPOLLIN) ||
        !Watch(EPOLL_CTL_ADD, wake_.get(), kWake, EPOLLIN)) {
      return disk::Error("initialize epoll listener");
    }

    std::stop_callback wake_on_stop(stop, [this] { Wake(wake_.get()); });
    ready(ntohs(address.sin_port));
    std::array<epoll_event, 64> events;
    while (!stop.stop_requested()) {
      const auto count = epoll_wait(epoll_.get(), events.data(), events.size(), -1);
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        return disk::Error("wait for epoll events");
      }
      for (int i = 0; i < count && !stop.stop_requested(); ++i) {
        const auto id = events[i].data.u64;
        if (id == kListen) {
          if (auto status = Accept(); !status.ok()) {
            return status;
          }
        } else if (id == kWake) {
          Deliver();
        } else {
          Handle(id, events[i].events);
        }
      }
    }
    return {};
  }

 private:
  bool Watch(int operation, int fd, std::uint64_t id, std::uint32_t flags) {
    epoll_event event{};
    event.events = flags;
    event.data.u64 = id;
    return epoll_ctl(epoll_.get(), operation, fd, &event) == 0;
  }

  Status Accept() {
    // Keep listener bursts from monopolizing the loop.
    for (int i = 0; i < 64; ++i) {
      disk::File socket(accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
      if (!socket.valid()) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return {};
        }
        if (errno == EINTR || errno == ECONNABORTED) {
          continue;
        }
        return disk::Error("accept client");
      }
      if (clients_.size() >= 1024 || inflight_ >= 1024 ||
          next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        continue;
      }
      const int no_delay = 1;
      (void)setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
      const auto id = next_id_++;
      if (Watch(EPOLL_CTL_ADD, socket.get(), id, EPOLLIN | EPOLLRDHUP)) {
        clients_.try_emplace(id, std::move(socket));
      }
    }
    return {};
  }

  bool Dispatch(std::uint64_t id, Connection& client) {
    auto line = client.input.Next();
    if (!line) {
      return !client.eof;
    }
    client.busy = workers_.Submit([this, id, line = std::move(*line)] {
      std::string response;
      try {
        response = Execute(db_, line, vectors_);
      } catch (...) {
        response = "ERR request failed\n";
      }
      {
        std::lock_guard lock(completion_mutex_);
        completed_.push_back({id, std::move(response)});
      }
      Wake(wake_.get());
    });
    inflight_ += client.busy;
    return client.busy;
  }

  bool Update(std::uint64_t id, Connection& client) {
    if (client.busy) {
      return Watch(EPOLL_CTL_MOD, client.fd.get(), id, 0);
    }
    const auto flags = client.output.empty() ? EPOLLIN | EPOLLRDHUP : EPOLLOUT;
    return Watch(EPOLL_CTL_MOD, client.fd.get(), id, flags);
  }

  bool Read(std::uint64_t id, Connection& client) {
    std::array<char, 8192> bytes;
    const auto count = recv(client.fd.get(), bytes.data(), bytes.size(), 0);
    if (count < 0) {
      return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
    }
    if (count == 0) {
      client.eof = true;
    } else if (!client.input.Append({bytes.data(), static_cast<std::size_t>(count)})) {
      client.output = "ERR line too long\n";
      client.close_after_write = true;
      return true;
    }
    return Dispatch(id, client);
  }

  bool Write(std::uint64_t id, Connection& client) {
    const auto count = send(client.fd.get(), client.output.data() + client.sent,
                            client.output.size() - client.sent, MSG_NOSIGNAL);
    if (count <= 0) {
      return count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK);
    }
    client.sent += count;
    if (client.sent < client.output.size()) {
      return true;
    }
    client.output.clear();
    client.sent = 0;
    return !client.close_after_write && Dispatch(id, client);
  }

  void Handle(std::uint64_t id, std::uint32_t events) {
    const auto it = clients_.find(id);
    if (it == clients_.end()) {
      return;
    }
    auto& client = it->second;
    bool alive = !(events & (EPOLLERR | EPOLLHUP));
    if (alive && (events & EPOLLOUT) && !client.output.empty()) {
      alive = Write(id, client);
    } else if (alive && (events & (EPOLLIN | EPOLLRDHUP)) && !client.busy &&
               client.output.empty()) {
      alive = Read(id, client);
    }
    if (!alive || !Update(id, client)) {
      clients_.erase(it);
    }
  }

  void Deliver() {
    std::uint64_t counter;
    while (read(wake_.get(), &counter, sizeof(counter)) < 0 && errno == EINTR) {
    }
    std::deque<Completion> ready;
    {
      std::lock_guard lock(completion_mutex_);
      ready.swap(completed_);
    }
    inflight_ -= ready.size();
    for (auto& completion : ready) {
      const auto it = clients_.find(completion.id);
      if (it == clients_.end()) {
        continue;
      }
      auto& client = it->second;
      client.busy = false;
      client.output = std::move(completion.response);
      if (!Update(completion.id, client)) {
        clients_.erase(it);
      }
    }
  }

  DB& db_;
  VectorStore* vectors_;
  disk::File listener_{socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
  disk::File epoll_{epoll_create1(EPOLL_CLOEXEC)};
  disk::File wake_{eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
  std::uint64_t next_id_ = 3;
  std::size_t inflight_ = 0;
  std::unordered_map<std::uint64_t, Connection> clients_;
  std::mutex completion_mutex_;
  std::deque<Completion> completed_;
  ThreadPool workers_;  // Drain callbacks before destroying their shared state.
};

}  // namespace

Status Serve(DB& db, std::uint16_t port, std::size_t threads, std::stop_token stop,
             const std::function<void(std::uint16_t)>& ready, VectorStore* vectors) try {
  return Loop(db, threads, vectors).Run(port, stop, ready);
} catch (...) {
  return {StatusCode::kIOError, "server event loop failed"};
}

}  // namespace strata::server
