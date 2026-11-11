// tests/memory_safety/test_allocator.cpp - Tests for allocator-based memory
// allocation

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"

using sun::driver::executeString;
using sun::driver::executeStringWithStdlib;

// ============================================================================
// Minimal Variadic Parameter Tests (no stdlib)
// ============================================================================

TEST(MemorySafety_Allocator, variadic_method_local_allocator) {
  // Minimal test: local class with variadic generic method
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        init(x: i32, y: i32) { this.x = x; this.y = y; }
        method sum() i32 { return this.x + this.y; }
    }

    class MyAllocator {
        init() {}
        method create<T>(args...: _params_of<T>) raw_ptr<T> {
            var size: i64 = _sizeof<T>();
            var memory: raw_ptr<i8> = unsafe { _malloc(size); };
            unsafe { _init<T>(memory, args...); };
            return memory;
        }
    }

    function main() i32 {
        var alloc = MyAllocator();
        var p = alloc.create<Point>(3, 4);
        return unsafe { p.sum(); };
    }
  )");
  EXPECT_EQ(value, 7);
}

// ============================================================================
// Basic HeapAllocator Tests
// ============================================================================

TEST(MemorySafety_Allocator, heap_allocator_create_simple_class) {
  auto value = executeStringWithStdlib(R"(
using std;

class Point {
  var x: i32;
  var y: i32;

  init(x: i32, y: i32) {
    this.x = x;
    this.y = y;
  }

  method sum() i32 {
    return this.x + this.y;
  }
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var rawPtr = try alloc.create<Point>(3, 4);
  var p = Unique<Point>(rawPtr);
  return Result.Ok(p.get().sum());
}
)");
  EXPECT_EQ(value, 7);
}

TEST(MemorySafety_Allocator, heap_allocator_create_generic_class) {
  auto value = executeStringWithStdlib(R"(
using std;

class Box<T> {
  var value: T;

  init(v: T) {
    this.value = v;
  }

  method get() T {
    return this.value;
  }
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var rawPtr = try alloc.create<Box<i32>>(42);
  var p = Unique<Box<i32>>(rawPtr);
  return Result.Ok(p.get().get());
}
)");
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_Allocator, unique_ptr_automatic_cleanup) {
  // Unique<T> wraps allocator.create<T>() result for automatic cleanup
  auto value = executeStringWithStdlib(R"(
using std;

class Point {
  var x: i32;
  var y: i32;

  init(x: i32, y: i32) {
    this.x = x;
    this.y = y;
  }
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var rawPtr = try alloc.create<Point>(3, 4);
  var p = Unique<Point>(rawPtr);
  var result = p.get().x + p.get().y;
  // p.deinit() is automatically called at scope exit
  return Result.Ok(result);
}
)");
  EXPECT_EQ(value, 7);
}

// ============================================================================
// Multiple Heap Allocations
// ============================================================================

TEST(MemorySafety_Allocator, multiple_heap_allocations) {
  auto value = executeStringWithStdlib(R"(
using std;

class Counter {
  var value: i32;

  init(v: i32) {
    this.value = v;
  }
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var c1 = Unique<Counter>(try alloc.create<Counter>(10));
  var c2 = Unique<Counter>(try alloc.create<Counter>(20));
  var c3 = Unique<Counter>(try alloc.create<Counter>(30));
  return Result.Ok(c1.get().value + c2.get().value + c3.get().value);
}
)");
  EXPECT_EQ(value, 60);
}

// ============================================================================
// Allocator Passed as Parameter
// ============================================================================

TEST(MemorySafety_Allocator, allocator_as_parameter) {
  auto value = executeStringWithStdlib(R"(
using std;

class Point {
  var x: i32;
  var y: i32;

  init(x: i32, y: i32) {
    this.x = x;
    this.y = y;
  }
}

function create_point(alloc: ref HeapAllocator, x: i32, y: i32) Result<raw_ptr<Point>> {
  return Result.Ok(try alloc.create<Point>(x, y));
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var p = Unique<Point>(try create_point(alloc, 5, 7));
  return Result.Ok(p.get().x + p.get().y);
}
)");
  EXPECT_EQ(value, 12);
}

