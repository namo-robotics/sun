// tests/classes/test_const_methods.cpp - `const function` methods: `this` is
// immutable inside, and only they may be called on a constant receiver.

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

namespace {

const char* kCounter = R"(
      class Counter {
          var n: i32;
          function init(n: i32) { this.n = n; }
          public const function get() i32 { return this.n; }
          const function is_zero() bool { return this.n == 0; }
          function bump() void { this.n = this.n + 1; }
      }
)";

}  // namespace

// ============================================================================
// Calling const methods
// ============================================================================

TEST(Classes_ConstMethods, const_method_on_const_receiver) {
  auto value = executeString(std::string(kCounter) + R"(
      function main() i32 {
          const c = Counter(41);
          if (c.is_zero()) { return -1; }
          return c.get() + 1;
      }
    )");
  EXPECT_EQ(value, 42);
}

TEST(Classes_ConstMethods, const_method_on_var_receiver) {
  auto value = executeString(std::string(kCounter) + R"(
      function main() i32 {
          var c = Counter(1);
          c.bump();
          return c.get();
      }
    )");
  EXPECT_EQ(value, 2);
}

TEST(Classes_ConstMethods, const_method_calls_const_method_on_this) {
  auto value = executeString(R"(
      class Pair {
          var a: i32;
          var b: i32;
          function init(a: i32, b: i32) { this.a = a; this.b = b; }
          const function sum() i32 { return this.a + this.b; }
          const function doubled() i32 { return this.sum() * 2; }
      }
      function main() i32 {
          const p = Pair(3, 4);
          return p.doubled();
      }
    )");
  EXPECT_EQ(value, 14);
}

TEST(Classes_ConstMethods, non_const_method_on_const_receiver_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(std::string(kCounter) + R"(
      function main() i32 {
          const c = Counter(1);
          c.bump();
          return c.get();
      }
    )"),
      "Cannot call non-const method 'bump' on constant 'c'");
}

TEST(Classes_ConstMethods, non_const_method_on_const_field_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(std::string(kCounter) + R"(
      class Holder {
          var c: Counter;
          function init() { this.c = Counter(0); }
      }
      function main() i32 {
          const h = Holder();
          h.c.bump();
          return h.c.get();
      }
    )"),
      "Cannot call non-const method 'bump' on constant 'h'");
}

// ============================================================================
// Inside a const method `this` is immutable
// ============================================================================

TEST(Classes_ConstMethods, field_store_in_const_method_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      class Counter {
          var n: i32;
          function init() { this.n = 0; }
          const function reset() void { this.n = 0; }
      }
      function main() i32 {
          var c = Counter();
          c.reset();
          return c.n;
      }
    )"),
      "Cannot assign to field 'n' of 'this' inside a const method");
}

TEST(Classes_ConstMethods, non_const_call_on_this_in_const_method_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      class Counter {
          var n: i32;
          function init() { this.n = 0; }
          function bump() void { this.n = this.n + 1; }
          const function poke() void { this.bump(); }
      }
      function main() i32 {
          var c = Counter();
          c.poke();
          return c.n;
      }
    )"),
      "Cannot call non-const method 'bump' on 'this' inside a const method");
}

TEST(Classes_ConstMethods, mutating_field_object_in_const_method_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeStringWithStdlib(R"(
      using sun;
      class Bag {
          var items: Vec<i32>;
          function init(alloc: ref HeapAllocator) { this.items = Vec<i32>(alloc, 4); }
          const function add(x: i32) void { this.items.push(x); }
      }
      function main() i32 {
          var allocator = make_heap_allocator();
          var b = Bag(allocator);
          b.add(1);
          return b.items.size();
      }
    )"),
      "Cannot call non-const method 'push' on 'this' inside a const method");
}

TEST(Classes_ConstMethods, mutable_ref_of_field_in_const_method_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      class Counter {
          var n: i32;
          function init() { this.n = 0; }
          const function poke() void { ref r = this.n; r = 1; }
      }
      function main() i32 {
          var c = Counter();
          c.poke();
          return c.n;
      }
    )"),
      "Cannot take a mutable reference to 'this' inside a const method");
}

TEST(Classes_ConstMethods, ref_argument_of_field_in_const_method_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      function bump(x: ref i32) void { x = x + 1; }
      class Counter {
          var n: i32;
          function init() { this.n = 0; }
          const function poke() void { bump(this.n); }
      }
      function main() i32 {
          var c = Counter();
          c.poke();
          return c.n;
      }
    )"),
      "Cannot pass as 'ref' argument 1 of 'bump' 'this' inside a const method");
}

TEST(Classes_ConstMethods, const_method_may_read_and_borrow_const) {
  auto value = executeString(R"(
      function peek(x: const ref i32) i32 { return x; }
      class Counter {
          var n: i32;
          function init(n: i32) { this.n = n; }
          const function twice() i32 {
              const ref r = this.n;
              return peek(this.n) + r;
          }
      }
      function main() i32 {
          const c = Counter(21);
          return c.twice();
      }
    )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Declarations
// ============================================================================

TEST(Classes_ConstMethods, const_init_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Counter {
          var n: i32;
          const function init() { this.n = 0; }
      }
      function main() i32 { return 0; }
    )"),
                                "'init' cannot be a const method");
}

