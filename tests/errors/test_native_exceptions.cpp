/** Verifies native and JIT propagation without the C++ exception runtime. */
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <thread>
#include <vector>

#include "driver/sun_jit.h"
#include "exception_fixture.h"
#include "exception_jit_fixture.h"
#include "exception_test_storage.h"
#include "runtime/exceptions.h"

/** Keeps JIT setup helpers local to these transport tests. */
namespace {
/** Runs the same immutable code with independent exception state on each
 * thread. */
int concurrentFailures(int (*run)()) {
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    /** Keeps all exceptions on this worker's storage and stack. */
    threads.emplace_back([&] {
      sun_eh_initialize();
      for (int j = 0; j < 32; ++j) {
        if (run() != 60) {
          ++failures;
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  return failures.load();
}

/** Exercises the same runtime behavior with both JIT backend optimization
 * modes. */
class Errors_JITExceptions : public testing::TestWithParam<bool> {
 protected:
  std::unique_ptr<sun::driver::SunJIT> jit;

  /** Materializes a fresh code lifetime before each independent scenario. */
  void SetUp() override {
    jit = sun::tests::exceptions::createExceptionJIT(GetParam());
    sun::tests::exceptions::loadExceptionFixture(*jit);
    ASSERT_NE(jit, nullptr);
    sun_eh_initialize();
  }

  /** Resolves a generated fixture using its exact C function-pointer type. */
  template <typename Function>
  Function lookup(const char* name) {
    return llvm::cantFail(jit->lookup(name)).getAddress().toPtr<Function>();
  }
};
}  // namespace

/** A and B retain their native headers while their respective cleanups handle
 * C. */
TEST(Errors_NativeExceptions, nested_cleanup_preserves_all_payloads) {
  sun_eh_initialize();
  EXPECT_EQ(sun_fixture_nested(), 60);
  EXPECT_EQ(sun_fixture_nested(), 60);
}

/** Catching a parent never slices the concrete identity used by a later
 * rethrow. */
TEST(Errors_NativeExceptions, rethrow_after_handled_error) {
  sun_eh_initialize();
  EXPECT_EQ(sun_fixture_rethrow(), 10);
}

/** Deep stack traversal must skip a catch clause for a different concrete type.
 */
TEST(Errors_NativeExceptions, deep_propagation_skips_unrelated_handler) {
  sun_eh_initialize();
  EXPECT_EQ(sun_fixture_deep(128), 10);
}

/** Equivalent descriptors from separately compiled modules match by identity.
 */
TEST(Errors_NativeExceptions, descriptor_identity_is_not_pointer_identity) {
  using sun::runtime::ExceptionTypeDescriptor;
  const ExceptionTypeDescriptor parent{"library:Parent:layout1", nullptr, 4, 4};
  const ExceptionTypeDescriptor imported{"library:Parent:layout1", nullptr, 4,
                                         4};
  const ExceptionTypeDescriptor other{"library:Parent:layout2", nullptr, 8, 8};
  const ExceptionTypeDescriptor child{"library:Child:layout1", &parent, 8, 4};
  EXPECT_TRUE(sun_eh_matches(&child, &imported));
  EXPECT_FALSE(sun_eh_matches(&child, &other));
}

/** An unhandled cleanup error terminates before its record's frame disappears.
 */
TEST(Errors_NativeExceptions, escaping_cleanup_terminates) {
  EXPECT_DEATH(
      {
        sun_eh_initialize();
        sun_fixture_escape();
      },
      "Sun exception escaped");
}

/** Each thread retains separate base storage throughout nested unwinding. */
TEST(Errors_NativeExceptions, simultaneous_threads) {
  EXPECT_EQ(concurrentFailures(&sun_fixture_nested), 0);
}

/** JIT cleanup preserves both the suspended payloads and their native headers.
 */
TEST_P(Errors_JITExceptions, nested_cleanup_preserves_all_payloads) {
  EXPECT_EQ(lookup<int (*)()>("sun_fixture_nested")(), 60);
}

/** A parent-caught JIT exception retains its concrete type when rethrown. */
TEST_P(Errors_JITExceptions, rethrow_after_handled_error) {
  EXPECT_EQ(lookup<int (*)()>("sun_fixture_rethrow")(), 10);
}

/** Registered JIT frame tables support deep propagation past unrelated
 * handlers. */
TEST_P(Errors_JITExceptions, deep_propagation_skips_unrelated_handler) {
  EXPECT_EQ(lookup<int (*)(int)>("sun_fixture_deep")(128), 10);
}

/** Threads share registered JIT tables without sharing active exception
 * records. */
TEST_P(Errors_JITExceptions, simultaneous_threads) {
  EXPECT_EQ(concurrentFailures(lookup<int (*)()>("sun_fixture_nested")), 0);
}

INSTANTIATE_TEST_SUITE_P(BackendOptimization, Errors_JITExceptions,
                         testing::Bool());

/** The extra eligible frame marks truncation without exceeding the inline
 * capacity. */
TEST(Errors_NativeExceptions, deep_capture_is_bounded) {
  sun::tests::exceptions::OwnedExceptionStorage caught;
  sun_fixture_capture(&caught.header, 64);
  ASSERT_EQ(caught.header.trace.count, sun_exception_trace_depth);
  EXPECT_EQ(caught.header.trace.status, sun::runtime::TraceStatus::Truncated);
  for (auto frame : caught.frames) {
    EXPECT_NE(frame, 0u);
  }
}

/** Nested throws cannot overwrite the trace retained by a catch and rethrow. */
TEST(Errors_NativeExceptions, rethrow_preserves_original_trace) {
  sun::tests::exceptions::OwnedExceptionStorage original{}, result;
  sun_fixture_capture(&original.header, 0);
  ASSERT_GT(original.header.trace.count, 0u);
  const auto trace = original.header.trace;
  const auto frames = original.frames;
  sun_fixture_trace_rethrow(&original.header, &result.header);
  EXPECT_EQ(original.header.type, nullptr);
  ASSERT_EQ(result.header.trace.count, trace.count);
  EXPECT_EQ(result.header.trace.status, trace.status);
  for (unsigned i = 0; i < trace.count; ++i) {
    EXPECT_EQ(result.frames[i], frames[i]);
  }
}

/** JIT capture finds the throw boundary and stops at the configured capacity.
 */
TEST_P(Errors_JITExceptions, deep_capture_is_bounded) {
  sun::tests::exceptions::OwnedExceptionStorage caught;
  lookup<void (*)(sun::runtime::ExceptionValue*, int)>("sun_fixture_capture")(
      &caught.header, 64);
  ASSERT_EQ(caught.header.trace.count, sun_exception_trace_depth);
  EXPECT_EQ(caught.header.trace.status, sun::runtime::TraceStatus::Truncated);
}

/** The terminal diagnostic identifies the escaping nested exception rather than
 * A. */
TEST(Errors_NativeExceptions, escaping_cleanup_reports_nested_exception) {
  EXPECT_DEATH(
      {
        sun_eh_initialize();
        sun_fixture_escape();
      },
      "Unhandled exception: fixture.B");
}

/** Reporting preserves ownership and errno while including the original frames.
 */
TEST(Errors_NativeExceptions, reporting_preserves_retained_value) {
  sun::tests::exceptions::OwnedExceptionStorage value;
  sun_fixture_capture(&value.header, 64);
  const auto* type = value.header.type;
  testing::internal::CaptureStderr();
  errno = EDOM;
  sun_eh_report(&value.header);
  EXPECT_EQ(errno, EDOM);
  auto output = testing::internal::GetCapturedStderr();
  EXPECT_EQ(value.header.type, type);
  EXPECT_NE(output.find("fixture.A"), std::string::npos);
  EXPECT_NE(output.find("  0x"), std::string::npos);
  EXPECT_NE(output.find("Trace truncated"), std::string::npos);
}

/** A closed diagnostic pipe must not convert reporting into process
 * termination. */
TEST(Errors_NativeExceptions, reporting_ignores_closed_pipe) {
  EXPECT_EXIT(
      {
        sun::tests::exceptions::OwnedExceptionStorage value;
        sun_fixture_capture(&value.header, 0);
        int descriptors[2];
        if (pipe(descriptors) || dup2(descriptors[1], STDERR_FILENO) < 0) {
          _exit(2);
        }
        close(descriptors[0]);
        close(descriptors[1]);
        sun_eh_report(&value.header);
        _exit(0);
      },
      testing::ExitedWithCode(0), "");
}

/** A destructor may select storage before any other runtime operation on its
 * thread. */
TEST(Errors_NativeExceptions, push_initializes_fresh_thread) {
  int result = 0;
  /** Begins with uninitialized thread state and handles nested exceptions
   * independently. */
  std::thread worker([&] {
    sun::tests::exceptions::OwnedPropagationStorage storage;
    auto* previous = sun_eh_push(&storage.header);
    result = sun_fixture_nested();
    sun_eh_pop(previous);
  });
  worker.join();
  EXPECT_EQ(result, 60);
}

/** Explicit printing reads only populated addresses and preserves the borrowed
 * value. */
TEST(Errors_NativeExceptions, explicit_print_preserves_trace) {
  sun::tests::exceptions::OwnedExceptionStorage value;
  sun_fixture_capture(&value.header, 0);
  const auto original = value.header.trace;
  const auto frames = value.frames;
  testing::internal::CaptureStderr();
  errno = EDOM;
  sun_eh_print_trace(
      {value.header.trace.addresses(), 1, &value.header.trace.count},
      static_cast<std::uint32_t>(value.header.trace.status));
  EXPECT_EQ(errno, EDOM);
  auto output = testing::internal::GetCapturedStderr();
  EXPECT_NE(output.find("Exception trace\n  0x"), std::string::npos);
  EXPECT_EQ(value.header.trace.count, original.count);
  EXPECT_EQ(value.header.trace.status, original.status);
  for (unsigned i = 0; i < original.count; ++i) {
    EXPECT_EQ(value.frames[i], frames[i]);
  }
}
