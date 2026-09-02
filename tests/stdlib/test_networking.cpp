// tests/stdlib/test_networking.cpp - TCP readiness and socket errors
//
// Exercises non-blocking TCP behavior against a native ephemeral listener.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "driver/execution_utils.h"

namespace {

/*
 * Owns one native socket used by a networking test.
 */
class SocketHandle {
 public:
  /*
   * Takes ownership of a native descriptor.
   */
  explicit SocketHandle(int fd) : fd_(fd) {}

  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;

  /*
   * Closes the descriptor when the test scope ends.
   */
  ~SocketHandle() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /*
   * Returns the owned native descriptor.
   */
  int get() const { return fd_; }

 private:
  int fd_;
};

}  // namespace

TEST(Stdlib_Networking, nonblocking_recv_reports_would_block) {
  SocketHandle listener(::socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_GE(listener.get(), 0);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ASSERT_EQ(::bind(listener.get(), reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)),
            0);
  ASSERT_EQ(::listen(listener.get(), 1), 0);

  socklen_t address_len = sizeof(address);
  ASSERT_EQ(::getsockname(listener.get(),
                         reinterpret_cast<sockaddr*>(&address), &address_len),
            0);
  const auto port = static_cast<int>(ntohs(address.sin_port));

  const std::string source = R"(
    using std;

    function main() i32 {
        var alloc = make_heap_allocator();
        var stream = TcpStream();
        try {
            stream.connect_local()"
      + std::to_string(port) + R"();
            stream.set_nonblocking(true);
            var buf = ContiguousBuffer<u8>(alloc, 16);
            try {
                stream.recv(buf);
                return 1;
            } catch (e: IError) {
                if (not is_would_block(e.code())) { return 2; }
            }
            stream.set_nonblocking(false);
            stream.close();
            return 0;
        } catch (e: IError) {
            return -1;
        }
    }
  )";

  EXPECT_EQ(executeStringWithStdlib(source), 0);
}

TEST(Stdlib_Networking, recv_reports_the_real_errno) {
  auto value = executeStringWithStdlib(R"(
    using std;

    function main() i32 {
        var alloc = make_heap_allocator();
        var stream = TcpStream();
        var buf = ContiguousBuffer<u8>(alloc, 1);
        try {
            stream.recv(buf);
            return 1;
        } catch (e: IError) {
            // EBADF is 9 on every supported target.
            if (e.code() != 9) { return 2; }
            if (is_would_block(e.code())) { return 3; }
            var listener = TcpListener();
            try {
                listener.set_nonblocking(true);
                return 4;
            } catch (listener_error: IError) {
                if (listener_error.code() != 9) { return 5; }
            }
            return 0;
        }
    }
  )");
  EXPECT_EQ(value, 0);
}
