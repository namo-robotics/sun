// tests/test_interfaces.cpp - Tests for interface support

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic Interface Definition Tests
// ============================================================================

TEST(InterfaceTest, simple_interface_definition) {
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

TEST(InterfaceTest, interface_with_field) {
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

TEST(InterfaceTest, interface_with_default_implementation) {
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

TEST(InterfaceTest, class_implements_interface) {
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

TEST(InterfaceTest, interface_field_inherited) {
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

TEST(InterfaceTest, default_method_used) {
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

TEST(InterfaceTest, override_default_method) {
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

TEST(InterfaceTest, multiple_interfaces) {
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

TEST(InterfaceTest, multiple_interfaces_with_methods) {
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

TEST(InterfaceTest, interface_default_uses_field) {
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

TEST(InterfaceTest, class_own_plus_interface_fields) {
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
// Generic Interface Tests
// ============================================================================

TEST(GenericInterfaceTest, simple_generic_interface_definition) {
  auto value = executeString(R"(
    interface IBox<T> {
      var value: T;
    }

    function main() i32 {
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(GenericInterfaceTest, generic_interface_with_method) {
  auto value = executeString(R"(
    interface IContainer<T> {
      function get() T;
    }

    function main() i32 {
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(GenericInterfaceTest, class_implements_generic_interface) {
  auto value = executeString(R"(
    interface IBox<T> {
      var value: T;
      function get() T;
    }

    class IntBox implements IBox<i32> {
      function init(v: i32) {
        this.value = v;
      }
      
      function get() i32 {
        return this.value;
      }
    }

    function main() i32 {
        var box = IntBox(42);
        return box.get();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(GenericInterfaceTest, generic_class_implements_generic_interface) {
  auto value = executeString(R"(
    interface IContainer<T> {
      function get() T;
    }

    class Box<T> implements IContainer<T> {
      var item: T;
      
      function init(v: T) {
        this.item = v;
      }
      
      function get() T {
        return this.item;
      }
    }

    function main() i32 {
        var b = Box<i32>(100);
        return b.get();
    }
  )");
  EXPECT_EQ(value, 100);
}

TEST(GenericInterfaceTest, two_type_parameters) {
  auto value = executeString(R"(
    interface IPair<A, B> {
      function first() A;
      function second() B;
    }

    class Pair<X, Y> implements IPair<X, Y> {
      var a: X;
      var b: Y;
      
      function init(x: X, y: Y) {
        this.a = x;
        this.b = y;
      }
      
      function first() X {
        return this.a;
      }
      
      function second() Y {
        return this.b;
      }
    }

    function main() i32 {
        var p = Pair<i32, i32>(10, 20);
        return p.first() + p.second();
    }
  )");
  EXPECT_EQ(value, 30);
}

TEST(GenericInterfaceTest, multiple_instantiations) {
  auto value = executeString(R"(
    interface IWrapper<T> {
      function unwrap() T;
    }

    class IntWrapper implements IWrapper<i32> {
      var val: i32;
      
      function init(v: i32) {
        this.val = v;
      }
      
      function unwrap() i32 {
        return this.val;
      }
    }

    class BoolWrapper implements IWrapper<bool> {
      var val: bool;
      
      function init(v: bool) {
        this.val = v;
      }
      
      function unwrap() bool {
        return this.val;
      }
    }

    function main() i32 {
        var iw = IntWrapper(7);
        var bw = BoolWrapper(true);
        if (bw.unwrap()) {
            return iw.unwrap();
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(GenericInterfaceTest, interface_field_inherited) {
  auto value = executeString(R"(
    interface IValue<T> {
      var data: T;
    }

    class IntValue implements IValue<i32> {
      function init(v: i32) {
        this.data = v;
      }
      
      function get() i32 {
        return this.data;
      }
    }

    function main() i32 {
        var iv = IntValue(55);
        return iv.get();
    }
  )");
  EXPECT_EQ(value, 55);
}

// ============================================================================
// Builtin Interface Tests (IIterator<T, Container>, IIterable<T, Self>)
// ============================================================================

TEST(BuiltinInterfaceTest, implements_iiterator) {
  // Test implementing IIterator<T, Container> - uses a dummy container
  // The iterator stores all state internally, so the container ref is unused
  auto value = executeString(R"(
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
      
      function hasNext(c: ref DummyContainer) bool {
        return this.current < this.end;
      }
      
      function next(c: ref DummyContainer) i32 {
        var result = this.current;
        this.current = this.current + 1;
        return result;
      }
    }

    function main() i32 {
        var container = DummyContainer();
        var iter = RangeIterator(0, 5);
        var sum: i32 = 0;
        while (iter.hasNext(container)) {
            sum = sum + iter.next(container);
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 10);  // 0 + 1 + 2 + 3 + 4 = 10
}

TEST(BuiltinInterfaceTest, generic_implements_iiterator) {
  // Generic class implementing IIterator<T, Container>
  auto value = executeString(R"(
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
      
      function hasNext(c: ref DummyContainer) bool {
        return this.index < this.size;
      }
      
      function next(c: ref DummyContainer) T {
        var result = this.items[this.index];
        this.index = this.index + 1;
        return result;
      }
    }

    function main() i32 {
        var container = DummyContainer();
        var arr = [10, 20, 30];
        var iter = ArrayIterator<i32>(arr, 3);
        var sum: i32 = 0;
        while (iter.hasNext(container)) {
            sum = sum + iter.next(container);
        }
        return sum;
    }
  )");
  EXPECT_EQ(value, 60);  // 10 + 20 + 30 = 60
}

// IIterable test with new interface signature
TEST(BuiltinInterfaceTest, iterable_pattern_duck_typed) {
  auto value = executeString(R"(
    class SingleValue {
      var val: i32;
      
      function init(v: i32) {
        this.val = v;
      }
      
      function getValue() i32 {
        return this.val;
      }
    }

    class SimpleIterator implements IIterator<i32, SingleValue> {
      var value: i32;
      var done: bool;
      
      function init(v: i32) {
        this.value = v;
        this.done = false;
      }
      
      function hasNext(sv: ref SingleValue) bool {
        return this.done == false;
      }
      
      function next(sv: ref SingleValue) i32 {
        this.done = true;
        return this.value;
      }
    }

    function main() i32 {
        var sv = SingleValue(42);
        var it = SimpleIterator(sv.getValue());
        if (it.hasNext(sv)) {
            return it.next(sv);
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Builtin Type Redefinition Tests
// ============================================================================

TEST(BuiltinInterfaceTest, cannot_redefine_IError_interface) {
  EXPECT_ANY_THROW({
    executeString(R"(
      interface IError {
        function code() i32;
      }
      function main() i32 { return 0; }
    )");
  });
}

TEST(BuiltinInterfaceTest, cannot_redefine_IIterator_interface) {
  EXPECT_ANY_THROW({
    executeString(R"(
      interface IIterator<T> {
        function hasNext() bool;
        function next() T;
      }
      function main() i32 { return 0; }
    )");
  });
}

TEST(BuiltinInterfaceTest, cannot_redefine_IIterable_interface) {
  EXPECT_ANY_THROW({
    executeString(R"(
      interface IIterable<T> {
        function iter() i32;
      }
      function main() i32 { return 0; }
    )");
  });
}

TEST(BuiltinInterfaceTest, cannot_redefine_IError_as_class) {
  EXPECT_ANY_THROW({
    executeString(R"(
      class IError {
        var code: i32;
      }
      function main() i32 { return 0; }
    )");
  });
}

TEST(BuiltinInterfaceTest, cannot_redefine_IIterator_as_class) {
  EXPECT_ANY_THROW({
    executeString(R"(
      class IIterator {
        var value: i32;
      }
      function main() i32 { return 0; }
    )");
  });
}

// ============================================================================
// Dynamic Dispatch Tests - vtable-based interface method calls
// ============================================================================

TEST(DynamicDispatchTest, basic_interface_variable_dispatch) {
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

TEST(DynamicDispatchTest, interface_param_dispatch) {
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
    function compute_area(s: IShape) i32 {
      return s.area();
    }
    function main() i32 {
      var c = Circle(4);
      return compute_area(c);
    }
  )");
  EXPECT_EQ(value, 48);  // 4 * 4 * 3 = 48
}

TEST(DynamicDispatchTest, multiple_classes_same_interface) {
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
    function get_area(s: IShape) i32 {
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

TEST(DynamicDispatchTest, interface_with_multiple_methods) {
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
    function sum_info(c: ICounter) i32 {
      return c.value() + c.name();
    }
    function main() i32 {
      var cnt = Counter(10, 5);
      return sum_info(cnt);
    }
  )");
  EXPECT_EQ(value, 15);
}

TEST(DynamicDispatchTest, interface_with_default_method_override) {
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
    function get_greeting(g: IGreeter) i32 {
      return g.greet();
    }
    function main() i32 {
      var c = CustomGreeter(7);
      return get_greeting(c);
    }
  )");
  EXPECT_EQ(value, 107);  // 100 + 7 = 107
}

TEST(DynamicDispatchTest, interface_with_default_method_no_override) {
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
    function get_greeting(g: IGreeter) i32 {
      return g.greet();
    }
    function main() i32 {
      var d = DefaultGreeter();
      return get_greeting(d);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(DynamicDispatchTest, generic_interface_dispatch) {
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
    function unbox(b: IBox<i32>) i32 {
      return b.get();
    }
    function main() i32 {
      var box = IntBox(99);
      return unbox(box);
    }
  )");
  EXPECT_EQ(value, 99);
}

TEST(DynamicDispatchTest, generic_method_dispatch_not_supported) {
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

TEST(DynamicDispatchTest, for_in_over_vec_of_interfaces) {
  // Iterate over a Vec of interface-typed objects with for-in
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    import "stdlib/vec.sun";
    
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
      items.deinit();
      return sum;
    }
  )");
  // NumA(10).get() = 10, NumB(5).get() = 10, NumA(3).get() = 3
  EXPECT_EQ(value, 23);
}