// tests/memory_safety/test_borrow_checker.cpp
// Tests for the Rust-style borrow checker

#include <gtest/gtest.h>

#include "borrow_checker/borrow_checker.h"
#include "driver/execution_utils.h"

// ============================================================================
// Valid Borrow Patterns - Should Compile Successfully
// ============================================================================

TEST(MemorySafety_BorrowChecker, single_mutable_ref) {
  // Single mutable reference is allowed
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        ref r = x;
        r = 50;
        return x;
    };
  )");
  EXPECT_EQ(value, 50);
}

TEST(MemorySafety_BorrowChecker, ref_goes_out_of_scope) {
  // Reference going out of scope frees the borrow
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        if (true) {
            ref r = x;
            r = 20;
        };
        // x is no longer borrowed here
        x = 30;
        return x;
    };
  )");
  EXPECT_EQ(value, 30);
}

TEST(MemorySafety_BorrowChecker, sequential_refs_to_same_var) {
  // Sequential (non-overlapping) refs to same variable are allowed
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        if (true) {
            ref r1 = x;
            r1 = 20;
        };
        if (true) {
            ref r2 = x;
            r2 = 30;
        };
        return x;
    };
  )");
  EXPECT_EQ(value, 30);
}

TEST(MemorySafety_BorrowChecker, refs_to_different_vars) {
  // Multiple refs to different variables are allowed
  auto value = executeString(R"(
    function main() i32 {
        var a: i32 = 5;
        var b: i32 = 10;
        ref ra = a;
        ref rb = b;
        ra = ra + rb;
        return a;
    };
  )");
  EXPECT_EQ(value, 15);
}

TEST(MemorySafety_BorrowChecker, ref_passed_to_function) {
  // Passing a ref to a function works
  auto value = executeString(R"(
    function increment(x: ref i32) void {
        x = x + 1;
    };

    function main() i32 {
        var val: i32 = 10;
        increment(val);
        return val;
    };
  )");
  EXPECT_EQ(value, 11);
}

TEST(MemorySafety_BorrowChecker, nested_function_with_ref_param) {
  // Function taking ref parameter, called from main
  auto value = executeString(R"(
    function double_it(x: ref i32) void {
        x = x * 2;
    };

    function main() i32 {
        var n: i32 = 7;
        double_it(n);
        return n;
    };
  )");
  EXPECT_EQ(value, 14);
}

// ============================================================================
// Borrow Violations - Should Be Caught by Borrow Checker
// ============================================================================

TEST(MemorySafety_BorrowChecker, double_mutable_borrow_is_error) {
  // Two simultaneous mutable borrows of the same variable is an error
  EXPECT_THROW(executeString(R"(
        function main() i32 {
            var x: i32 = 10;
            ref r1 = x;
            ref r2 = x;
            r1 = 50;
            return r2;
        };
      )"),
               SunError);
}

// ============================================================================
// Return Type Restrictions - References Cannot Be Returned
// ============================================================================

TEST(MemorySafety_BorrowChecker, function_returns_value_not_ref) {
  // Returning the VALUE through a ref is fine (value is copied)
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 42;
        ref r = x;
        return r;  // Returns the value, not the reference
    };
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Class/Struct Field Restrictions
// ============================================================================

TEST(MemorySafety_BorrowChecker, class_without_ref_fields) {
  // Classes can have regular fields
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        
        function init(x: i32, y: i32) {
            this.x = x;
            this.y = y;
        };
        
        function sum() i32 {
            return this.x + this.y;
        };
    };

    function main() i32 {
        var p = Point(3, 4);
        return p.sum();
    };
  )");
  EXPECT_EQ(value, 7);
}

// ============================================================================
// Scope-Based Borrow Invalidation
// ============================================================================

TEST(MemorySafety_BorrowChecker, borrow_ends_at_scope_exit) {
  // Borrow should end when the reference goes out of scope
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 100;
        if (true) {
            ref r = x;
            r = 200;
        };
        // r is out of scope, x is no longer borrowed
        return x;
    };
  )");
  EXPECT_EQ(value, 200);
}

