/** Checks explicit class alignment across compiler-managed storage. */
#include <gtest/gtest.h>

#include "driver/compiler.h"
#include "driver/execution_utils.h"
#include "support/error.h"

using sun::driver::executeString;

/** Loads the native address probe used by generated code. */
static void registerAlignmentProbe() {
  sun::driver::LinkOptions options;
  options.libraries = {"sun_ffi_testlib"};
  options.searchPaths = {SUN_FFI_TESTLIB_DIR};
  ASSERT_TRUE(sun::driver::loadNativeLibraries(options).failed.empty());
}

/** Checks size, alignment and nested field offsets without allocating values.
 */
TEST(Classes_Alignment, layout_and_nested_arrays) {
  EXPECT_EQ(executeString(R"(
    /** An explicitly aligned scalar. */
    class A { alignment: 64 var value: i32; }
    /** A record containing an aligned field and a trailing byte. */
    class B { var first: u8; var value: A; var last: u8; }
    _assert_c_layout<A>(64, 64, 0);
    _assert_c_layout<B>(192, 64, 0, 64, 128);
    /** Returns the size and alignment of a fixed array. */
    function main() i64 {
      return _sizeof<array<A, 3>>() + _alignof<array<A, 3>>();
    }
  )"),
            256);
}

/** Confirms metadata survives specialization and never reduces natural
 * alignment. */
TEST(Classes_Alignment, generic_and_natural_alignment) {
  EXPECT_EQ(executeString(R"(
    /** Keeps generic elements on an explicitly aligned boundary. */
    class Box<T> { alignment: 32 var value: T; }
    /** Requests less alignment than its field naturally needs. */
    class Natural { alignment: 1 var value: i64; }
    _assert_c_layout<Box<i32>>(32, 32, 0);
    _assert_c_layout<Natural>(8, 8, 0);
    /** Reports the concrete generic layout. */
    function main() i64 { return _sizeof<Box<i64>>(); }
  )"),
            32);
}

/** Tests addresses after a value is passed to and returned from a Sun function.
 */
TEST(Classes_Alignment, stack_arguments_and_returns) {
  registerAlignmentProbe();
  EXPECT_EQ(executeString(R"(
    /** Checks a pointer using the native test harness. */
    extern function sun_test_alignment_remainder(p: raw_ptr<u8>, alignment: i64) i64;
    /** A value requiring more alignment than the ordinary stack ABI. */
    class A { alignment: 64 var value: i64; }
    /** Checks the callee's parameter storage and returns ownership. */
    function pass(value: A) A {
      if (unsafe { sun_test_alignment_remainder(_bitcast<raw_ptr<u8>>(_address_of<A>(value)), 64); } != 0) { value.value = 0; }
      return value;
    }
    /** Checks the caller's result storage. */
    function main() i64 {
      var value: A = {value: 17};
      var result = pass(value);
      if (unsafe { sun_test_alignment_remainder(_bitcast<raw_ptr<u8>>(_address_of<A>(result)), 64); } != 0) { return 0; }
      return result.value;
    }
  )"),
            17);
}

/** Checks that aligned heap memory remains compatible with ordinary
 * deallocation. */
TEST(Classes_Alignment, aligned_heap_storage) {
  registerAlignmentProbe();
  EXPECT_EQ(executeString(R"(
    /** Checks a pointer using the native test harness. */
    extern function sun_test_alignment_remainder(p: raw_ptr<u8>, alignment: i64) i64;
    /** An over-aligned heap element. */
    class A { alignment: 256 var value: i64; }
    /** Allocates, checks and frees element storage. */
    function main() i64 {
      unsafe {
        var memory = _aligned_malloc<A>(_sizeof<A>() * 3);
        if (memory == null) { return 1; }
        var remainder = sun_test_alignment_remainder(memory, 256);
        _free(memory);
        return _convert<i64>(remainder);
      };
    }
  )"),
            0);
}

/** Rejects malformed or contradictory class alignment metadata. */
TEST(Classes_Alignment, invalid_metadata) {
  for (const auto* source :
       {"class A { alignment: 0 }", "class A { alignment: 3 }",
        "class A { alignment: -16 }", "class A { alignment: 16; }",
        "class A { alignment: 16 alignment: 32 }",
        "class A { alignment: 131072 }",
        "packed_class A { alignment: 16 var x: i32; }",
        "class A {} partial class A { alignment: 16 }"}) {
    SCOPED_TRACE(source);
    EXPECT_THROW(executeString(std::string(source) +
                               " function main() i32 { return 0; }"),
                 sun::support::SunError);
  }
}

