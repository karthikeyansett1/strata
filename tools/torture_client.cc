#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <array>
#include <charconv>
#include <fstream>
#include <iostream>

#include "disk.h"

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: torture_client PORT SAFE_KEY_PREFIX ACK_LOG\n";
    return 2;
  }
  std::uint16_t port = 0;
  const std::string_view port_text = argv[1];
  const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
  if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || port == 0) {
    return 2;
  }
  const std::string prefix = argv[2];
  if (prefix.empty() ||
      prefix.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyz-") != std::string::npos) {
    return 2;
  }
  std::ofstream log(argv[3], std::ios::app);
  strata::disk::File socket_fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (!log || !socket_fd.valid() ||
      connect(socket_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    return 1;
  }
  const int no_delay = 1;
  if (setsockopt(socket_fd.get(), IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) < 0) {
    return 1;
  }
  for (std::uint64_t i = 0;; ++i) {
    const auto key = prefix + '-' + std::to_string(i);
    const auto command = "PUT " + key + ' ' + key + '\n';
    std::string_view rest(command);
    while (!rest.empty()) {
      const auto count = send(socket_fd.get(), rest.data(), rest.size(), MSG_NOSIGNAL);
      if (count <= 0) {
        return (errno == EPIPE || errno == ECONNRESET) ? 0 : 1;
      }
      rest.remove_prefix(count);
    }
    std::array<char, 3> reply;
    const auto count = recv(socket_fd.get(), reply.data(), reply.size(), MSG_WAITALL);
    if (count != static_cast<ssize_t>(reply.size())) {
      return count >= 0 || errno == ECONNRESET ? 0 : 1;
    }
    if (std::string_view(reply.data(), reply.size()) != "OK\n") {
      return 1;
    }
    log << key << std::endl;
    if (!log) {
      return 1;
    }
  }
}