TEST(MemorySafety_BorrowChecker, while_loop_borrow) {
  // Borrow inside while loop should be scoped properly
  auto value = executeString(R"(
    function main() i32 {
        var count: i32 = 0;
        var i: i32 = 0;
        while (i < 3) {
            ref r = count;
            r = r + 10;
            i = i + 1;
        };
        return count;
    };
  )");
  EXPECT_EQ(value, 30);
}

TEST(MemorySafety_BorrowChecker, error_on_string_use_after_move) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using sun;

    function consume(s: ref String) void {
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var s1 = String(allocator, "Hello World");
        var s2 = s1;
        consume(s1);
        return 0;
    };
  )"),
               std::exception);
}

// ============================================================================
// Compound Types Passed by Value with Move Semantics
// ============================================================================

// REQUIRE_REF_FOR_COMPOUND_PARAMS is false, so passing classes by value
// is allowed with move semantics - the original variable becomes invalid
TEST(MemorySafety_BorrowChecker, compound_type_by_value_moves) {
  // Passing a class by value should work and move the value
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        function init(x: i32, y: i32) {
            this.x = x;
            this.y = y;
        }
    }

    function consume(p: Point) i32 {
        return p.x + p.y;
    }

    function main() i32 {
        var p = Point(3, 4);
        return consume(p);
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(MemorySafety_BorrowChecker, use_after_move_by_value_param) {
  // Using a variable after passing it by value should be an error
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        function init(x: i32, y: i32) {
            this.x = x;
            this.y = y;
        }
    }

    function consume(p: Point) i32 {
        return p.x + p.y;
    }

    function main() i32 {
        var p = Point(3, 4);
        var r = consume(p);  // p is moved here
        return p.x;          // ERROR: use of moved variable
    }
  )"),
                                "Borrow check failed");
}

TEST(MemorySafety_BorrowChecker, compound_type_with_ref_works) {
  // Passing by ref should work
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init(n: i32) {
            this.count = n;
        }
    }

    function sum_counters(a: ref Counter, b: ref Counter) i32 {
        return a.count + b.count;
    }

    function main() i32 {
        var c1 = Counter(10);
        var c2 = Counter(20);
        return sum_counters(c1, c2);
    }
  )");
  EXPECT_EQ(value, 30);
}

// ============================================================================
// Temporary Ownership Transfer
// ============================================================================