// ============================================================================
// Intrinsic Tests
// ============================================================================

TEST(MemorySafety_Allocator, sizeof_intrinsic_i32) {
  auto value = executeString(R"(
    function main() i64 {
        return _sizeof<i32>();
    }
  )");
  EXPECT_EQ(value, 4);
}

TEST(MemorySafety_Allocator, sizeof_intrinsic_i64) {
  auto value = executeString(R"(
    function main() i64 {
        return _sizeof<i64>();
    }
  )");
  EXPECT_EQ(value, 8);
}

TEST(MemorySafety_Allocator, sizeof_intrinsic_class) {
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        init() {
            this.x = 0;
            this.y = 0;
        }
    }
    
    function main() i64 {
        return _sizeof<Point>();
    }
  )");
  EXPECT_EQ(value, 8);  // Two i32 fields = 8 bytes
}

TEST(MemorySafety_Allocator, init_intrinsic_with_allocator) {
  // Test _init<T> with allocator-allocated memory
  auto value = executeStringWithStdlib(R"(
using std;

class Point {
  var x: i32;
  var y: i32;

  init(x: i32, y: i32) {
    this.x = x;
    this.y = y;
  }

  method sum() i32 {
    return this.x + this.y;
  }
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var p = try alloc.create<Point>(10, 20);
  return Result.Ok(unsafe { p.sum(); });
}
)");
  EXPECT_EQ(value, 30);  // 10 + 20
}

// ============================================================================
// Overloaded constructors via _params_of<T> variadic factory
// ============================================================================

TEST(MemorySafety_Allocator, create_selects_init_overload_by_args) {
  // The same create<Point> specialization site is used with two different
  // arities; each must select the matching init overload.
  auto value = executeStringWithStdlib(R"(
using std;

class Point {
  var x: i32;
  var y: i32;

  init(x: i32, y: i32) {
    this.x = x;
    this.y = y;
  }

  init(v: i32) {
    this.x = v;
    this.y = v;
  }
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var a = Unique<Point>(try alloc.create<Point>(7));  // init(i32) -> 14
  var b = Unique<Point>(try alloc.create<Point>(3, 4));  // init(i32,i32) -> 7
  return Result.Ok(a.get().x + a.get().y + b.get().x + b.get().y);
}
)");
  EXPECT_EQ(value, 21);  // 14 + 7
}

TEST(MemorySafety_Allocator, create_single_arg_init_overload) {
  auto value = executeStringWithStdlib(R"(
using std;

class Point {
  var x: i32;
  var y: i32;

  init(x: i32, y: i32) {
    this.x = x;
    this.y = y;
  }

  init(v: i32) {
    this.x = v;
    this.y = v;
  }
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var p = Unique<Point>(try alloc.create<Point>(9));
  return Result.Ok(p.get().x + p.get().y);  // 9 + 9
}
)");
  EXPECT_EQ(value, 18);
}

TEST(MemorySafety_Allocator, create_no_matching_init_overload_errors) {
  // No 3-arg init exists; this must be a clean semantic error, not a crash.
  EXPECT_THROW(executeStringWithStdlib(R"(
using std;

class Point {
  var x: i32;
  var y: i32;

  init(x: i32, y: i32) {
    this.x = x;
    this.y = y;
  }
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var p = Unique<Point>(try alloc.create<Point>(1, 2, 3));
  return Result.Ok(0);
}
)"),
               sun::support::SunError);
}

// ============================================================================
// create_unique<T> Tests
// ============================================================================

