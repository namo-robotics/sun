/** Exercises allocation failure without exhausting the host's memory. */
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <unordered_set>

#include "driver/driver.h"
#include "driver/execution_utils.h"

/** Contains native allocation shims private to each test's JIT instance. */
namespace {
/** Tracks only allocations made by the generated program on this thread. */
struct AllocationState {
  std::int64_t remaining = -1;
  std::unordered_set<void*> live;
};
thread_local AllocationState* allocationState = nullptr;

/** Report whether the configured successful allocation budget is exhausted. */
bool failAllocation() {
  if (allocationState->remaining < 0) {
    return false;
  }
  if (allocationState->remaining == 0) {
    return true;
  }
  --allocationState->remaining;
  return false;
}

/** Allocate for generated code, with deterministic failure and leak tracking. */
void* checkedMalloc(std::size_t bytes) {
  if (failAllocation()) {
    return nullptr;
  }
  void* pointer = std::malloc(bytes);
  if (pointer) {
    allocationState->live.insert(pointer);
  }
  return pointer;
}

/** Preserve the POSIX failure contract while testing aligned allocation. */
int checkedAlignedMalloc(void** pointer, std::size_t alignment,
                         std::size_t bytes) {
  if (failAllocation()) {
    return ENOMEM;
  }
  int status = posix_memalign(pointer, alignment, bytes);
  if (status == 0) {
    EXPECT_GT(bytes, 0u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(*pointer) % alignment, 0u);
    allocationState->live.insert(*pointer);
  }
  return status;
}

/** Release generated allocations and record that their owners cleaned up. */
void checkedFree(void* pointer) {
  if (pointer) {
    EXPECT_EQ(allocationState->live.erase(pointer), 1u);
  }
  std::free(pointer);
}

/** Allow Sun test code to select the allocation that will fail. */
void failAfter(std::int64_t remaining) {
  allocationState->remaining = remaining;
}

/** Restore native test state even when compilation or execution throws. */
struct AllocationScope {
  AllocationState state;
  /** Start an isolated allocation accounting scope. */
  AllocationScope() { allocationState = &state; }
  /** Detect leaked generated allocations and restore the host environment. */
  ~AllocationScope() {
    EXPECT_TRUE(state.live.empty());
    for (void* pointer : state.live) {
      std::free(pointer);
    }
    allocationState = nullptr;
  }
};

/** Execute a program with allocator shims bound only in its own JIT instance. */
sun::driver::SunValue executeWithAllocationFailures(const std::string& source) {
  sun::driver::initTestEnvironment();
  AllocationScope scope;
  auto driver = sun::driver::Driver::createForJIT();
  driver->setMoonImports(sun::driver::getStdlibMoonImports());
  driver->defineJITSymbol("malloc", reinterpret_cast<void*>(&checkedMalloc));
  driver->defineJITSymbol("posix_memalign",
                          reinterpret_cast<void*>(&checkedAlignedMalloc));
  driver->defineJITSymbol("free", reinterpret_cast<void*>(&checkedFree));
  driver->defineJITSymbol("allocation_fail_after",
                          reinterpret_cast<void*>(&failAfter));
  return driver->executeString(R"(
    using std;
    /** Configure failures in this test's native allocation shim. */
    extern function allocation_fail_after(count: i64) void;
  )" + source);
}
}  // namespace

/** Raw allocation reports a null native result as AllocationError. */
TEST(MemorySafety_AllocationFailure, raw_allocation_reports_failure) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Request storage after exhausting the test allocation budget. */
    function main() i32 {
      unsafe { allocation_fail_after(0); };
      return match global_allocator.alloc_raw(64) {
        AllocationResult.Allocation(error) => _convert<i32>(error.requested_size()),
        _ => -1
      };
    }
  )"), 64);
}

/** Invalid sizes and typed multiplication overflow do not reach native allocation. */
TEST(MemorySafety_AllocationFailure, invalid_sizes_are_rejected) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Check distinct invalid-argument and overflow outcomes. */
    function main() i32 {
      unsafe { allocation_fail_after(0); };
      var negative = match global_allocator.alloc_raw(-1) {
        AllocationResult.InvalidArgument(_) => 1,
        _ => 0
      };
      var overflow = match global_allocator.alloc<i64>(std.I64_MAX) {
        AllocationResult.Overflow(_) => 2,
        _ => 0
      };
      return negative + overflow;
    }
  )"), 3);
}

/** A zero-size allocation succeeds with normal ownership and deallocation. */
TEST(MemorySafety_AllocationFailure, zero_size_has_freeable_storage) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Allocate and release zero logical bytes through the global allocator. */
    function main() i32 {
      return match global_allocator.alloc_raw(0) {
        AllocationResult.Ok(pointer) => {
          var valid = pointer != null;
          global_allocator.dealloc_raw(pointer, 0);
          valid ? 1 : 0;
        },
        _ => 0
      };
    }
  )"), 1);
}

/** Failed storage allocation must not invoke the requested constructor. */
TEST(MemorySafety_AllocationFailure, allocation_failure_skips_constructor) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    var constructions: i32 = 0;
    /** Records construction without allocating. */
    class Value {
      /** Record that initialized storage was available. */
      init() { constructions = constructions + 1; }
    }
    /** Verify failure happens before initialization. */
    function main() i32 {
      unsafe { allocation_fail_after(0); };
      var failed = match global_allocator.create<Value>() {
        Result.Allocation(_) => 1,
        _ => 0
      };
      return failed - constructions;
    }
  )"), 1);
}

/** Vector growth failure preserves all existing initialized values. */
TEST(MemorySafety_AllocationFailure, vector_growth_preserves_contents) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Fill a vector, then fail its next growth. */
    function check() Result<i32> {
      var values = try Vec<i32>(global_allocator, 1);
      try values.push(42);
      unsafe { allocation_fail_after(0); };
      var failed = match values.push(99) {
        AllocationResult.Allocation(_) => 1,
        _ => 0
      };
      return Result.Ok(_convert<i32>(failed == 1 and values.size() == 1 and values.capacity() == 1 and values[0] == 42 ? 1 : 0));
    }
    /** Translate the checked scenario into a test exit value. */
    function main() i32 { return match check() { Result.Ok(value) => value, _ => -1 }; }
  )"), 1);
}

/** Over-aligned requests propagate native allocation failure too. */
TEST(MemorySafety_AllocationFailure, aligned_allocation_reports_failure) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Forces the allocator to use its aligned allocation path. */
    class Aligned {
      alignment: 256
      var value: i64 = 0;
    }
    /** Fail aligned storage allocation deterministically. */
    function main() i32 {
      unsafe { allocation_fail_after(0); };
      return match global_allocator.alloc<Aligned>(1) {
        AllocationResult.Allocation(_) => 1,
        _ => 0
      };
    }
  )"), 1);
}

/** Failed constructors drop initialized fields once and release raw storage. */
TEST(MemorySafety_AllocationFailure, failed_constructor_releases_storage) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    var field_drops: i32 = 0;
    var object_drops: i32 = 0;
    /** Records destruction of an initialized field. */
    class Field {
      /** Count the field's cleanup. */
      deinit() { field_drops += 1; }
    }
    /** Fails after initializing one field. */
    class Value {
      var field: Field;
      /** Initialize a field, then report a supported constructor error. */
      init() AccessResult<void> {
        this.field = Field();
        return AccessResult.OutOfBounds(IndexOutOfBoundsError(7));
      }
      /** Count only fully initialized object destruction. */
      deinit() { object_drops += 1; }
    }
    /** Observe the propagated error and partial-object cleanup. */
    function main() i32 {
      var failed = match global_allocator.create_unique<Value>() {
        Result.OutOfBounds(_) => 1,
        _ => 0
      };
      return failed == 1 and field_drops == 1 and object_drops == 0 ? 1 : 0;
    }
  )"), 1);
}

/** A later map storage failure must leave the original table intact. */
TEST(MemorySafety_AllocationFailure, map_growth_preserves_contents) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Fail the second replacement allocation during rehashing. */
    function check() Result<i32> {
      var values = try Map<i64, i32>(global_allocator, 2);
      try values.insert(1, 42);
      unsafe { allocation_fail_after(1); };
      var failed = match values.insert(2, 99) {
        AllocationResult.Allocation(_) => 1,
        _ => 0
      };
      var value = try values.get(1);
      return Result.Ok(_convert<i32>(failed == 1 and values.size() == 1 and values.capacity() == 2 and value == 42 ? 1 : 0));
    }
    /** Translate the checked scenario into a test exit value. */
    function main() i32 { return match check() { Result.Ok(value) => value, _ => -1 }; }
  )"), 1);
}

/** Interpolation returns an error and releases its partial string. */
TEST(MemorySafety_AllocationFailure, interpolation_propagates_failure) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Deny the allocation needed by a long interpolation. */
    function main() i32 {
      unsafe { allocation_fail_after(0); };
      return match `this string is longer than inline storage: ${42}` {
        AllocationResult.Allocation(_) => 1,
        _ => 0
      };
    }
  )"), 1);
}

/** Building an allocation error does not itself need memory. */
TEST(MemorySafety_AllocationFailure, diagnostic_allocation_is_fallible) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Handle a message-copy failure independently from creating the error. */
    function main() i32 {
      unsafe { allocation_fail_after(0); };
      var error = AllocationError(42);
      return match error.message() {
        AllocationResult.Allocation(_) => 1,
        _ => 0
      };
    }
  )"), 1);
}

/** Every thread allocation failure cleans up arguments already moved into spawn. */
TEST(MemorySafety_AllocationFailure, thread_setup_releases_partial_storage) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    var drops: i32 = 0;
    /** Carries one owned argument into thread creation. */
    class Owner {
      var value: i32 = 42;
      /** Count destruction whether the worker starts or setup fails. */
      deinit() { drops += 1; }
    }
    /** Would consume the argument if thread creation succeeded. */
    function work(value: Owner) i32 { return value.value; }
    /** Fail the argument, context, and result allocations in turn. */
    function main() i32 {
      var failures: i32 = 0;
      for (var budget: i64 = 0; budget < 3; budget += 1) {
        unsafe { allocation_fail_after(budget); };
        match std.thread.spawn(work, Owner()) {
          AllocationResult.Allocation(_) => { failures += 1; },
          _ => {}
        };
      }
      return failures == 3 and drops == 3 ? 1 : 0;
    }
  )"), 1);
}

/** Global allocation remains explicit and callable through an immutable module borrow. */
TEST(MemorySafety_AllocationFailure, global_allocator_across_modules) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Provides allocation operations across a module boundary. */
    module client {
      /** Allocate typed storage through a const allocator reference. */
      public function allocate(allocator: const ref HeapAllocator) AllocationResult<i32> {
        var pointer = try allocator.alloc<i32>(1);
        unsafe { _store<i32>(pointer, 0, 42); };
        var value = unsafe { _load<i32>(pointer, 0); };
        allocator.dealloc(pointer, 1);
        var values = try Vec<i32>(allocator, 1);
        try values.push(value);
        return AllocationResult.Ok(values.size() == 1 ? value : 0);
      }
    }
    /** Pass the immutable standard allocator into another module. */
    function main() i32 {
      return match client.allocate(global_allocator) {
        AllocationResult.Ok(value) => value,
        _ => -1
      };
    }
  )"), 42);
}

/** Constructor errors must fit the factory result instead of being silently lost. */
TEST(MemorySafety_AllocationFailure, unsupported_constructor_error_is_diagnosed) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::compileStringWithStdlib(R"(
    using std;
    /** Represents a constructor-specific failure outside the standard result. */
    class CustomError implements IError {
      /** Identify this fixture error. */
      public const method code() i32 { return 100; }
      /** Produce a fallible description for the error interface. */
      public const method message() AllocationResult<String> {
        return String(global_allocator, "custom constructor error");
      }
    }
    /** Reports construction failure without a standard error category. */
    enum Constructed<T> { Ok(T), Error(CustomError) }
    /** Always fails construction with the custom error. */
    class Object {
      /** Reject construction before any object exists. */
      init() Constructed<void> { return Constructed.Error(CustomError()); }
    }
    /** Force instantiation of the allocating constructor adapter. */
    function main() void { ignore_error(global_allocator.create<Object>()); }
  )"), "Cannot propagate error variant");
}

/** Error reporting uses fixed text when allocating a diagnostic also fails. */
TEST(MemorySafety_AllocationFailure, diagnostic_reporting_uses_fixed_fallback) {
  testing::internal::CaptureStdout();
  const auto status = executeWithAllocationFailures(R"(
    /** Report a failed diagnostic without trying to allocate another message. */
    function main() i32 {
      unsafe { allocation_fail_after(0); };
      var error = AllocationError(42);
      var result = std.test.TestResult("allocation", false, error.message());
      return std.test.report(result) ? 0 : 1;
    }
  )");
  const auto output = testing::internal::GetCapturedStdout();
  EXPECT_EQ(status, 1);
  EXPECT_NE(output.find("unable to allocate error message"), std::string::npos);
}

/** Zero logical elements retain alignment and ordinary deallocation semantics. */
TEST(MemorySafety_AllocationFailure, zero_size_aligned_storage) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Requires the native aligned allocation path. */
    class Aligned { alignment: 256 var value: i64 = 0; }
    /** Release aligned storage without constructing an element. */
    function main() i32 {
      return match global_allocator.alloc<Aligned>(0) {
        AllocationResult.Ok(pointer) => {
          var valid = pointer != null;
          global_allocator.dealloc<Aligned>(pointer, 0);
          valid ? 1 : 0;
        },
        _ => 0
      };
    }
  )"), 1);
}

/** Typed negative counts fail before any native allocation is attempted. */
TEST(MemorySafety_AllocationFailure, negative_typed_count) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Distinguish invalid input from an exhausted allocation budget. */
    function main() i32 {
      unsafe { allocation_fail_after(0); };
      return match global_allocator.alloc<i64>(-1) {
        AllocationResult.InvalidArgument(_) => 1,
        _ => 0
      };
    }
  )"), 1);
}

/** A later interpolation allocation failure drops previously allocated text. */
TEST(MemorySafety_AllocationFailure, interpolation_releases_partial_string) {
  EXPECT_EQ(executeWithAllocationFailures(R"(
    /** Permit the first expansion and fail the next expansion. */
    function check() AllocationResult<i32> {
      var part = try String(global_allocator, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefgh");
      unsafe { allocation_fail_after(1); };
      return AllocationResult.Ok(match `${part}${part}` {
        AllocationResult.Allocation(_) => 1,
        _ => 0
      });
    }
    /** Return the checked scenario's result to the harness. */
    function main() i32 {
      return match check() { AllocationResult.Ok(value) => value, _ => -1 };
    }
  )"), 1);
}