TEST(MemorySafety_BorrowChecker, temporary_passed_to_ref_param) {
  // Passing a class temporary to a ref param should work
  auto value = executeString(R"(
    class Wrapper {
        var value: i32;
        function init(v: i32) {
            this.value = v;
        }
    }

    function get_value(w: ref Wrapper) i32 {
        return w.value;
    }

    function main() i32 {
        return get_value(Wrapper(42));
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_BorrowChecker, temporary_passed_to_ref_param_with_deinit) {
  // Temporary with deinit passed to ref param - deinit should run after call
  auto value = executeString(R"(
    class Counter {
        var count: i32;
        function init(n: i32) {
            this.count = n;
        }
        function deinit() void {
            // deinit called when temporary is destroyed
        }
    }

    function double_count(c: ref Counter) i32 {
        return c.count * 2;
    }

    function main() i32 {
        return double_count(Counter(21));
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_BorrowChecker, multiple_temporaries_to_ref_params) {
  // Multiple temporaries passed to different ref params
  auto value = executeString(R"(
    class Num {
        var n: i32;
        function init(v: i32) {
            this.n = v;
        }
    }

    function add_nums(a: ref Num, b: ref Num) i32 {
        return a.n + b.n;
    }

    function main() i32 {
        return add_nums(Num(17), Num(25));
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_BorrowChecker, temporary_with_method_call) {
  // Call method on temporary then pass to ref param
  auto value = executeString(R"(
    class Box {
        var val: i32;
        function init(v: i32) {
            this.val = v;
        }
        function get() i32 {
            return this.val;
        }
    }

    function extract(b: ref Box) i32 {
        return b.get();
    }

    function main() i32 {
        return extract(Box(42));
    }
  )");
  EXPECT_EQ(value, 42);
}
// ============================================================================
// Match bindings borrow from the discriminant
// ============================================================================

// A compound match binding borrows the payload of the matched value, so a
// ref derived from it lives as long as that value: returning it is fine when
// the discriminant is `this` or a ref parameter.
TEST(MemorySafety_BorrowChecker, ref_return_through_match_binding_of_this) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    class OutOfRange implements IError {
      function init() {}
      function code() i32 { return 3; }
      function message() String { return String("out of range"); }
    }

    enum Value {
      Items(Vec<String>),
      Text(String),
      Empty
    }

    class Holder {
      var v: Value;
      function init(v: Value) { this.v = v; }
      function at(i: i64) ref String, IError {
        match this.v {
          Value.Items(items) => { return items.get(i); },
          Value.Text(s) => { return s; },
          _ => { throw OutOfRange(); }
        };
      }
    }

    function first(h: ref Holder) ref String, IError {
      match h.v {
        Value.Items(items) => { return items.get(0); },
        _ => { throw OutOfRange(); }
      };
    }

    function main() i32, IError {
      var alloc = make_heap_allocator();
      var items = Vec<String>(alloc, 2);
      items.push(String(alloc, "ab"));
      items.push(String(alloc, "cde"));
      var h = Holder(Value.Items(items));
      var t = Holder(Value.Text(String(alloc, "z")));
      return _convert<i32>(h.at(1).length() + first(h).length() + t.at(0).length());
    }
  )");
  EXPECT_EQ(value, 6);
}

TEST(MemorySafety_BorrowChecker,
     ref_return_through_match_binding_of_local_is_dangling) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using sun;

    enum Value {
      Items(Vec<String>),
      Empty
    }

    function first(alloc: ref HeapAllocator) ref String {
      var local = Value.Items(Vec<String>(alloc, 1));
      match local {
        Value.Items(items) => { return items.get(0); },
        _ => { }
      };
      return unsafe { _to_ref<String>(null); };
    }

    function main() i32 {
      var alloc = make_heap_allocator();
      return _convert<i32>(first(alloc).length());
    }
  )"),
               SunError);
}

// ============================================================================
// Flow Sensitivity Across Branches
//
// A branch that returns or throws never reaches the code after it, so a move
// on that branch must not mark the variable moved on the paths that do.
// ============================================================================

TEST(MemorySafety_BorrowChecker, return_in_catch_does_not_move_on_fallthrough) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function pick(alloc: ref HeapAllocator, n: i64) Vec<String>, IError {
      var out = Vec<String>(alloc, 4);
      try {
        if (n == 3) { throw EmptyError(); }
      } catch (e: IError) {
        return out;
      }
      if (n < 2) { return out; }
      out.push(String(alloc, "ok"));
      return out;
    }

    function main() i32 {
      var alloc = make_heap_allocator();
      try {
        var v = pick(alloc, 5);
        return _convert<i32>(v.size());
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_BorrowChecker,
     catch_clauses_are_alternatives_not_a_sequence) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function pick(alloc: ref HeapAllocator, n: i64) Vec<String>, IError {
      var out = Vec<String>(alloc, 4);
      try {
        if (n == 3) { throw EmptyError(); }
      } catch (e: EmptyError) {
        return out;
      } catch (e: IError) {
        return out;
      }
      return out;
    }

    function main() i32 {
      var alloc = make_heap_allocator();
      try {
        var v = pick(alloc, 5);
        return _convert<i32>(v.size());
      } catch (e: IError) {
        return -1;
      }
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(MemorySafety_BorrowChecker,
     return_in_match_arm_does_not_move_on_fallthrough) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function pick(alloc: ref HeapAllocator, o: Option<i64>) Vec<String> {
      var out = Vec<String>(alloc, 4);
      match o {
        Option.Some(v) => { return out; },
        Option.None => { out.push(String(alloc, "x")); }
      };
      return out;
    }

    function main() i32 {
      var alloc = make_heap_allocator();
      var v = pick(alloc, Option.None);
      return _convert<i32>(v.size());
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_BorrowChecker, error_on_move_in_falling_through_catch) {
  // The catch clause moves and does NOT diverge, so the later use is a
  // genuine use-after-move and must still be rejected.
  EXPECT_THROW(executeStringWithStdlib(R"(
    using sun;

    function consume(v: Vec<String>) i64 { return v.size(); }

    function f(alloc: ref HeapAllocator, n: i64) Vec<String>, IError {
      var out = Vec<String>(alloc, 4);
      try {
        if (n == 3) { throw EmptyError(); }
      } catch (e: IError) {
        var k = consume(out);
      }
      return out;
    }

    function main() i32 {
      var alloc = make_heap_allocator();
      try { var v = f(alloc, 1); return 0; } catch (e: IError) { return -1; }
    }
  )"),
               SunError);
}

TEST(MemorySafety_BorrowChecker, error_on_move_in_falling_through_match_arm) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using sun;

    function consume(v: Vec<String>) i64 { return v.size(); }

    function f(alloc: ref HeapAllocator, o: Option<i64>) Vec<String> {
      var out = Vec<String>(alloc, 4);
      match o {
        Option.Some(v) => { var k = consume(out); },
        Option.None => { }
      };
      return out;
    }

    function main() i32 {
      var alloc = make_heap_allocator();
      var v = f(alloc, Option.None);
      return 0;
    }
  )"),
               SunError);
}

