/** Exercises one runtime behavior at a time while heap allocation is denied. */
#include "exception_allocation_support.h"

#include <array>
#include <atomic>
#include <thread>

#include "allocation_audit.h"
#include "exception_source_fixture.h"
#include "exception_test_storage.h"

/** Runs the same scenarios with native entrypoints or already-materialized JIT
 * entrypoints. */
namespace sun::tests::exceptions {

int checkTransport(const TransportFixture& fixture) {
  sun_eh_initialize();
  AllocationAudit audit("Exception transport");
  const bool nestedPreserved = fixture.nested() == 60;
  const bool rethrowPreserved = fixture.rethrow() == 10;
  if (!audit.finish()) {
    return 1;
  }
  return nestedPreserved && rethrowPreserved ? 0 : 2;
}

int checkNativeTraceBounds() {
  sun_eh_initialize();
  bool valid = true;
  SourceIndexFixture source;
  AllocationAudit audit("Native trace bounds and reporting");
  sun::tests::exceptions::OwnedExceptionStorage value;
  sun_fixture_capture(&value.header, 0);
  const auto base = value.header.trace.count;
  if (!base || base >= sun_exception_trace_depth ||
      value.header.trace.status != sun::runtime::TraceStatus::Complete) {
    valid = false;
  } else {
    sun_fixture_capture(&value.header, sun_exception_trace_depth - base);
    if (value.header.trace.count != sun_exception_trace_depth ||
        value.header.trace.status != sun::runtime::TraceStatus::Complete) {
      valid = false;
    }
    sun_fixture_capture(&value.header, sun_exception_trace_depth - base + 1);
    if (value.header.trace.count != sun_exception_trace_depth ||
        value.header.trace.status != sun::runtime::TraceStatus::Truncated) {
      valid = false;
    }
  }
  sun_eh_report(&value.header);
  sun_eh_print_trace(
      {value.header.trace.addresses(), 1, &value.header.trace.count},
      static_cast<std::uint32_t>(value.header.trace.status));
  const std::uintptr_t addresses[] = {0x300, 0x200, 0x100};
  const std::uint64_t count = 3;
  sun_eh_print_trace({addresses, 1, &count}, 0);
  sun_report_stack_trace();
  if (!audit.finish()) {
    return 1;
  }
  return valid ? 0 : 2;
}

int checkConcurrentTransport(const TransportFixture& fixture) {
  std::atomic<unsigned> ready{0}, done{0}, failures{0};
  std::atomic<bool> start{false}, leave{false};
  std::array<std::thread, 4> workers;
  for (auto& worker : workers) {
    /** Excludes worker startup and teardown from the allocation-denial
     * interval. */
    worker = std::thread([&] {
      sun_eh_initialize();
      ++ready;
      while (!start.load()) {
        std::this_thread::yield();
      }
      if (fixture.nested() != 60 || fixture.rethrow() != 10) {
        ++failures;
      }
      ++done;
      while (!leave.load()) {
        std::this_thread::yield();
      }
    });
  }
  while (ready.load() != workers.size()) {
    std::this_thread::yield();
  }

  AllocationAudit audit("Concurrent exception transport");
  start.store(true);
  while (done.load() != workers.size()) {
    std::this_thread::yield();
  }
  const bool allocationFree = audit.finish();

  leave.store(true);
  for (auto& worker : workers) {
    worker.join();
  }
  if (!allocationFree) {
    return 1;
  }
  return failures.load() == 0 ? 0 : 2;
}
}  // namespace sun::tests::exceptions
