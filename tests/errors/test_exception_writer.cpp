/** Injects interrupted and partial writes into the mandatory exception
 * reporter. */
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "exception_source_fixture.h"
#include "exception_test_storage.h"
#include "runtime/exceptions.h"

/** Holds bounded output and controls the next simulated system write. */
namespace writer_test {
char output[sun::runtime::reportWorkspaceSize + 2048];
std::size_t used;
unsigned calls;
unsigned interruptCount;
std::size_t writeLimit;
bool fail;
bool zero;

/** Restores independent output state before each writer scenario. */
void reset() {
  used = 0;
  calls = 0;
  interruptCount = 0;
  writeLimit = sizeof(output);
  fail = false;
  zero = false;
}
}  // namespace writer_test

/** Simulates system-call behavior without allocation or an actual diagnostic
 * destination. */
extern "C" ssize_t sun_test_write(int, const void* bytes, size_t count) {
  using namespace writer_test;
  ++calls;
  if (interruptCount) {
    --interruptCount;
    errno = EINTR;
    return -1;
  }
  if (fail) {
    errno = EIO;
    return -1;
  }
  if (zero) {
    return 0;
  }
  count = std::min(count, writeLimit);
  if (used + count > sizeof(output)) {
    errno = ENOSPC;
    return -1;
  }
  std::memcpy(output + used, bytes, count);
  used += count;
  return static_cast<ssize_t>(count);
}

/** Compiles a private reporter with simulated writes, leaving the real runtime
 * available to other tests in the same executable. */
#define sun_eh_report test_eh_report
#define sun_eh_print_trace test_eh_print_trace
#define sun_report_stack_trace test_report_stack_trace
#define write sun_test_write
#include "../../src/runtime/exception_report.cpp"
#undef write
#undef sun_eh_report
#undef sun_eh_print_trace
#undef sun_report_stack_trace

/** Interrupted writes retry without changing the caller's errno. */
TEST(Errors_ExceptionWriter, retries_interrupted_writes) {
  writer_test::reset();
  writer_test::interruptCount = 2;
  const std::uintptr_t frames[] = {1, 2};
  const std::uint64_t dimensions[] = {2};
  errno = EDOM;
  test_eh_print_trace({frames, 1, dimensions}, 0);
  EXPECT_EQ(errno, EDOM);
  EXPECT_EQ(std::string(writer_test::output, writer_test::used),
            "Exception trace\n  0x2\n  └─ 0x1\n");
}

/** Short writes resume at the first unwritten byte. */
TEST(Errors_ExceptionWriter, completes_short_writes) {
  writer_test::reset();
  writer_test::writeLimit = 3;
  const std::uintptr_t frame = 1;
  const std::uint64_t count = 1;
  test_eh_print_trace({&frame, 1, &count}, 0);
  EXPECT_EQ(std::string(writer_test::output, writer_test::used),
            "Exception trace\n  0x1\n");
}

/** A permanent write failure stops immediately without replacing the exception.
 */
TEST(Errors_ExceptionWriter, stops_after_failed_write) {
  writer_test::reset();
  writer_test::fail = true;
  sun::runtime::ExceptionTypeDescriptor descriptor{"writer.Failure", nullptr, 0,
                                                   1};
  sun::tests::exceptions::OwnedExceptionStorage value{};
  value.header.type = &descriptor;
  value.header.trace.status = sun::runtime::TraceStatus::Complete;
  errno = EDOM;
  test_eh_report(&value.header, 0);
  EXPECT_EQ(writer_test::calls, 1u);
  EXPECT_EQ(errno, EDOM);
  EXPECT_EQ(value.header.type, &descriptor);
}

/** A destination making no progress cannot trap the reporter in a retry loop.
 */
TEST(Errors_ExceptionWriter, stops_after_zero_length_write) {
  writer_test::reset();
  writer_test::zero = true;
  const std::uint64_t dimensions[] = {0};
  test_eh_print_trace({nullptr, 1, dimensions}, 4);
  EXPECT_EQ(writer_test::calls, 1u);
}

/** Resolves only during reporting and lists callers before the original throw.
 */
TEST(Errors_ExceptionWriter, reports_source_call_chain) {
  sun::tests::exceptions::SourceIndexFixture source;
  writer_test::reset();
  const std::uintptr_t frames[] = {0x300, 0x200, 0x100};
  const std::uint64_t count = 3;
  test_eh_print_trace({frames, 1, &count}, 0);
  EXPECT_EQ(std::string(writer_test::output, writer_test::used),
            "Exception trace\n"
            "  main()  /tmp/report example.sun:10:2\n"
            "  └─ foo()  /tmp/report example.sun:20:2\n"
            "    └─ bar()  /tmp/report example.sun:30:2\n"
            "      └─ throw Failure()  /tmp/report example.sun:30:2\n");
}

/** Limits long function names without losing the frame's source location. */
TEST(Errors_ExceptionWriter, truncates_long_function_names) {
  sun::tests::exceptions::SourceIndexFixture source;
  std::memset(source.names[0], 'x', sizeof(source.names[0]) - 1);
  writer_test::reset();
  const std::uintptr_t frame = 0x100;
  const std::uint64_t count = 1;
  test_eh_print_trace({&frame, 1, &count}, 0);
  const std::string output(writer_test::output, writer_test::used);
  EXPECT_NE(
      output.find(std::string(sun::runtime::reportNameLimit, 'x') + "...()"),
      std::string::npos);
  EXPECT_NE(output.find("/tmp/report example.sun:10:2"), std::string::npos);
}

/** Stops at a complete line when a large inline chain fills the reserved report
 * workspace. */
TEST(Errors_ExceptionWriter, bounds_the_complete_report) {
  sun::tests::exceptions::SourceIndexFixture<256> source;
  std::memset(source.names[0], 'x', sizeof(source.names[0]) - 1);
  for (auto& frame : source.frames) {
    frame = source.frames[0];
  }
  source.rows[0].frameCount = 256;
  writer_test::reset();
  const std::uintptr_t frame = 0x100;
  const std::uint64_t count = 1;
  test_eh_print_trace({&frame, 1, &count}, 0);
  const std::string output(writer_test::output, writer_test::used);
  EXPECT_NE(output.find("... trace report truncated\n"), std::string::npos);
  EXPECT_LE(output.size(), sun::runtime::reportWorkspaceSize + 128);
}

/** The readable type remains available when capture is disabled. */
TEST(Errors_ExceptionWriter, names_exception_without_a_trace) {
  writer_test::reset();
  sun::runtime::ExceptionTypeDescriptor descriptor{"internal-identity", nullptr,
                                                   0, 1, "Failure"};
  sun::tests::exceptions::OwnedExceptionStorage value{};
  value.header.type = &descriptor;
  value.header.trace.status = sun::runtime::TraceStatus::Disabled;
  test_eh_report(&value.header, 0);
  EXPECT_EQ(std::string(writer_test::output, writer_test::used),
            "Exception: Failure\nTrace disabled\n");
}

/** Labels relative paths with their entrypoint base instead of implying the
 * working directory. */
TEST(Errors_ExceptionWriter, labels_entrypoint_relative_sources) {
  sun::tests::exceptions::SourceIndexFixture source;
  std::strcpy(source.file, "../../x.sun");
  writer_test::reset();
  const std::uintptr_t frame = 0x100;
  const std::uint64_t count = 1;
  test_eh_print_trace({&frame, 1, &count}, 0);
  EXPECT_EQ(std::string(writer_test::output, writer_test::used),
            "Exception trace\n  main()  ENTRYPOINT/../../x.sun:10:2\n");
}