TEST(MemorySafety_BorrowChecker, error_on_move_in_try_block_used_after) {
  // A move inside the try block is on the normal path and still propagates
  // into the catch clauses and past the try/catch.
  EXPECT_THROW(executeStringWithStdlib(R"(
    using sun;

    function consume(v: Vec<String>) i64 { return v.size(); }

    function f(alloc: ref HeapAllocator) Vec<String>, IError {
      var out = Vec<String>(alloc, 4);
      try {
        var k = consume(out);
      } catch (e: IError) {
        return out;
      }
      return out;
    }

    function main() i32 {
      var alloc = make_heap_allocator();
      try { var v = f(alloc); return 0; } catch (e: IError) { return -1; }
    }
  )"),
               SunError);
}

// ============================================================================
// Partial moves: a field moved out of its object
//
// Reading a compound field by value moves it out and leaves the field empty.
// That is allowed, but the checker tracks it: the field cannot be read again
// until a value is put back, and the object cannot be used as a whole while
// one of its fields is missing.
// ============================================================================

TEST(MemorySafety_BorrowChecker, error_on_use_of_moved_field) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Inner {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function deinit() void { }
        function get() i32 { return this.v; }
    }
    class Config {
        var line: Inner;
        function init() { this.line = Inner(7); }
        function deinit() void { }
    }

    function main() i32 {
        var cfg = Config();
        var subject = cfg.line;   // moves the field out
        return cfg.line.get();    // ERROR: use of moved field
    }
  )"),
                                "Borrow check failed");
}

TEST(MemorySafety_BorrowChecker, error_on_whole_object_use_after_field_move) {
  // Borrowing the object hands the callee a field that is no longer there
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Inner {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function deinit() void { }
        function get() i32 { return this.v; }
    }
    class Config {
        var line: Inner;
        function init() { this.line = Inner(7); }
        function deinit() void { }
    }

    function read(c: ref Config) i32 { return c.line.get(); }

    function main() i32 {
        var cfg = Config();
        var subject = cfg.line;
        return read(cfg);         // ERROR: cfg is partially moved
    }
  )"),
                                "Borrow check failed");
}

TEST(MemorySafety_BorrowChecker, error_on_borrow_of_moved_field) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Inner {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function deinit() void { }
        function get() i32 { return this.v; }
    }
    class Config {
        var line: Inner;
        function init() { this.line = Inner(7); }
        function deinit() void { }
    }

    function main() i32 {
        var cfg = Config();
        var subject = cfg.line;
        ref r = cfg.line;         // ERROR: borrow of moved field
        return r.get();
    }
  )"),
                                "Borrow check failed");
}

TEST(MemorySafety_BorrowChecker, error_on_use_of_moved_field_of_this) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Inner {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function deinit() void { }
        function get() i32 { return this.v; }
    }
    class Holder {
        var inner: Inner;
        function init() { this.inner = Inner(9); }
        function deinit() void { }
        function bad() i32 {
            var taken = this.inner;
            return this.inner.get();   // ERROR: use of moved field
        }
    }

    function main() i32 {
        var h = Holder();
        return h.bad();
    }
  )"),
                                "Borrow check failed");
}