TEST(Classes_ConstMethods, const_field_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      class Counter {
          const var n: i32;
          function init() { this.n = 0; }
      }
      function main() i32 { return 0; }
    )"),
                                "'const' is not allowed on fields");
}

TEST(Classes_ConstMethods, const_free_function_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      const function f() i32 { return 1; }
      function main() i32 { return f(); }
    )"),
      "'const function' is only allowed on class and interface methods");
}

TEST(Classes_ConstMethods, public_const_order) {
  auto value = executeString(R"(
      public class Counter {
          var n: i32;
          public function init(n: i32) { this.n = n; }
          public const function get() i32 { return this.n; }
      }
      function main() i32 {
          const c = Counter(5);
          return c.get();
      }
    )");
  EXPECT_EQ(value, 5);
}

// ============================================================================
// Bound methods
// ============================================================================

TEST(Classes_ConstMethods, bound_const_method_on_const_receiver) {
  auto value = executeString(std::string(kCounter) + R"(
      function main() i32 {
          const c = Counter(9);
          var get = c.get;
          return get();
      }
    )");
  EXPECT_EQ(value, 9);
}

TEST(Classes_ConstMethods,
     bound_non_const_method_on_const_receiver_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(std::string(kCounter) + R"(
      function main() i32 {
          const c = Counter(9);
          var bump = c.bump;
          bump();
          return c.get();
      }
    )"),
      "Cannot call non-const method 'bump' on constant 'c'");
}

// ============================================================================
// Interfaces
// ============================================================================

TEST(Classes_ConstMethods, interface_const_method_through_const_ref) {
  auto value = executeString(R"(
      interface IShape {
          const function area() i32;
      }
      class Square implements IShape {
          var side: i32;
          function init(s: i32) { this.side = s; }
          const function area() i32 { return this.side * this.side; }
      }
      function measure(s: const ref IShape) i32 { return s.area(); }
      function main() i32 {
          const sq = Square(6);
          return measure(sq) + sq.area();
      }
    )");
  EXPECT_EQ(value, 72);
}

TEST(Classes_ConstMethods, interface_const_method_needs_const_implementation) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      interface IShape {
          const function area() i32;
      }
      class Square implements IShape {
          var side: i32;
          function init(s: i32) { this.side = s; }
          function area() i32 { return this.side * this.side; }
      }
      function main() i32 { return 0; }
    )"),
                                "implements const member 'IShape.area' and "
                                "must be declared 'const function'");
}

TEST(Classes_ConstMethods, class_may_add_const_beyond_interface) {
  auto value = executeString(R"(
      interface IShape {
          function area() i32;
      }
      class Square implements IShape {
          var side: i32;
          function init(s: i32) { this.side = s; }
          const function area() i32 { return this.side * this.side; }
      }
      function main() i32 {
          const sq = Square(3);
          return sq.area();
      }
    )");
  EXPECT_EQ(value, 9);
}

TEST(Classes_ConstMethods,
     non_const_interface_method_on_const_ref_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      interface IShape {
          function grow() void;
      }
      class Square implements IShape {
          var side: i32;
          function init(s: i32) { this.side = s; }
          function grow() void { this.side = this.side + 1; }
      }
      function poke(s: const ref IShape) void { s.grow(); }
      function main() i32 {
          var sq = Square(3);
          poke(sq);
          return sq.side;
      }
    )"),
      "Cannot call non-const method 'grow' on const reference 's'");
}

// ============================================================================
// Generics
// ============================================================================

TEST(Classes_ConstMethods, generic_class_keeps_const) {
  auto value = executeString(R"(
      class Box<T> {
          var v: T;
          function init(v: T) { this.v = v; }
          const function get() T { return this.v; }
          function set(v: T) void { this.v = v; }
      }
      function main() i32 {
          const b = Box<i32>(42);
          return b.get();
      }
    )");
  EXPECT_EQ(value, 42);

  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      class Box<T> {
          var v: T;
          function init(v: T) { this.v = v; }
          const function get() T { return this.v; }
          function set(v: T) void { this.v = v; }
      }
      function main() i32 {
          const b = Box<i32>(42);
          b.set(1);
          return b.get();
      }
    )"),
      "Cannot call non-const method 'set' on constant 'b'");
}

TEST(Classes_ConstMethods, generic_const_method_body_is_checked) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
      class Box<T> {
          var v: T;
          function init(v: T) { this.v = v; }
          const function set(v: T) void { this.v = v; }
      }
      function main() i32 {
          var b = Box<i32>(42);
          b.set(1);
          return b.v;
      }
    )"),
      "Cannot assign to field 'v' of 'this' inside a const method");
}

// ============================================================================
// Stdlib containers through a constant receiver
// ============================================================================

