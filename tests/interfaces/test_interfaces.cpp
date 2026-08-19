// tests/interfaces/test_interfaces.cpp - Tests for interface support

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic Interface Definition Tests
// ============================================================================

TEST(Interfaces, simple_interface_definition) {
  auto value = executeString(R"(
    interface Printable {
      function print() void;
    }

    function main() i32 {
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Interfaces, interface_with_field) {
  auto value = executeString(R"(
    interface Named {
      var name: i32;
    }

    function main() i32 {
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Interfaces, interface_with_default_implementation) {
  auto value = executeString(R"(
    interface Greeter {
      function greet() i32 {
        return 42;
      }
    }

    function main() i32 {
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Class Implements Interface Tests
// ============================================================================

TEST(Interfaces, class_implements_interface) {
  auto value = executeString(R"(
    interface Counter {
      function count() i32;
    }

    class SimpleCounter implements Counter {
      var value: i32;
      
      function init() {
        this.value = 0;
      }

      function count() i32 {
        return this.value;
      }
    }

    function main() i32 {
        var c = SimpleCounter();
        return c.count();
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Interfaces, interface_field_inherited) {
  auto value = executeString(R"(
    interface HasValue {
      var value: i32;
    }

    class ValueHolder implements HasValue {
      function init(v: i32) {
        this.value = v;
      }
      
      function get() i32 {
        return this.value;
      }
    }

    function main() i32 {
        var h = ValueHolder(42);
        return h.get();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Interfaces, default_method_used) {
  auto value = executeString(R"(
    interface Answerable {
      function answer() i32 {
        return 42;
      }
    }

    class Thinker implements Answerable {
      function init() {
      }
    }

    function main() i32 {
        var t = Thinker();
        return t.answer();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Interfaces, override_default_method) {
  auto value = executeString(R"(
    interface Answerable {
      function answer() i32 {
        return 42;
      }
    }

    class SmartThinker implements Answerable {
      function init() {
      }
      
      function answer() i32 {
        return 100;
      }
    }

    function main() i32 {
        var t = SmartThinker();
        return t.answer();
    }
  )");
  EXPECT_EQ(value, 100);
}

// ============================================================================
// Multiple Interface Implementation Tests
// ============================================================================

TEST(Interfaces, multiple_interfaces) {
  auto value = executeString(R"(
    interface HasX {
      var x: i32;
    }

    interface HasY {
      var y: i32;
    }

    class Point implements HasX, HasY {
      function init(px: i32, py: i32) {
        this.x = px;
        this.y = py;
      }
      
      function sum() i32 {
        return this.x + this.y;
      }
    }

    function main() i32 {
        var p = Point(10, 20);
        return p.sum();
    }
  )");
  EXPECT_EQ(value, 30);
}

TEST(Interfaces, multiple_interfaces_with_methods) {
  auto value = executeString(R"(
    interface Adder {
      function add(a: i32, b: i32) i32;
    }

    interface Multiplier {
      function mult(a: i32, b: i32) i32;
    }

    class Calculator implements Adder, Multiplier {
      function init() {
      }
      
      function add(a: i32, b: i32) i32 {
        return a + b;
      }
      
      function mult(a: i32, b: i32) i32 {
        return a * b;
      }
    }

    function main() i32 {
        var calc = Calculator();
        return calc.add(3, 4) + calc.mult(2, 5);
    }
  )");
  EXPECT_EQ(value, 17);  // 7 + 10
}

// ============================================================================
// Interface with Default Implementation and Fields
// ============================================================================

TEST(Interfaces, interface_default_uses_field) {
  auto value = executeString(R"(
    interface Incrementable {
      var counter: i32;
      
      function increment() i32 {
        return this.counter + 1;
      }
    }

    class MyCounter implements Incrementable {
      function init(start: i32) {
        this.counter = start;
      }
    }

    function main() i32 {
        var c = MyCounter(10);
        return c.increment();
    }
  )");
  EXPECT_EQ(value, 11);
}

// ============================================================================
// Class with Own Fields and Interface Fields
// ============================================================================

TEST(Interfaces, class_own_plus_interface_fields) {
  auto value = executeString(R"(
    interface HasId {
      var id: i32;
    }

    class Entity implements HasId {
      var name_length: i32;
      
      function init(id: i32, len: i32) {
        this.id = id;
        this.name_length = len;
      }
      
      function total() i32 {
        return this.id + this.name_length;
      }
    }

    function main() i32 {
        var e = Entity(100, 5);
        return e.total();
    }
  )");
  EXPECT_EQ(value, 105);
}

// ============================================================================
// Stdlib Iteration Interfaces (IIterator<T, Container>, IIterable<T, Self>)
// ============================================================================

TEST(Interfaces_Iterator, implements_iiterator) {
  // Implementing IIterator<T, Container> - uses a dummy container
  // The iterator stores all state internally, so the container ref is unused
  auto value = executeStringWithStdlib(R"(
    using sun;

    class DummyContainer {
      function init() {}
    }

    class RangeIterator implements IIterator<i32, DummyContainer> {
      var current: i32;
      var end: i32;

      function init(start: i32, end: i32) {
        this.current = start;
        this.end = end;
      }

      function next(c: ref DummyContainer) Option<i32> {
        if (this.current >= this.end) {
          return Option.None;
        }
        var result = this.current;
        this.current = this.current + 1;
        return Option.Some(result);
      }
    }

    function main() i32 {
        var container = DummyContainer();
        var iter = RangeIterator(0, 5);
        var sum: i32 = 0;
        var going = true;
        while (going) {
            match iter.next(container) {
                Option.Some(v) => { sum = sum + v; },
                Option.None => { going = false; }
            };
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 10);  // 0 + 1 + 2 + 3 + 4 = 10
}

TEST(Interfaces_Iterator, generic_implements_iiterator) {
  // Generic class implementing IIterator<T, Container>
  auto value = executeStringWithStdlib(R"(
    using sun;

    class DummyContainer {
      function init() {}
    }

    class ArrayIterator<T> implements IIterator<T, DummyContainer> {
      var items: array<T>;
      var index: i32;
      var size: i32;

      function init(arr: ref array<T>, sz: i32) {
        this.items = arr;
        this.index = 0;
        this.size = sz;
      }

      function next(c: ref DummyContainer) Option<T> {
        if (this.index >= this.size) {
          return Option.None;
        }
        var result = this.items[this.index];
        this.index = this.index + 1;
        return Option.Some(result);
      }
    }

    function main() i32 {
        var container = DummyContainer();
        var arr = [10, 20, 30];
        var iter = ArrayIterator<i32>(arr, 3);
        var sum: i32 = 0;
        var going = true;
        while (going) {
            match iter.next(container) {
                Option.Some(v) => { sum = sum + v; },
                Option.None => { going = false; }
            };
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 60);  // 10 + 20 + 30 = 60
}

TEST(Interfaces_Iterator, covariant_iter_is_static_only) {
  // iter() may return the concrete iterator class, but such an IIterable
  // cannot be dispatched through a fat pointer, so conversion is rejected
  EXPECT_ANY_THROW({
    executeStringWithStdlib(R"(
      using sun;

      class Range implements IIterable<i32, Range> {
          function init() {}
          function iter() RangeIterator { return RangeIterator(); }
      }

      class RangeIterator implements IIterator<i32, Range> {
          function init() {}
          function next(r: ref Range) Option<i32> { return Option.None; }
      }

      function main() i32 {
          var r = Range();
          var it: IIterable<i32, Range> = r;
          return 0;
      }
    )");
  });
}

TEST(Interfaces_Iterator, generic_class_conformance_is_checked) {
  // Generic specializations validate their interfaces like other classes
  EXPECT_ANY_THROW({
    executeStringWithStdlib(R"(
      using sun;

      class Wrong<T> implements IIterator<T, Wrong<T>> {
        function init() {}
        function next(w: ref Wrong<T>) T { return 0; }
      }

      function main() i32 {
          var w = Wrong<i32>();
          return 0;
      }
    )");
  });
}

TEST(Interfaces_Iterator, missing_next_is_error) {
  // A class claiming IIterator without next() is rejected
  EXPECT_ANY_THROW({
    executeStringWithStdlib(R"(
      using sun;

      class Broken implements IIterator<i32, Broken> {
        function init() {}
      }
      function main() i32 { return 0; }
    )");
  });
}

TEST(Interfaces_Iterator, wrong_next_signature_is_error) {
  // next() must return Option<T>
  EXPECT_ANY_THROW({
    executeStringWithStdlib(R"(
      using sun;

      class Broken implements IIterator<i32, Broken> {
        function init() {}
        function next(c: ref Broken) i32 { return 0; }
      }
      function main() i32 { return 0; }
    )");
  });
}

// ============================================================================
// Builtin Type Redefinition Tests
// ============================================================================

TEST(Interfaces_Builtin, cannot_redefine_IError_interface) {
  EXPECT_ANY_THROW({
    executeString(R"(
      interface IError {
        function code() i32;
      }
      function main() i32 { return 0; }
    )");
  });
}

TEST(Interfaces_Builtin, cannot_redefine_IError_as_class) {
  EXPECT_ANY_THROW({
    executeString(R"(
      class IError {
        var code: i32;
      }
      function main() i32 { return 0; }
    )");
  });
}

// ============================================================================
// Dynamic Dispatch Tests - vtable-based interface method calls
// ============================================================================

TEST(Interfaces_DynamicDispatch, basic_interface_variable_dispatch) {
  // Assign class to interface-typed variable and call method
  auto value = executeString(R"(
    interface IShape {
      function area() i32;
    }
    class Square implements IShape {
      var side: i32;
      function init(s: i32) {
        this.side = s;
      }
      function area() i32 {
        return this.side * this.side;
      }
    }
    function main() i32 {
      var shape: IShape = Square(5);
      return shape.area();
    }
  )");
  EXPECT_EQ(value, 25);
}

TEST(Interfaces_DynamicDispatch, interface_param_dispatch) {
  // Pass class to function taking interface parameter
  auto value = executeString(R"(
    interface IShape {
      function area() i32;
    }
    class Circle implements IShape {
      var radius: i32;
      function init(r: i32) {
        this.radius = r;
      }
      function area() i32 {
        return this.radius * this.radius * 3;
      }
    }
    function compute_area(s: ref IShape) i32 {
      return s.area();
    }
    function main() i32 {
      var c = Circle(4);
      return compute_area(c);
    }
  )");
  EXPECT_EQ(value, 48);  // 4 * 4 * 3 = 48
}

TEST(Interfaces_DynamicDispatch, multiple_classes_same_interface) {
  // Different classes implementing same interface
  auto value = executeString(R"(
    interface IShape {
      function area() i32;
    }
    class Square implements IShape {
      var side: i32;
      function init(s: i32) { this.side = s; }
      function area() i32 { return this.side * this.side; }
    }
    class Rectangle implements IShape {
      var width: i32;
      var height: i32;
      function init(w: i32, h: i32) { this.width = w; this.height = h; }
      function area() i32 { return this.width * this.height; }
    }
    function get_area(s: ref IShape) i32 {
      return s.area();
    }
    function main() i32 {
      var sq = Square(5);
      var rect = Rectangle(3, 4);
      return get_area(sq) + get_area(rect);
    }
  )");
  EXPECT_EQ(value, 37);  // 25 + 12 = 37
}

TEST(Interfaces_DynamicDispatch, interface_with_multiple_methods) {
  // Interface with multiple non-generic methods
  auto value = executeString(R"(
    interface ICounter {
      function value() i32;
      function name() i32;
    }
    class Counter implements ICounter {
      var val: i32;
      var id: i32;
      function init(v: i32, n: i32) { this.val = v; this.id = n; }
      function value() i32 { return this.val; }
      function name() i32 { return this.id; }
    }
    function sum_info(c: ref ICounter) i32 {
      return c.value() + c.name();
    }
    function main() i32 {
      var cnt = Counter(10, 5);
      return sum_info(cnt);
    }
  )");
  EXPECT_EQ(value, 15);
}

TEST(Interfaces_DynamicDispatch, interface_with_default_method_override) {
  // Class overrides default method - vtable should use class method
  auto value = executeString(R"(
    interface IGreeter {
      function greet() i32 {
        return 42;
      }
    }
    class CustomGreeter implements IGreeter {
      var bonus: i32;
      function init(b: i32) { this.bonus = b; }
      function greet() i32 {
        return 100 + this.bonus;
      }
    }
    function get_greeting(g: ref IGreeter) i32 {
      return g.greet();
    }
    function main() i32 {
      var c = CustomGreeter(7);
      return get_greeting(c);
    }
  )");
  EXPECT_EQ(value, 107);  // 100 + 7 = 107
}

TEST(Interfaces_DynamicDispatch, interface_with_default_method_no_override) {
  // Class uses default method - vtable should point to wrapper
  auto value = executeString(R"(
    interface IGreeter {
      function greet() i32 {
        return 42;
      }
    }
    class DefaultGreeter implements IGreeter {
      function init() {}
    }
    function get_greeting(g: ref IGreeter) i32 {
      return g.greet();
    }
    function main() i32 {
      var d = DefaultGreeter();
      return get_greeting(d);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Interfaces_DynamicDispatch, generic_interface_dispatch) {
  // Dynamic dispatch on a generic interface (interface has type parameter)
  auto value = executeString(R"(
    interface IBox<T> {
      function get() T;
    }
    class IntBox implements IBox<i32> {
      var val: i32;
      function init(v: i32) { this.val = v; }
      function get() i32 { return this.val; }
    }
    function unbox(b: ref IBox<i32>) i32 {
      return b.get();
    }
    function main() i32 {
      var box = IntBox(99);
      return unbox(box);
    }
  )");
  EXPECT_EQ(value, 99);
}

TEST(Interfaces_DynamicDispatch, generic_method_dispatch_not_supported) {
  // Generic methods on interfaces cannot be dynamically dispatched
  // because they require compile-time type information
  EXPECT_ANY_THROW({
    executeString(R"(
      interface IFactory {
        function create<T>() T;
      }
      class IntFactory implements IFactory {
        function init() {}
        function create<T>() T {
          return 0;
        }
      }
      function use_factory(f: IFactory) i32 {
        return f.create<i32>();
      }
      function main() i32 {
        var factory = IntFactory();
        return use_factory(factory);
      }
    )");
  });
}

TEST(Interfaces_DynamicDispatch, for_in_over_vec_of_interfaces) {
  // Iterate over a Vec of interface-typed objects with for-in
  auto value = executeStringWithStdlib(R"(
    using sun;
    
    interface IValue {
      function get() i32;
    }
    class NumA implements IValue {
      var n: i32;
      function init(v: i32) { this.n = v; }
      function get() i32 { return this.n; }
    }
    class NumB implements IValue {
      var n: i32;
      function init(v: i32) { this.n = v; }
      function get() i32 { return this.n * 2; }
    }
    function main() i32 {
      var alloc = make_heap_allocator();
      var items = Vec<IValue>(alloc, 8);
      items.push(NumA(10));
      items.push(NumB(5));
      items.push(NumA(3));
      var sum: i32 = 0;
      for (var item: IValue in items) {
        sum = sum + item.get();
      }
      return sum;
    }
  )");
  // NumA(10).get() = 10, NumB(5).get() = 10, NumA(3).get() = 3
  EXPECT_EQ(value, 23);
}