#include "server/server.h"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <barrier>
#include <future>
#include <thread>

#include "disk.h"
#include "server/protocol.h"
#include "temp_directory.h"

namespace strata::server {
namespace {

class Client {
 public:
  explicit Client(std::uint16_t port) : file_(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const timeval timeout{10, 0};
    connected_ = file_.valid() &&
                 setsockopt(file_.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
                 setsockopt(file_.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
                 connect(file_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  }

  bool Send(std::string_view bytes) const {
    while (!bytes.empty()) {
      const auto n = send(file_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (n <= 0) {
        return false;
      }
      bytes.remove_prefix(n);
    }
    return true;
  }

  bool FinishSending() const { return shutdown(file_.get(), SHUT_WR) == 0; }
  bool connected() const { return connected_; }

  std::string ReadLine() {
    auto end = pending_.find('\n');
    while (end == std::string::npos) {
      const auto previous = pending_.size();
      std::array<char, 8192> bytes;
      const auto n = recv(file_.get(), bytes.data(), bytes.size(), 0);
      if (n <= 0) {
        return {};
      }
      pending_.append(bytes.data(), n);
      end = pending_.find('\n', previous);
    }
    auto line = pending_.substr(0, end + 1);
    pending_.erase(0, end + 1);
    return line;
  }

 private:
  disk::File file_;
  bool connected_ = false;
  std::string pending_;
};

TEST(Protocol, AssemblesPartialAndPipelinedCommands) {
  Input input;
  ASSERT_TRUE(input.Append("PUT ke"));
  EXPECT_FALSE(input.Next());
  ASSERT_TRUE(input.Append("y a value\r\nGET key\nDEL"));
  EXPECT_EQ(input.Next(), "PUT key a value");
  EXPECT_EQ(input.Next(), "GET key");
  EXPECT_FALSE(input.Next());
  ASSERT_TRUE(input.Append(" key\n"));
  EXPECT_EQ(input.Next(), "DEL key");
  EXPECT_FALSE(input.Append(std::string(kMaxLine + 1, 'x')));
}

TEST(Protocol, RejectsMalformedCommandsAndAcceptsEmptyValues) {
  test::TempDirectory directory;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open({}, directory.path, &db).ok());
  for (const std::string_view command : {"", "PUT", "PUT key", "GET", "GET a b", "DEL a b",
                                         "SCAN a b c", "STATS extra", "UNKNOWN"}) {
    EXPECT_TRUE(Execute(*db, command).starts_with("ERR ")) << command;
  }
  EXPECT_TRUE(Execute(*db, std::string_view("GET a\0b", 7)).starts_with("ERR "));
  EXPECT_EQ(Execute(*db, "PUT key "), "OK\n");
  EXPECT_EQ(Execute(*db, "GET key"), "VALUE \n");
  EXPECT_EQ(Execute(*db, "SCAN key l"), "key \nEND\n");
}

class ServerTest : public testing::Test {
 protected:
  void SetUp() override {
    Options options;
    options.sync_mode = SyncMode::kBatch;
    ASSERT_TRUE(DB::Open(options, directory_.path, &db_).ok());
    auto future = ready_.get_future();
    server_ = std::jthread([this](std::stop_token stop) {
      bool started = false;
      const auto result = Serve(*db_, 0, 8, stop, [&](std::uint16_t port) {
        started = true;
        ready_.set_value(port);
      });
      if (!started) {
        ready_.set_value(0);
      }
      EXPECT_TRUE(result.ok()) << result.message();
    });
    port_ = future.get();
    ASSERT_NE(port_, 0);
  }

  test::TempDirectory directory_;
  std::unique_ptr<DB> db_;
  std::promise<std::uint16_t> ready_;
  std::jthread server_;
  std::uint16_t port_ = 0;
};

TEST_F(ServerTest, Handles128ConcurrentMixedClientsWithOrderedReplies) {
  constexpr int kClients = 128;
  std::barrier connected(kClients);
  std::vector<std::jthread> clients;
  for (int i = 0; i < kClients; ++i) {
    clients.emplace_back([&, i] {
      Client client(port_);
      connected.arrive_and_wait();
      ASSERT_TRUE(client.connected());
      const auto key = "key-" + std::to_string(i);
      ASSERT_TRUE(client.Send("PU"));
      ASSERT_TRUE(client.Send("T " + key + " a value\nGET " + key + "\nDEL " + key + "\nGET " +
                              key + "\n"));
      EXPECT_EQ(client.ReadLine(), "OK\n");
      EXPECT_EQ(client.ReadLine(), "VALUE a value\n");
      EXPECT_EQ(client.ReadLine(), "OK\n");
      EXPECT_EQ(client.ReadLine(), "NOT_FOUND\n");
    });
  }
}

TEST_F(ServerTest, HandlesLargeRepliesHalfCloseAndDisconnectedClients) {
  const std::string payload(5 * 1024 * 1024, 'x');
  ASSERT_TRUE(db_->Put("large", payload).ok());
  {
    Client abandoned(port_);
    ASSERT_TRUE(abandoned.connected());
    ASSERT_TRUE(abandoned.Send("GET large\n"));
  }
  Client client(port_);
  ASSERT_TRUE(client.connected());
  ASSERT_TRUE(client.Send("GET large\nSTATS\n"));
  ASSERT_TRUE(client.FinishSending());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(client.ReadLine(), "VALUE " + payload + "\n");
  EXPECT_TRUE(client.ReadLine().starts_with("seq="));
  EXPECT_TRUE(client.ReadLine().starts_with("wal_syncs="));
  EXPECT_TRUE(client.ReadLine().starts_with("sstable_count="));
  EXPECT_TRUE(client.ReadLine().starts_with("compactions="));
  EXPECT_EQ(client.ReadLine(), "END\n");
  EXPECT_TRUE(client.ReadLine().empty());
}

}  // namespace
}  // namespace strata::server
