/** Compare portable open-file metadata with the host's native stat API. */
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "driver/execution_utils.h"

/** Native fixtures and comparisons for the standard library's stat boundary. */
namespace {
/** Own a scratch file and its descriptor for one test. */
class MetadataFixture {
 public:
  char path[64] = "tmp/sun_metadata_XXXXXX";
  int fd;

  /** Acquire a unique file beneath the workspace's temporary directory. */
  MetadataFixture() : fd(-1) {
    std::filesystem::create_directories("tmp");
    fd = mkstemp(path);
  }

  /** Release the file even if an assertion returns early. */
  ~MetadataFixture() {
    if (fd >= 0) {
      close(fd);
    }
    unlink(path);
  }
};

/** Compare kind, size, timestamp, and unchanged seek position against native
 * fstat. */
void expectMetadata(int fd, const std::string& kind) {
  struct stat native{};
  ASSERT_EQ(fstat(fd, &native), 0);
#ifdef __APPLE__
  const auto modified = native.st_mtimespec;
#else
  const auto modified = native.st_mtim;
#endif
  const auto offset = lseek(fd, 0, SEEK_CUR);
  const int owned = dup(fd);
  ASSERT_GE(owned, 0);
  const auto source = std::string(R"(
    using std;
    /** Check that metadata describes the adopted object and preserves its offset. */
    function main() i32 {
      var file = std.io.File();
      file.adopt()") + std::to_string(owned) +
                      R"();
      var info = match file.metadata() {
        SystemResult.Ok(value) => value,
        SystemResult.Error(error) => { return 1; }
      };
      if (info.kind != std.io.FileKind.)" +
                      kind + R"() { return 2; }
      if (info.size != )" +
                      std::to_string(native.st_size) + R"() { return 3; }
      if (info.modified.seconds != )" +
                      std::to_string(modified.tv_sec) + R"() { return 4; }
      if (info.modified.nanoseconds != )" +
                      std::to_string(modified.tv_nsec) + R"() { return 5; }
      if (file.seek(0, std.io.Whence.Current) != )" +
                      std::to_string(offset) + R"() { return 6; }
      return 0;
    }
  )";
  EXPECT_EQ(sun::driver::executeStringWithStdlib(source), 0);
}
}  // namespace

/** Empty and sparse large files retain exact sizes and subsecond timestamps. */
TEST(EndToEnd_ServerOs, NativeFileMetadata) {
  MetadataFixture file;
  ASSERT_GE(file.fd, 0);
  expectMetadata(file.fd, "File");
  ASSERT_EQ(ftruncate(file.fd, INT64_C(5368709137)), 0);
  const timespec times[] = {{123456789, 0}, {1700000000, 123456789}};
  ASSERT_EQ(futimens(file.fd, times), 0);
  ASSERT_EQ(lseek(file.fd, 17, SEEK_SET), 17);
  expectMetadata(file.fd, "File");
  // Removing the pathname cannot redirect metadata away from the open object.
  ASSERT_EQ(unlink(file.path), 0);
  expectMetadata(file.fd, "File");
}

/** A directory uses its native kind and size without requiring seek-to-end. */
TEST(EndToEnd_ServerOs, NativeDirectoryMetadata) {
  std::filesystem::create_directories("tmp");
  char path[] = "tmp/sun_metadata_dir_XXXXXX";
  ASSERT_NE(mkdtemp(path), nullptr);
  const int fd = open(path, O_RDONLY | O_DIRECTORY);
  ASSERT_GE(fd, 0);
  expectMetadata(fd, "Dir");
  close(fd);
  rmdir(path);
}

/** The online CPU query uses the host's native selector and does not silently
 * substitute one. */
TEST(EndToEnd_ServerOs, NativeOnlineCpuCount) {
  const long count = sysconf(_SC_NPROCESSORS_ONLN);
  ASSERT_GT(count, 0);
  const auto source = std::string(R"(
    using std;
    /** Compare the portable query with the native count supplied by this test. */
    function main() i32 {
      return match std.thread.online_cpu_count() {
        SystemResult.Ok(count) => count == )") +
                      std::to_string(count) + R"( ? 0 : 1,
        SystemResult.Error(error) => 2
      };
    }
  )";
  EXPECT_EQ(sun::driver::executeStringWithStdlib(source), 0);
}

/** An explicitly selected process policy also protects native writes outside
 * Sun's send wrapper. */
TEST(EndToEnd_ServerOs, NativeBrokenPipePolicy) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      const auto result = sun::driver::executeStringWithStdlib(R"(
        using std;
        /** Select the server's process-wide broken-pipe policy explicitly. */
        function main() i32 {
          return match std.signal.ignore_broken_pipe() {
            SystemResult.Ok => 0,
            SystemResult.Error(error) => 1
          };
        }
      )");
      if (std::get<int32_t>(result) != 0) {
        _exit(1);
      }
      struct sigaction action{};
      if (sigaction(SIGPIPE, nullptr, &action) != 0 ||
          action.sa_handler != SIG_IGN) {
        _exit(2);
      }
      int fds[2];
      if (pipe(fds) != 0) {
        _exit(3);
      }
      close(fds[0]);
      const char byte = 0;
      const auto count = write(fds[1], &byte, 1);
      const int error = errno;
      close(fds[1]);
      _exit(count == -1 && error == EPIPE ? 0 : 4);
    } catch (...) {
      _exit(5);
    }
  }
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  ASSERT_EQ(waited, child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

#ifdef __linux__
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <cstddef>

/** Force descriptor configuration to fail and check rollback of pipes and
 * accepted sockets. */
TEST(EndToEnd_ServerOs, ConfigurationFailureClosesDescriptors) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fcntl, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[1])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETFL, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog filter{static_cast<unsigned short>(sizeof(instructions) /
                                                  sizeof(instructions[0])),
                      instructions};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter) != 0) {
      _exit(77);
    }
    try {
      const auto result = sun::driver::executeStringWithStdlib(R"(
        using std;
        /** Exercise both fallible constructors repeatedly and compare descriptor counts. */
        function verify() Result<i32> {
          var alloc = make_heap_allocator();
          var listener = TcpListener();
          try listener.bind(ipv4_loopback(), 0, true, false);
          try listener.listen(8);
          var port = try listener.local_port();
          var before = try std.io.read_dir(alloc, "/proc/self/fd");
          for (var i: i32 = 0; i < 8; i = i + 1) {
            match std.io.Wakeup(alloc) {
              SystemResult.Ok(wake) => { return Result.Ok(1); },
              SystemResult.Error(error) => { if (error.code() != 13) { return Result.Ok(2); } }
            };
            var client = TcpStream();
            try client.connect_local(port);
            match listener.try_accept() {
              AcceptResult.Error(error) => { if (error.code() != 13) { return Result.Ok(3); } },
              _ => { return Result.Ok(4); }
            };
          }
          var after = try std.io.read_dir(alloc, "/proc/self/fd");
          return Result.Ok(before.size() == after.size() ? 0 : 5);
        }
        /** Convert unexpected native setup failures to a distinct process status. */
        function main() i32 {
          return match verify() { Result.Ok(code) => code, _ => 6 };
        }
      )");
      _exit(std::get<int32_t>(result));
    } catch (...) {
      _exit(7);
    }
  }
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  ASSERT_EQ(waited, child);
  ASSERT_TRUE(WIFEXITED(status));
  if (WEXITSTATUS(status) == 77) {
    GTEST_SKIP() << "Host does not permit seccomp";
  }
  EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif
