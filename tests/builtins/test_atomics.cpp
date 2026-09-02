// tests/builtins/test_atomics.cpp - The atomic and futex intrinsics
//
// These are the primitives std.thread.Mutex is built from. The Mutex
// behavior tests (single- and multi-threaded) live in stdlib/mutex_tests.sun;
// what is checked here is each intrinsic on its own, plus the memory
// orderings the backend emits for them.

#include <gtest/gtest.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

#include "driver/execution_utils.h"

namespace {

/*
 * Compiles a Sun program ahead of time and returns its LLVM IR as text.
 */
std::string atomicIrFor(const std::string& source) {
  initTestEnvironment();
  auto driver = Driver::createForAOT("atomic_ir");
  driver->compileString(source);
  std::string text;
  llvm::raw_string_ostream stream(text);
  driver->getModule().print(stream, nullptr);
  return text;
}

}  // namespace

// ============================================================================
// 32-bit compare-and-swap, store and load
// ============================================================================

TEST(Builtins_Atomics, atomic_cmpxchg_success) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 0;
      var old = unsafe { _atomic_cmpxchg_i32(_address_of<i32>(x), 0, 1); };
      // old is the value before the swap; x is now 1
      return old;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Builtins_Atomics, atomic_cmpxchg_fail) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 5;
      // Expected is 0 but x is 5, so the swap does not happen
      var old = unsafe { _atomic_cmpxchg_i32(_address_of<i32>(x), 0, 1); };
      // old is the actual value, unchanged
      return old;
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Builtins_Atomics, atomic_store_and_load) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 0;
      unsafe { _atomic_store_i32(_address_of<i32>(x), 42); };
      var val = unsafe { _atomic_load_i32(_address_of<i32>(x)); };
      return val;
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// 64-bit variants, signed and unsigned
// ============================================================================

TEST(Builtins_Atomics, atomic_i64_operations_execute) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i64 = 10;
      var expected: i64 = 10;
      var desired: i64 = 12;
      var delta: i64 = 5;
      var old = unsafe {
        _atomic_cmpxchg_i64(_address_of<i64>(x), expected, desired);
      };
      var before_add = unsafe {
        _atomic_fetch_add_i64(_address_of<i64>(x), delta);
      };
      var before_sub = unsafe {
        _atomic_fetch_sub_i64(_address_of<i64>(x), delta);
      };
      unsafe { _atomic_store_i64(_address_of<i64>(x), expected); };
      var loaded = unsafe { _atomic_load_i64(_address_of<i64>(x)); };
      if (old != 10) { return 1; }
      if (before_add != 12) { return 2; }
      if (before_sub != 17) { return 3; }
      if (loaded != 10) { return 4; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Builtins_Atomics, atomic_u64_operations_execute) {
  auto value = executeString(R"(
    function main() i32 {
      var x: u64 = 20;
      var expected: u64 = 20;
      var desired: u64 = 25;
      var delta: u64 = 2;
      var old = unsafe {
        _atomic_cmpxchg_u64(_address_of<u64>(x), expected, desired);
      };
      var before_add = unsafe {
        _atomic_fetch_add_u64(_address_of<u64>(x), delta);
      };
      var before_sub = unsafe {
        _atomic_fetch_sub_u64(_address_of<u64>(x), delta);
      };
      unsafe { _atomic_store_u64(_address_of<u64>(x), expected); };
      var loaded = unsafe { _atomic_load_u64(_address_of<u64>(x)); };
      if (old != expected) { return 1; }
      if (before_add != desired) { return 2; }
      if (before_sub != 27) { return 3; }
      if (loaded != expected) { return 4; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Memory orderings reach the IR
// ============================================================================

TEST(Builtins_Atomics, atomic_orderings_are_emitted) {
  std::string ir = atomicIrFor(R"(
    function main() i32 {
      var x: i32 = 0;
      unsafe { _atomic_fence_release(); };
      unsafe { _atomic_store_i32(_address_of<i32>(x), 1); };
      var loaded = unsafe { _atomic_load_i32(_address_of<i32>(x)); };
      var old = unsafe {
        _atomic_cmpxchg_i32(_address_of<i32>(x), loaded, 2);
      };
      var before_add = unsafe {
        _atomic_fetch_add_i32(_address_of<i32>(x), old);
      };
      unsafe { _atomic_fence_acquire(); };
      return before_add;
    }
  )");

  EXPECT_NE(ir.find("store atomic i32"), std::string::npos) << ir;
  EXPECT_NE(ir.find("release, align"), std::string::npos) << ir;
  EXPECT_NE(ir.find("load atomic i32"), std::string::npos) << ir;
  EXPECT_NE(ir.find("acquire, align"), std::string::npos) << ir;
  EXPECT_NE(ir.find("cmpxchg"), std::string::npos) << ir;
  EXPECT_NE(ir.find("acq_rel acquire"), std::string::npos) << ir;
  EXPECT_NE(ir.find("atomicrmw add"), std::string::npos) << ir;
  EXPECT_NE(ir.find("fence release"), std::string::npos) << ir;
  EXPECT_NE(ir.find("fence acquire"), std::string::npos) << ir;
}

// ============================================================================
// Futex
// ============================================================================

// Waking an address nobody waits on is a harmless no-op. (_futex_wait is not
// exercised here: with no other thread to wake it, it would block forever.)
TEST(Builtins_Atomics, futex_wake_without_waiters_returns) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 0;
      unsafe { _futex_wake(_address_of<i32>(x)); };
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}
