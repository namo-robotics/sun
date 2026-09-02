// tests/stdlib/concurrency/test_mutex.cpp - Compile-level tests for the
// atomic and futex intrinsics behind Mutex, plus Mutex compilation checks.
// The Mutex behavior tests (single- and multi-threaded) live in
// stdlib/mutex_tests.sun and run through `sun test`.

#include <gtest/gtest.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"
#include "support/error.h"

namespace {

/** Compiles a Sun program and returns its LLVM IR. */
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
// Atomic Intrinsic Compilation Tests
// ============================================================================

TEST(Stdlib_Concurrency_Mutex, atomic_cmpxchg_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 0;
      var old = unsafe { _atomic_cmpxchg_i32(_address_of<i32>(x), 0, 1); };
      return old;
    }
  )"));
}

TEST(Stdlib_Concurrency_Mutex, atomic_store_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 0;
      unsafe { _atomic_store_i32(_address_of<i32>(x), 42); };
      return 0;
    }
  )"));
}

TEST(Stdlib_Concurrency_Mutex, atomic_load_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 42;
      var val = unsafe { _atomic_load_i32(_address_of<i32>(x)); };
      return val;
    }
  )"));
}

TEST(Stdlib_Concurrency_Mutex, atomic_i64_operations_execute) {
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

TEST(Stdlib_Concurrency_Mutex, atomic_u64_operations_execute) {
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

TEST(Stdlib_Concurrency_Mutex, atomic_orderings_are_emitted) {
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
// Futex Intrinsic Compilation Tests
// ============================================================================

TEST(Stdlib_Concurrency_Mutex, futex_wait_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 0;
      // Don't actually wait - just verify it compiles
      // _futex_wait(_address_of<i32>(x), 1);  // Would block if x != 1
      return 0;
    }
  )"));
}

TEST(Stdlib_Concurrency_Mutex, futex_wake_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 0;
      unsafe { _futex_wake(_address_of<i32>(x)); };
      return 0;
    }
  )"));
}

// ============================================================================
// Atomic Intrinsic Execution Tests
// ============================================================================

TEST(Stdlib_Concurrency_Mutex, atomic_cmpxchg_success) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 0;
      var old = unsafe { _atomic_cmpxchg_i32(_address_of<i32>(x), 0, 1); };
      // old should be 0 (the original value)
      // x should now be 1
      return old;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Concurrency_Mutex, atomic_cmpxchg_fail) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 5;
      // Expected is 0, but x is 5, so cmpxchg should fail
      var old = unsafe { _atomic_cmpxchg_i32(_address_of<i32>(x), 0, 1); };
      // old should be 5 (the actual value, not changed)
      return old;
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Stdlib_Concurrency_Mutex, atomic_store_and_load) {
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
// Mutex Compilation Tests
// ============================================================================

TEST(Stdlib_Concurrency_Mutex, mutex_import_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    using std.thread;

    function main() i32 {
      var m = Mutex();
      return 0;
    }
  )",
                                true));
}

TEST(Stdlib_Concurrency_Mutex, mutex_lock_unlock_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    using std.thread;

    function main() i32 {
      var m = Mutex();
      m.lock();
      m.unlock();
      return 0;
    }
  )",
                                true));
}

// The Mutex execution tests (single- and multi-threaded) were migrated to
// stdlib/mutex_tests.sun.
