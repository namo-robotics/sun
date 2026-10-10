/** Native Linux fault tests for identity-bound standard-library process
 * handles. */
#include <gtest/gtest.h>

#ifdef __linux__
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>

#include "driver/execution_utils.h"

/** Helpers for isolating irreversible seccomp filters in child processes. */
namespace {
/** Force one process syscall to fail, then verify Sun preserves its native
 * error. */
void expectProcessError(int syscallNumber, int error) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    // Only the selected syscall is denied. The compiler and test runner remain
    // usable.
    sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                 static_cast<uint32_t>(syscallNumber), 0, 1),
        BPF_STMT(BPF_RET | BPF_K,
                 SECCOMP_RET_ERRNO | static_cast<uint32_t>(error)),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog program{static_cast<unsigned short>(sizeof(instructions) /
                                                   sizeof(instructions[0])),
                       instructions};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
      _exit(77);
    }
    try {
      const auto source = R"(
        using std;
        using std.process;
        /** Return the exact native error from opening an identity-bound handle. */
        function main() i32 {
          var alloc = make_heap_allocator();
          var identity = match process_identity(alloc, pid()) {
            SystemResult.Ok(value) => value,
            SystemResult.Error(error) => { return 120; }
          };
          var before = match std.io.read_dir(alloc, "/proc/self/fd") {
            SystemResult.Ok(value) => value,
            SystemResult.Error(error) => { return 123; }
          };
          var code: i32 = 0;
          for (var i: i32 = 0; i < 20; i = i + 1) {
            var handle = ProcessHandle(alloc);
            code = match handle.open(identity) {
              SystemResult.Ok => { return 121; },
              SystemResult.Error(error) => error.code()
            };
          }
          var after = match std.io.read_dir(alloc, "/proc/self/fd") {
            SystemResult.Ok(value) => value,
            SystemResult.Error(error) => { return 124; }
          };
          return before.size() == after.size() ? code : 125;
        }
      )";
      _exit(std::get<int32_t>(sun::driver::executeStringWithStdlib(source)));
    } catch (...) {
      _exit(122);
    }
  }
  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  ASSERT_EQ(result, child);
  ASSERT_TRUE(WIFEXITED(status));
  if (WEXITSTATUS(status) == 77) {
    GTEST_SKIP() << "Host does not permit an unprivileged seccomp filter";
  }
  EXPECT_EQ(WEXITSTATUS(status), error);
}
}  // namespace

/** An older kernel cannot silently downgrade an atomic handle to POSIX kill. */
TEST(EndToEnd_ProcessInspection, UnsupportedPidfdOpen) {
  expectProcessError(SYS_pidfd_open, ENOSYS);
}

/** Missing signaling support closes the just-opened descriptor and reports
 * ENOSYS. */
TEST(EndToEnd_ProcessInspection, UnsupportedPidfdSignal) {
  expectProcessError(SYS_pidfd_send_signal, ENOSYS);
}

/** Native permission failures remain distinguishable from missing processes. */
TEST(EndToEnd_ProcessInspection, PermissionDenied) {
  expectProcessError(SYS_pidfd_send_signal, EPERM);
}
/** Descriptor zero remains owned and is released when the handle closes. */
TEST(EndToEnd_ProcessInspection, OwnsDescriptorZero) {
  const auto result = sun::driver::executeStringWithStdlib(R"(
    using std;
    using std.process;
    /** Duplicate stdin so the test can restore it after exercising descriptor zero. */
    extern "C" function process_test_dup(fd: i32) i32 as "dup";
    /** Close the descriptor selected by this test. */
    extern "C" function process_test_close(fd: i32) i32 as "close";
    /** Restore stdin after the test. */
    extern "C" function process_test_dup2(old_fd: i32, new_fd: i32) i32 as "dup2";
    /** Query whether descriptor zero has been released. */
    extern "C" function process_test_fcntl(fd: i32, cmd: i32, ...) i32 as "fcntl";
    /** Verify that the handle treats fd zero as an owned descriptor. */
    function main() i32 {
      var alloc = make_heap_allocator();
      var identity = match process_identity(alloc, pid()) {
        SystemResult.Ok(value) => value,
        SystemResult.Error(error) => { return 1; }
      };
      var saved = unsafe { process_test_dup(0); };
      var handle = ProcessHandle(alloc);
      unsafe { process_test_close(0); };
      var code = match handle.open(identity) {
        SystemResult.Ok => 0,
        SystemResult.Error(error) => error.code()
      };
      var opened = unsafe { process_test_fcntl(0, 1); } >= 0;
      handle.close();
      var closed = unsafe { process_test_fcntl(0, 1); } < 0;
      if (saved >= 0) {
        unsafe { process_test_dup2(saved, 0); process_test_close(saved); };
      }
      return code == 0 and opened and closed ? 0 : 2;
    }
  )");
  EXPECT_EQ(result, 0);
}
#endif