/** Rejects incorrect assertions even if the checked class is never
 * instantiated. */
TEST(Classes_Alignment, invalid_layout_assertions) {
  for (const auto* check :
       {"_assert_c_layout<A>(4, 16, 0);", "_assert_c_layout<A>(16, 4, 0);",
        "_assert_c_layout<A>(16, 16, 4);", "_assert_c_layout<A>(16, 16);",
        "_assert_c_layout<A>(16 + 0, 16, 0);",
        "_assert_c_target(\"not-a-target\");"}) {
    SCOPED_TRACE(check);
    EXPECT_THROW(executeString(
                     std::string("class A { alignment: 16 var value: i32; } ") +
                     check + " function main() i32 { return 0; }"),
                 sun::support::SunError);
  }
}

/** Checks global storage and the alignment of an enum payload after a move. */
TEST(Classes_Alignment, globals_and_enum_payloads) {
  registerAlignmentProbe();
  EXPECT_EQ(executeString(R"(
    /** Probes pointer alignment in the native test harness. */
    extern function sun_test_alignment_remainder(p: raw_ptr<u8>, alignment: i64) i64;
    /** A value with an over-aligned payload. */
    class A { alignment: 64 var value: i64; }
    /** Carries an aligned value through tagged storage. */
    enum E { Value(A), Empty }
    var global: A = {value: 42};
    /** Checks global and enum storage addresses. */
    function main() i64 {
      if (unsafe { sun_test_alignment_remainder(_bitcast<raw_ptr<u8>>(_address_of<A>(global)), 64); } != 0) { return 1; }
      var value: A = {value: 42};
      var wrapped = E.Value(value);
      if (_alignof<E>() != 64) { return 2; }
      match (wrapped) {
        E.Value(payload) => {
          if (unsafe { sun_test_alignment_remainder(_bitcast<raw_ptr<u8>>(_address_of<A>(payload)), 64); } != 0) { return 3; }
          return payload.value;
        },
        E.Empty => { return 4; }
      };
    }
  )"),
            42);
}

/** Checks the safe typed allocator entry points with an over-aligned class. */
TEST(Classes_Alignment, stdlib_typed_heap_storage) {
  registerAlignmentProbe();
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    using std;
    /** Probes pointer alignment in the native test harness. */
    extern function sun_test_alignment_remainder(p: raw_ptr<u8>, alignment: i64) i64;
    /** A typed heap allocation with an explicit alignment requirement. */
    class A {
      alignment: 256
      var value: i64;
      /** Initializes the value in its final storage. */
      init(value: i64) { this.value = value; }
    }
    /** Allocates and frees a typed value and a typed array. */
    function main() i64 {
      var alloc = HeapAllocator();
      var one = alloc.create<A>(42);
      var many = alloc.alloc<A>(3);
      var result: i64 = 0;
      unsafe {
        result = sun_test_alignment_remainder(_bitcast<raw_ptr<u8>>(one), 256)
          + sun_test_alignment_remainder(_bitcast<raw_ptr<u8>>(many), 256);
        _deinit<A>(one);
      };
      alloc.dealloc<A>(one, 1);
      alloc.dealloc<A>(many, 3);
      return result;
    }
  )"),
            0);
}

/** Carries aligned arguments and results through thread-owned heap storage. */
TEST(Classes_Alignment, thread_arguments_and_results) {
  registerAlignmentProbe();
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    using std;
    /** Probes pointer alignment in the native test harness. */
    extern function sun_test_alignment_remainder(p: raw_ptr<u8>, alignment: i64) i64;
    /** A value moved between threads. */
    class A { alignment: 64 var value: i64; }
    /** Checks the worker's parameter storage and returns ownership. */
    function worker(value: A) A {
      if (unsafe { sun_test_alignment_remainder(_bitcast<raw_ptr<u8>>(_address_of<A>(value)), 64); } != 0) { value.value = 0; }
      return value;
    }
    /** Moves a value to a worker and checks its returned storage. */
    function main() i64 {
      var value: A = {value: 42};
      var thread = std.thread.spawn(worker, value);
      var result = thread.join();
      if (unsafe { sun_test_alignment_remainder(_bitcast<raw_ptr<u8>>(_address_of<A>(result)), 64); } != 0) { return 0; }
      return result.value;
    }
  )"),
            42);
}