TEST(Classes_ConstMethods, const_vec_reads) {
  auto value = executeStringWithStdlib(R"(
      using sun;
      function total(v: const ref Vec<i32>) i32 {
          var sum: i32 = 0;
          var i: i64 = 0;
          while (i < v.size()) {
              sum = sum + v[i] + v.get_unchecked(i);
              i = i + 1;
          }
          return sum;
      }
      function main() i32 {
          var allocator = make_heap_allocator();
          var v = Vec<i32>(allocator, 4);
          v.push(1);
          v.push(2);
          v.push(3);
          const ref cv = v;
          if (cv.is_empty()) { return -1; }
          return total(v) + _convert<i32>(cv.size());
      }
    )");
  EXPECT_EQ(value, 12 + 3);
}

TEST(Classes_ConstMethods, const_vec_cannot_push_or_set) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeStringWithStdlib(R"(
      using sun;
      function main() i32 {
          var allocator = make_heap_allocator();
          var v = Vec<i32>(allocator, 4);
          const ref cv = v;
          cv.push(1);
          return 0;
      }
    )"),
      "Cannot call non-const method 'push' on const reference 'cv'");

  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeStringWithStdlib(R"(
      using sun;
      function main() i32 {
          var allocator = make_heap_allocator();
          var v = Vec<i32>(allocator, 4);
          v.push(1);
          const ref cv = v;
          cv[0] = 2;
          return 0;
      }
    )"),
      "Cannot assign to an element of const reference 'cv'");
}

// A const method's result is its "const view": every `ref` in it, including
// inside a payload enum, is `const ref` when the receiver is constant.
TEST(Classes_ConstMethods, peek_accessors_are_const_views) {
  auto value = executeStringWithStdlib(R"(
      using sun;
      function main() i32 {
          var allocator = make_heap_allocator();
          var v = Vec<i32>(allocator, 4);
          v.push(5);
          v.push(7);
          var m = Map<i64, i32>(allocator, 8);
          m.insert(1, 30);
          var l = LinkedList<i32>(allocator);
          l.push_back(100);
          const ref cv = v;
          const ref cm = m;
          const ref cl = l;
          var a: i32 = match cv.first() { Option.Some(x) => x, Option.None => -1000 };
          var b: i32 = match cv.last() { Option.Some(x) => x, Option.None => -1000 };
          var c: i32 = match cm.find(1) { Option.Some(x) => x, Option.None => -1000 };
          var d: i32 = match cl.last() { Option.Some(x) => x, Option.None => -1000 };
          return a + b + c + d;
      }
    )");
  EXPECT_EQ(value, 5 + 7 + 30 + 100);

  // Through a constant receiver the peeked element is read-only ...
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
      using sun;
      function main() i32 {
          var allocator = make_heap_allocator();
          var v = Vec<i32>(allocator, 4);
          v.push(5);
          const ref cv = v;
          match cv.last() {
              Option.Some(x) => { x = 1; },
              Option.None => { }
          };
          return 0;
      }
    )"),
                                "Cannot assign through const reference 'x'");

  // ... while a mutable receiver still hands out a writable borrow
  auto written = executeStringWithStdlib(R"(
      using sun;
      function main() i32 {
          var allocator = make_heap_allocator();
          var v = Vec<i32>(allocator, 4);
          v.push(5);
          match v.last() {
              Option.Some(x) => { x = 42; },
              Option.None => { }
          };
          return v.get(0);
      }
    )");
  EXPECT_EQ(written, 42);
}

TEST(Classes_ConstMethods, user_const_method_returning_option_ref) {
  auto value = executeString(R"(
      enum Maybe<T> { Some(T), None }
      class Cell {
          var v: i32;
          function init(v: i32) { this.v = v; }
          const function peek() Maybe<ref i32> { return Maybe.Some(this.v); }
      }
      function main() i32 {
          const c = Cell(21);
          var w = Cell(1);
          match w.peek() { Maybe.Some(r) => { r = 21; }, Maybe.None => { } };
          return match c.peek() { Maybe.Some(r) => r, Maybe.None => -1 } + w.v;
      }
    )");
  EXPECT_EQ(value, 42);

  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      enum Maybe<T> { Some(T), None }
      class Cell {
          var v: i32;
          function init(v: i32) { this.v = v; }
          const function peek() Maybe<ref i32> { return Maybe.Some(this.v); }
      }
      function main() i32 {
          const c = Cell(21);
          match c.peek() { Maybe.Some(r) => { r = 1; }, Maybe.None => { } };
          return c.v;
      }
    )"),
                                "Cannot assign through const reference 'r'");
}

TEST(Classes_ConstMethods, const_map_reads) {
  auto value = executeStringWithStdlib(R"(
      using sun;
      function main() i32 {
          var allocator = make_heap_allocator();
          var m = Map<i64, i32>(allocator, 8);
          m.insert(1, 10);
          m.insert(2, 20);
          const ref cm = m;
          if (not cm.contains(2)) { return -1; }
          return cm.get(1) + cm.get(2) + _convert<i32>(cm.size());
      }
    )");
  EXPECT_EQ(value, 32);
}