TEST(MemorySafety_BorrowChecker, sibling_field_and_refill_after_field_move) {
  // Moving one field out leaves the others readable, and assigning a value
  // back into the moved field makes the object whole again
  auto value = executeString(R"(
    class Inner {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function deinit() void { }
        function get() i32 { return this.v; }
    }
    class Config {
        var line: Inner;
        var query: Inner;
        function init() { this.line = Inner(7); this.query = Inner(3); }
        function deinit() void { }
    }

    function main() i32 {
        var cfg = Config();
        var subject = cfg.line;      // moves the field out
        var q = cfg.query.get();     // sibling field is still there
        cfg.line = Inner(5);         // field owns a value again
        return subject.get() * 100 + q * 10 + cfg.line.get();
    }
  )");
  EXPECT_EQ(value, 735);
}

TEST(MemorySafety_BorrowChecker, borrowing_a_field_does_not_move_it) {
  auto value = executeString(R"(
    class Inner {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function deinit() void { }
        function get() i32 { return this.v; }
    }
    class Config {
        var line: Inner;
        function init() { this.line = Inner(7); }
        function deinit() void { }
    }

    function main() i32 {
        var cfg = Config();
        ref subject = cfg.line;
        return subject.get() * 10 + cfg.line.get();
    }
  )");
  EXPECT_EQ(value, 77);
}

// ============================================================================
// Reading a class out of a borrow
//
// Only scalars (and arrays, which are fat pointers into storage owned
// elsewhere) can be duplicated by a read. A class value has one owner, so a
// borrow of one cannot be turned back into a value - with or without drop glue.
// ============================================================================

TEST(MemorySafety_BorrowChecker, error_on_returning_borrowed_class_by_value) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Cursor {
        var pos: i32;
        function init(p: i32) { this.pos = p; }
        function get() i32 { return this.pos; }
    }

    function snapshot(c: ref Cursor) Cursor {
        return c;                 // ERROR: reading a class out of a borrow
    }

    function main() i32 {
        var c = Cursor(7);
        var copy = snapshot(c);
        return copy.get();
    }
  )"),
                                "Cannot return a borrowed");
}

TEST(MemorySafety_BorrowChecker, error_on_storing_borrowed_class_in_field) {
  EXPECT_THROW(executeString(R"(
    class Cursor {
        var pos: i32;
        function init(p: i32) { this.pos = p; }
    }
    class Holder {
        var cur: Cursor;
        function init(c: ref Cursor) { this.cur = c; }
    }

    function main() i32 {
        var c = Cursor(7);
        var h = Holder(c);
        return 0;
    }
  )"),
               std::exception);
}

// ============================================================================
// Loop-carried moves
//
// A move that is still standing when a loop body ends runs again on the next
// iteration, against a value that is already gone.
// ============================================================================

TEST(MemorySafety_BorrowChecker, error_on_variable_moved_every_iteration) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Inner {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function deinit() void { }
        function get() i32 { return this.v; }
    }

    function main() i32 {
        var src = Inner(7);
        var total = 0;
        for (var i: i32 = 0; i < 3; i = i + 1) {
            var taken = src;      // ERROR: moved again on the next iteration
            total = total + taken.get();
        }
        return total;
    }
  )"),
                                "Borrow check failed");
}

TEST(MemorySafety_BorrowChecker, error_on_field_moved_every_iteration) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Inner {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function deinit() void { }
        function get() i32 { return this.v; }
    }
    class Config {
        var line: Inner;
        function init() { this.line = Inner(7); }
        function deinit() void { }
    }

    function main() i32 {
        var cfg = Config();
        var total = 0;
        for (var i: i32 = 0; i < 3; i = i + 1) {
            var taken = cfg.line;   // ERROR: the field is gone next iteration
            total = total + taken.get();
        }
        return total;
    }
  )"),
                                "Borrow check failed");
}