TEST(MemorySafety_Allocator, create_unique_basic) {
  auto value = executeStringWithStdlib(R"(
using std;

class Point {
  var x: i32;
  var y: i32;

  init(x: i32, y: i32) {
    this.x = x;
    this.y = y;
  }

  method sum() i32 {
    return this.x + this.y;
  }
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var p = try alloc.create_unique<Point>(3, 4);
  return Result.Ok(p.get().sum());
}
)");
  EXPECT_EQ(value, 7);
}

TEST(MemorySafety_Allocator, create_unique_overloaded_init) {
  auto value = executeStringWithStdlib(R"(
using std;

class Point {
  var x: i32;
  var y: i32;

  init(v: i32) {
    this.x = v;
    this.y = v;
  }

  init(x: i32, y: i32) {
    this.x = x;
    this.y = y;
  }
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var a = try alloc.create_unique<Point>(7);
  var b = try alloc.create_unique<Point>(3, 4);
  return Result.Ok(a.get().x + a.get().y + b.get().x + b.get().y);
}
)");
  EXPECT_EQ(value, 21);
}

TEST(MemorySafety_Allocator, create_unique_deinit_called_once) {
  // Returning Unique<T> by value from create_unique must move, not copy:
  // exactly one deinit per object, no double free.
  auto value = executeStringWithStdlib(R"(
using std;

var counter: i32 = 0;

class Resource {
  var value: i32;

  init(v: i32) {
    this.value = v;
  }

  deinit() {
    counter = counter + 1;
  }
}

function helper() Result<i32> {
  var alloc = make_heap_allocator();
  var r1 = try alloc.create_unique<Resource>(10);
  var r2 = try alloc.create_unique<Resource>(20);
  return Result.Ok(r1.get().value + r2.get().value);
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var result = try helper();
  return Result.Ok(counter * 100 + result);
}
)");
  // 2 deinits, values sum to 30
  EXPECT_EQ(value, 230);
}

TEST(MemorySafety_Allocator, create_unique_returned_from_function) {
  auto value = executeStringWithStdlib(R"(
using std;

var counter: i32 = 0;

class Resource {
  var value: i32;

  init(v: i32) {
    this.value = v;
  }

  deinit() {
    counter = counter + 1;
  }
}

function make(alloc: ref HeapAllocator, v: i32) Result<Unique<Resource>> {
  return Result.Ok(try alloc.create_unique<Resource>(v));
}

var before: i32 = 0;

function use(alloc: ref HeapAllocator) Result<i32> {
  var r = try make(alloc, 5);
  var v: i32 = r.get().value;
  before = counter;  // no deinit yet while r is alive
  return Result.Ok(v);
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var v = try use(alloc);
  // deinit ran exactly once, and only after use() returned
  return Result.Ok(before * 100 + counter * 10 + v);
}
)");
  EXPECT_EQ(value, 15);
}

// ============================================================================
// Fixed parameters before the pack
// ============================================================================

// A method may declare ordinary parameters before its `args...`. Only what is
// left after them fills the pack — getting the split wrong pushes `tag` into
// the pack and leaves Point without its arguments.
TEST(MemorySafety_Allocator, method_fixed_params_precede_the_pack) {
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        init(x: i32, y: i32) { this.x = x; this.y = y; }
        method sum() i32 { return this.x + this.y; }
    }

    class TaggedAllocator {
        init() {}
        method create<T>(tag: i32, args...: _params_of<T>) i32 {
            var size: i64 = _sizeof<T>();
            var memory: raw_ptr<i8> = unsafe { _malloc(size); };
            unsafe { _init<T>(memory, args...); };
            var total = unsafe { _to_ref<T>(memory).sum(); };
            unsafe { _free(memory); };
            return tag * 100 + total;
        }
    }

    function main() i32 {
        var alloc = TaggedAllocator();
        return alloc.create<Point>(2, 3, 4);
    }
  )");
  EXPECT_EQ(value, 207);
}

TEST(MemorySafety_Allocator, method_too_few_arguments_for_fixed_params) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Point {
        var x: i32;
        init(x: i32) { this.x = x; }
    }

    class TaggedAllocator {
        init() {}
        method create<T>(tag: i32, args...: _params_of<T>) i32 { return tag; }
    }

    function main() i32 {
        var alloc = TaggedAllocator();
        return alloc.create<Point>();
    }
  )"),
                                "expects at least 1 argument");
}

TEST(MemorySafety_Allocator, init_primitive_overwrites_existing_storage) {
  for (const std::string type : {"i8", "u8", "i16", "u16", "i32", "u32", "i64",
                                 "u64", "f32", "f64", "bool"}) {
    SCOPED_TRACE(type);
    const bool boolean = type == "bool";
    const bool floating = type[0] == 'f';
    const std::string initial = boolean ? "true" : floating ? "3.5" : "37";
    const std::string zero = boolean ? "false" : floating ? "0.0" : "0";
    auto value = executeString(
        "function main() i32 { var p = unsafe { _malloc(_sizeof<" + type +
        ">()); }; var initial: " + type + " = " + initial +
        "; var zero: " + type + " = " + zero + R"(;
        var result: i32 = 0;
        unsafe {
          _store<)" +
        type + R"(>(p, 0, initial);
          _init<)" +
        type + R"(>(p, zero);
          if (_load<)" +
        type + R"(>(p, 0) != zero) { result = 1; }
          _init<)" +
        type + R"(>(p, initial);
          if (_load<)" +
        type + R"(>(p, 0) != initial) { result = 2; }
          _init<)" +
        type + R"(>(p);
          if (_load<)" +
        type + R"(>(p, 0) != zero) { result = 3; }
          _free(p);
        };
        return result;
      })");
    EXPECT_EQ(value, 0);
  }
}

TEST(MemorySafety_Allocator,
     init_primitive_evaluates_and_widens_argument_once) {
  EXPECT_EQ(executeString(R"(
    var calls: i32 = 0;
    function next() i32 { calls = calls + 1; return -42; }
    function main() i32 {
      var p = unsafe { _malloc(_sizeof<i64>()); };
      var result: i32 = 0;
      unsafe {
        _init<i64>(p, next());
        if (_load<i64>(p, 0) != -42) { result = 1; }
        _free(p);
      };
      if (calls != 1) { result = 2; }
      return result;
    }
  )"),
            0);
}

TEST(MemorySafety_Allocator, init_primitive_rejects_invalid_arguments) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
      var p = unsafe { _malloc(_sizeof<i32>()); };
      unsafe { _init<i32>(p, 1, 2); _free(p); };
      return 0;
    }
  )"),
               std::exception);
  EXPECT_THROW(executeString(R"(
    function main() i32 {
      var p = unsafe { _malloc(_sizeof<i32>()); };
      unsafe { _init<i32>(p, 1.5); _free(p); };
      return 0;
    }
  )"),
               std::exception);
}

TEST(MemorySafety_Allocator, heap_allocator_creates_primitive_values) {
  EXPECT_EQ(executeStringWithStdlib(R"(
using std;

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  var alloc = make_heap_allocator();
  var p = try alloc.create<i32>(42);
  var result: i32 = 0;
  unsafe {
    if (_load<i32>(p, 0) != 42) {
      result = 1;
    }
  };
  alloc.dealloc<i32>(p, 1);
  var zero = try alloc.create_unique<i32>(0);
  var negative = try alloc.create_unique<i64>(-42);
  var floating = try alloc.create_unique<f64>(3.5);
  var boolean = try alloc.create_unique<bool>(true);
  if (zero.get() != 0) {
    result = 2;
  }
  if (negative.get() != -42) {
    result = 3;
  }
  if (floating.get() != 3.5) {
    result = 4;
  }
  if (boolean.get() == false) {
    result = 5;
  }
  return Result.Ok(result);
}
)"),
            0);
}