TEST(MemorySafety_BorrowChecker,
     loop_moves_a_value_made_in_the_same_iteration) {
  // A value created inside the body is fresh each time round
  auto value = executeString(R"(
    class Inner {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function deinit() void { }
        function get() i32 { return this.v; }
    }

    function consume(i: Inner) i32 { return i.get(); }

    function main() i32 {
        var total = 0;
        for (var i: i32 = 0; i < 3; i = i + 1) {
            var tmp = Inner(2);
            total = total + consume(tmp);
        }
        return total;
    }
  )");
  EXPECT_EQ(value, 6);
}

TEST(MemorySafety_BorrowChecker, loop_refills_the_moved_field_before_the_end) {
  auto value = executeString(R"(
    class Inner {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function deinit() void { }
        function get() i32 { return this.v; }
    }
    class Config {
        var line: Inner;
        function init() { this.line = Inner(7); }
        function deinit() void { }
    }

    function main() i32 {
        var cfg = Config();
        var total = 0;
        for (var i: i32 = 0; i < 3; i = i + 1) {
            var taken = cfg.line;
            total = total + taken.get();
            cfg.line = Inner(1);      // whole again before the iteration ends
        }
        return total;
    }
  )");
  EXPECT_EQ(value, 9);
}

TEST(MemorySafety_BorrowChecker, loop_body_that_returns_moves_once) {
  auto value = executeString(R"(
    class Inner {
        var v: i32;
        function init(v: i32) { this.v = v; }
        function deinit() void { }
        function get() i32 { return this.v; }
    }
    class Config {
        var line: Inner;
        function init() { this.line = Inner(7); }
        function deinit() void { }
    }

    function main() i32 {
        var cfg = Config();
        while (true) {
            var taken = cfg.line;     // the body cannot run twice
            return taken.get();
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 7);
}

// ============================================================================
// Assigning while borrowed - overwriting a compound value would drop the
// storage a live borrow points into, so it is rejected; writing through the
// borrow itself replaces the referent correctly.
// ============================================================================

TEST(MemorySafety_BorrowChecker, assigning_through_ref_replaces_the_referent) {
  // Assignment through a ref drops the old referent and moves the new value
  // in - it must never store the source's address over the referent's bytes
  auto value = executeString(R"(
    class Box {
        var v: i32;
        function init(v: i32) { this.v = v; }
    }
    function main() i32 {
        var b: Box = Box(1);
        var c: ref Box = b;
        c = Box(42);
        return b.v + c.v;
    };
  )");
  EXPECT_EQ(value, 84);
}

TEST(MemorySafety_BorrowChecker, assigning_to_borrowed_compound_is_error) {
  // Replacing b would drop the object c still points into
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Box {
        var v: i32;
        function init(v: i32) { this.v = v; }
    }
    function main() i32 {
        var b: Box = Box(1);
        var c: ref Box = b;
        b = Box(2);
        return c.v;
    };
  )"),
                                "Borrow check failed");
}

TEST(MemorySafety_BorrowChecker, ref_returning_call_borrows_its_arguments) {
  // The returned ref could point into either argument, so both stay
  // borrowed while c lives and neither may be reassigned
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Box {
        var v: i32;
        function init(v: i32) { this.v = v; }
    }
    function pick(a: ref Box, b: ref Box) ref Box {
        if (a.v > b.v) { return a; } else { return b; }
    }
    function main() i32 {
        var a: Box = Box(1);
        var b: Box = Box(2);
        var c: ref Box = pick(a, b);
        a = Box(3);
        return c.v;
    };
  )"),
                                "Borrow check failed");
}

TEST(MemorySafety_BorrowChecker, assigning_through_call_returned_ref_works) {
  // c binds whichever argument pick chose at run time; assigning through it
  // replaces that object in place
  auto value = executeString(R"(
    class Box {
        var v: i32;
        function init(v: i32) { this.v = v; }
    }
    function pick(a: ref Box, b: ref Box) ref Box {
        if (a.v > b.v) { return a; } else { return b; }
    }
    function main() i32 {
        var a: Box = Box(1);
        var b: Box = Box(2);
        var c: ref Box = pick(a, b);
        c = Box(9);
        return a.v + b.v;
    };
  )");
  EXPECT_EQ(value, 10);
}
