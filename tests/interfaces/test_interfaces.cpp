// tests/interfaces/test_interfaces.cpp - Tests for interface support

#include <gtest/gtest.h>
#include <llvm/IR/Constants.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/driver.h"
#include "driver/execution_utils.h"

using sun::driver::executeString;
using sun::driver::executeStringWithStdlib;

// ============================================================================
// Basic Interface Definition Tests
// ============================================================================

TEST(Interfaces, simple_interface_definition) {
  auto value = executeString(R"(
    interface Printable {
      method print() void;
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
      method greet() i32 {
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
      method count() i32;
    }

    class SimpleCounter implements Counter {
      var value: i32;
      
      init() {
        this.value = 0;
      }

      method count() i32 {
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
      init(v: i32) {
        this.value = v;
      }
      
      method get() i32 {
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
      method answer() i32 {
        return 42;
      }
    }

    class Thinker implements Answerable {
      init() {
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
      method answer() i32 {
        return 42;
      }
    }

    class SmartThinker implements Answerable {
      init() {
      }
      
      method answer() i32 {
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
      init(px: i32, py: i32) {
        this.x = px;
        this.y = py;
      }
      
      method sum() i32 {
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
      method add(a: i32, b: i32) i32;
    }

    interface Multiplier {
      method mult(a: i32, b: i32) i32;
    }

    class Calculator implements Adder, Multiplier {
      init() {
      }
      
      method add(a: i32, b: i32) i32 {
        return a + b;
      }
      
      method mult(a: i32, b: i32) i32 {
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
      
      method increment() i32 {
        return this.counter + 1;
      }
    }

    class MyCounter implements Incrementable {
      init(start: i32) {
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
      
      init(id: i32, len: i32) {
        this.id = id;
        this.name_length = len;
      }
      
      method total() i32 {
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
    using std;

    class DummyContainer {
      init() {}
    }

    class RangeIterator implements IIterator<i32, DummyContainer> {
      var current: i32;
      var end: i32;

      init(start: i32, end: i32) {
        this.current = start;
        this.end = end;
      }

      method next(c: ref DummyContainer) Option<i32> {
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
    using std;

    class DummyContainer {
      init() {}
    }

    // The iterator views the caller's array through a `ref array<T>` field,
    // which makes it a reference holder bound to that array's frame.
    class ArrayIterator<T> implements IIterator<T, DummyContainer> {
      var items: ref array<T>;
      var index: i32;
      var size: i32;

      init(arr: ref array<T>, sz: i32) {
        this.items = arr;
        this.index = 0;
        this.size = sz;
      }

      method next(c: ref DummyContainer) Option<T> {
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
      using std;

      class Range implements IIterable<i32, Range> {
          init() {}
          method iter() RangeIterator { return RangeIterator(); }
      }

      class RangeIterator implements IIterator<i32, Range> {
          init() {}
          method next(r: ref Range) Option<i32> { return Option.None; }
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
      using std;

      class Wrong<T> implements IIterator<T, Wrong<T>> {
        init() {}
        method next(w: ref Wrong<T>) T { return 0; }
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
      using std;

      class Broken implements IIterator<i32, Broken> {
        init() {}
      }
      function main() i32 { return 0; }
    )");
  });
}

TEST(Interfaces_Iterator, wrong_next_signature_is_error) {
  // next() must return Option<T>
  EXPECT_ANY_THROW({
    executeStringWithStdlib(R"(
      using std;

      class Broken implements IIterator<i32, Broken> {
        init() {}
        method next(c: ref Broken) i32 { return 0; }
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
        method code() i32;
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
      method area() i32;
    }
    class Square implements IShape {
      var side: i32;
      init(s: i32) {
        this.side = s;
      }
      method area() i32 {
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

TEST(Interfaces_DynamicDispatch, interface_typed_field_dispatch) {
  auto value = executeString(R"(
    interface IClickHandler {
      public method onClick(id: i32) i32;
    }

    class Counter implements IClickHandler {
      var total: i32;
      init() { this.total = 0; }
      public method onClick(id: i32) i32 {
        this.total = this.total + id;
        return this.total;
      }
    }

    class Button {
      var handler: IClickHandler;
      init() { this.handler = Counter(); }
      public method click(id: i32) i32 {
        return this.handler.onClick(id);
      }
    }

    function main() i32 {
      var b = Button();
      b.click(20);
      return b.click(22);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Interfaces_DynamicDispatch, interface_field_drops_erased_owners) {
  auto value = executeString(R"(
    var dropped: i32 = 0;

    interface IClickHandler {
      method onClick(id: i32) i32;
    }

    class TrackedHandler implements IClickHandler {
      var weight: i32;
      init(weight: i32) { this.weight = weight; }
      method onClick(id: i32) i32 { return id; }
      deinit() { dropped = dropped + this.weight; }
    }

    class Button {
      var handler: IClickHandler;
      init() { this.handler = TrackedHandler(1); }
      method replace(next: IClickHandler) void {
        this.handler = next;
      }
    }

    function exercise() void {
      var button = Button();
      button.replace(TrackedHandler(10));
    }

    function main() i32 {
      exercise();
      return dropped;
    }
  )");
  EXPECT_EQ(value, 11);
}

TEST(Interfaces_DynamicDispatch, interface_param_dispatch) {
  // Pass class to function taking interface parameter
  auto value = executeString(R"(
    interface IShape {
      method area() i32;
    }
    class Circle implements IShape {
      var radius: i32;
      init(r: i32) {
        this.radius = r;
      }
      method area() i32 {
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
      method area() i32;
    }
    class Square implements IShape {
      var side: i32;
      init(s: i32) { this.side = s; }
      method area() i32 { return this.side * this.side; }
    }
    class Rectangle implements IShape {
      var width: i32;
      var height: i32;
      init(w: i32, h: i32) { this.width = w; this.height = h; }
      method area() i32 { return this.width * this.height; }
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
      method value() i32;
      method name() i32;
    }
    class Counter implements ICounter {
      var val: i32;
      var id: i32;
      init(v: i32, n: i32) { this.val = v; this.id = n; }
      method value() i32 { return this.val; }
      method name() i32 { return this.id; }
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
      method greet() i32 {
        return 42;
      }
    }
    class CustomGreeter implements IGreeter {
      var bonus: i32;
      init(b: i32) { this.bonus = b; }
      method greet() i32 {
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
      method greet() i32 {
        return 42;
      }
    }
    class DefaultGreeter implements IGreeter {
      init() {}
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
      method get() T;
    }
    class IntBox implements IBox<i32> {
      var val: i32;
      init(v: i32) { this.val = v; }
      method get() i32 { return this.val; }
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
        method create<T>() T;
      }
      class IntFactory implements IFactory {
        init() {}
        method create<T>() T {
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
    using std;
    
    interface IValue {
      method get() i32;
    }
    class NumA implements IValue {
      var n: i32;
      init(v: i32) { this.n = v; }
      method get() i32 { return this.n; }
    }
    class NumB implements IValue {
      var n: i32;
      init(v: i32) { this.n = v; }
      method get() i32 { return this.n * 2; }
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
TEST(Interfaces_DynamicDispatch, extra_argument_to_interface_method_is_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface IShape { method area() i32; }
    class Square implements IShape {
        var s: i32;
        init(s: i32) { this.s = s; }
        method area() i32 { return this.s * this.s; }
    }
    function main() i32 {
        var q = Square(2);
        var shape: IShape = q;
        return shape.area(7);
    }
  )"),
                                "'area' expects 0 arguments, got 1");
}

// ============================================================================
// Duplicate interface fields
// ============================================================================

TEST(Interfaces, duplicate_field_in_interface_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
    interface IShape {
      var sides: i32;
      var sides: i32;
      method area() i32;
    }
    function main() i32 { return 0; }
  )"),
      "Field 'sides' already exists in interface 'IShape'");
}

// Generic interfaces are templates, so the duplicate is caught on the
// declaration rather than on whatever instantiates it.
TEST(Interfaces, duplicate_field_in_generic_interface_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
    interface IHolder<T> {
      var value: T;
      var value: T;
      method get() T;
    }
    function main() i32 { return 0; }
  )"),
      "Field 'value' already exists in interface 'IHolder'");
}

// ============================================================================
// Borrowed class arguments to interface parameters: a borrow reaches an
// interface only as `ref Interface`. An interface value owns what it points
// at, so a borrow cannot become one.
// ============================================================================

TEST(Interfaces, borrowed_class_passes_to_ref_interface_parameter) {
  auto value = executeString(R"(
    interface IShape { method area() i32; }
    class Sq implements IShape {
      var s: i32;
      init(s: i32) { this.s = s; }
      method area() i32 { return this.s * this.s; }
    }
    function measure(sh: IShape) i32 { return sh.area(); }
    function measure_ref(sh: ref IShape) i32 { return sh.area(); }
    function via_borrow(q: ref Sq) i32 { return measure_ref(q); }
    function main() i32 {
        var q = Sq(3);
        return via_borrow(q) + measure_ref(q) + measure(q);   // 9 + 9 + 9
    }
  )");
  EXPECT_EQ(value, 27);
}

TEST(Interfaces, borrowed_class_does_not_pass_to_by_value_interface) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface IShape { method area() i32; }
    class Sq implements IShape {
      var s: i32;
      init(s: i32) { this.s = s; }
      method area() i32 { return this.s * this.s; }
    }
    function measure(sh: IShape) i32 { return sh.area(); }
    function via_borrow(q: ref Sq) i32 { return measure(q); }
    function main() i32 {
        var q = Sq(3);
        return via_borrow(q);
    }
  )"),
                                "No matching overload of 'measure' for "
                                "argument types (ref Sq)");
}

TEST(Interfaces, borrowed_class_does_not_assign_to_interface_variable) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface IShape { method area() i32; }
    class Sq implements IShape {
      var s: i32;
      init(s: i32) { this.s = s; }
      method area() i32 { return this.s * this.s; }
    }
    function via_borrow(q: ref Sq) i32 {
        var sh: IShape = q;
        return sh.area();
    }
    function main() i32 {
        var q = Sq(3);
        return via_borrow(q);
    }
  )"),
                                "Cannot assign value of type");
}

// ============================================================================
// Class arguments to interface-typed constructor and overload parameters
// (issue #219): overload selection accepts the same class-to-interface
// conversion a single known signature does.
// ============================================================================

/** Keeps test fixtures and helpers local to this source file. */
namespace {

// A handler interface with one implementation, shared by the tests below.
constexpr const char* kHandler = R"(
    interface IHandler { method handle() i32; }
    class Bump implements IHandler {
        var n: i32;
        init(n: i32) { this.n = n; }
        method handle() i32 { return this.n; }
    }
)";

}  // namespace

TEST(Interfaces, class_passes_to_interface_constructor_parameter) {
  auto value = executeString(std::string(kHandler) + R"(
    class W {
        var h: IHandler;
        init(h: IHandler) { this.h = h; }
        method run() i32 { return this.h.handle(); }
    }
    function main() i32 {
        var w = W(Bump(7));
        var b = Bump(5);
        var v = W(b);   // b moves into the interface value
        return w.run() + v.run();
    }
  )");
  EXPECT_EQ(value, 12);
}

// The constructor would store the borrow in an owning field, so a borrowed
// class does not select an interface-typed constructor parameter.
TEST(Interfaces, borrowed_class_does_not_match_interface_constructor) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(std::string(kHandler) + R"(
    class W {
        var h: IHandler;
        init(h: IHandler) { this.h = h; }
    }
    function wrap(b: ref Bump) i32 { var w = W(b); return 0; }
    function main() i32 {
        var b = Bump(5);
        return wrap(b);
    }
  )"),
                                "No matching constructor for 'W'");
}

TEST(Interfaces, class_passes_to_ref_interface_constructor_parameter) {
  auto value = executeString(std::string(kHandler) + R"(
    class Reader {
        var seen: i32;
        init(h: ref IHandler) { this.seen = h.handle(); }
    }
    function main() i32 {
        var b = Bump(9);
        var r = Reader(b);
        return r.seen;
    }
  )");
  EXPECT_EQ(value, 9);
}

TEST(Interfaces, class_selects_interface_method_overload) {
  auto value = executeString(std::string(kHandler) + R"(
    class W {
        var h: IHandler;
        init() { this.h = Bump(0); }
        // The i32 overload comes first so a name-only fallback would pick it
        method set(n: i32) void { this.h = Bump(n + 100); }
        method set(h: IHandler) void { this.h = h; }
        method run() i32 { return this.h.handle(); }
    }
    function main() i32 {
        var w = W();
        w.set(Bump(5));
        return w.run();
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Interfaces, exact_class_overload_beats_interface_overload) {
  auto value = executeString(std::string(kHandler) + R"(
    class W {
        var tag: i32;
        init(h: IHandler) { this.tag = 1; }
        init(b: Bump) { this.tag = 2; }
    }
    function main() i32 {
        var w = W(Bump(0));
        return w.tag;
    }
  )");
  EXPECT_EQ(value, 2);
}

// The frame-carrying ban carries over: a class that can hold a '<'_>'
// lambda still does not become an interface value through a constructor.
TEST(Interfaces, frame_carrying_class_does_not_match_interface_constructor) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface ICallable { method call() i32; }
    class Holder {
        var f: <'_>() => i32;
        init(f: <'this>() => i32) { this.f = f; }
        public method call() i32 { var g = this.f; return g(); }
    }
    class W {
        var c: ICallable;
        init(c: ICallable) { this.c = c; }
    }
    function main() i32 {
        var x = 3;
        var h = Holder([ref x]() => i32 { return x; });
        var w = W(h);
        return 0;
    }
  )"),
                                "No matching constructor for 'W'");
}

// A pack forwarded through _params_of<C> selects the interface overload too.
TEST(Interfaces, class_fills_interface_parameter_through_params_of) {
  auto value = executeString(std::string(kHandler) + R"(
    class W {
        var h: IHandler;
        init(h: IHandler) { this.h = h; }
        method run() i32 { return this.h.handle(); }
    }
    function make<T>(args...: _params_of<T>) raw_ptr<T> {
        var size: i64 = _sizeof<T>();
        var memory: raw_ptr<i8> = unsafe { _malloc(size); };
        unsafe { _init<T>(memory, args...); };
        return memory;
    }
    function main() i32 {
        var w = make<W>(Bump(4));
        return unsafe { w.run(); };
    }
  )");
  EXPECT_EQ(value, 4);
}

TEST(Interfaces, qualified_names_in_implements_and_constraints) {
  EXPECT_EQ(executeString(R"(
    /** Defines interfaces that must be selected by their module path. */
    public module contracts {
      /** Supplies a value. */
      public interface IValue {
        /** Returns the supplied value. */
        public method value() i32;
      }
      /** Groups generic interfaces in a nested module. */
      public module nested {
        /** Supplies a value of the requested type. */
        public interface IBox<T> {
          /** Returns the contained value. */
          public method get() T;
        }
      }
    }
    interface IValue { method other() i32; }
    class Value implements contracts.IValue, contracts.nested.IBox<i32> {
      init() {}
      /** Returns the supplied value. */
      public method value() i32 { return 21; }
      /** Returns the contained value. */
      public method get() i32 { return 21; }
    }
    class Box<T> implements contracts.nested.IBox<T> {
      var item: T;
      init(item: T) { this.item = item; }
      /** Returns the contained value. */
      public method get() T { return this.item; }
    }
    class Wrapper<T: contracts.IValue> {
      init() {}
      /** Reads through the interface constraint. */
      public method read(item: ref T) i32 { return item.value(); }
    }
    function read<T: contracts.IValue>(item: ref T) i32 {
      return item.value();
    }
    function main() i32 {
      var item = Value();
      var wrapper = Wrapper<Value>();
      var box = Box<i32>(item.get());
      return read(item) + wrapper.read(item) + box.get();
    }
  )"),
            63);
}

TEST(Interfaces, qualified_constraint_rejects_same_named_interface) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
    /** Defines the required interface. */
    public module contracts {
      /** Marks accepted values. */
      public interface IValue {}
    }
    interface IValue {}
    class Wrong implements IValue { init() {} }
    class Wrapper<T: contracts.IValue> { init() {} }
    function main() i32 { var wrapper = Wrapper<Wrong>(); return 0; }
  )"),
      "does not satisfy constraint 'contracts.IValue'");
}

TEST(Interfaces, qualified_constraints_use_definition_scope) {
  EXPECT_EQ(executeString(R"(
    /** Owns the constrained declarations. */
    public module api {
      /** Groups the required interface. */
      public module contracts {
        /** Marks accepted values. */
        public interface IValue {}
      }
      /** Implements the interface in this module. */
      public class Value implements contracts.IValue { init() {} }
      /** Accepts values implementing the local interface. */
      public function accept<T: contracts.IValue>() i32 { return 1; }
      /** Exposes a constrained generic method. */
      public class Factory {
        init() {}
        /** Accepts values implementing the local interface. */
        public method accept<T: contracts.IValue>() i32 { return 2; }
      }
      /** Constrains the element type of an interface. */
      public interface IBox<T: contracts.IValue> {}
      /** Constrains the element type of an enum. */
      public enum Choice<T: contracts.IValue> { Empty }
    }
    /** Provides a conflicting module at the call site. */
    public module contracts {
      /** Has the same short name but a different identity. */
      public interface IValue {}
    }
    class Box implements api.IBox<api.Value> { init() {} }
    function main() i32 {
      var factory = api.Factory();
      var box = Box();
      var choice: api.Choice<api.Value> = api.Choice.Empty;
      return api.accept<api.Value>() + factory.accept<api.Value>();
    }
  )"),
            3);
}

/** Checks forward parents, inherited requirements, and borrowed ancestor
 * dispatch. */
TEST(Interfaces, inherited_requirements_and_forward_parent) {
  EXPECT_EQ(executeString(R"(
    /** Adds one requirement. */
    interface Child extends Parent { /** Child value. */ method extra() i32; }
    /** Defines a base requirement. */
    interface Parent { /** Base value. */ method value() i32; }
    /** Implements both contracts. */
    class Item implements Child {
      /** Creates an item. */ init() {}
      /** Base value. */ method value() i32 { return 40; }
      /** Child value. */ method extra() i32 { return 2; }
    }
    /** Reads a parent borrow. */
    function read(x: ref Parent) i32 { return x.value(); }
    /** Exercises borrowed conversion. */
    function main() i32 { var x: Child = Item(); return read(x) + x.extra(); }
  )"),
            42);
}

/** Checks that ancestor dispatch uses the nearest default implementation. */
TEST(Interfaces, nearest_default_through_ancestor) {
  EXPECT_EQ(executeString(R"(
    /** Provides an initial default. */
    interface Base { /** Computes a value. */ method value() i32 { return 1; } }
    /** Replaces the default. */
    interface Middle extends Base { /** Computes a value. */ method value() i32 { return 42; } }
    /** Inherits the replacement. */ interface Leaf extends Middle {}
    /** Lists a redundant ancestor before its descendant. */
    class Item implements Base, Leaf { /** Creates an item. */ init() {} }
    /** Dispatches through the ancestor. */ function read(x: ref Base) i32 { return x.value(); }
    /** Exercises transitive conformance. */ function main() i32 { var x = Item(); return read(x); }
  )"),
            42);
}

/** Checks generic parent substitution and class conformance to ancestors. */
TEST(Interfaces, generic_parent_and_ancestor_constraint) {
  EXPECT_EQ(executeString(R"(
    /** Requires a typed value. */ interface Parent<T> { /** Reads the value. */ method value() T; }
    /** Retains the parent parameter. */ interface Child<T> extends Parent<T> {}
    /** Supplies an integer implementation. */
    class Item implements Child<i32> {
      /** Creates an item. */ init() {}
      /** Reads the value. */ method value() i32 { return 42; }
    }
    /** Accepts any implementation of the ancestor. */
    function read<T: Parent<i32>>(x: ref T) i32 { return x.value(); }
    /** Exercises the generic constraint. */ function main() i32 { var x = Item(); return read<Item>(x); }
  )"),
            42);
}

/** Checks owned conversions through initialization, assignment, calls, and
 * returns. */
TEST(Interfaces, owned_ancestor_conversion_paths) {
  EXPECT_EQ(executeString(R"(
    /** Requires a value. */ interface Base { /** Reads the value. */ method value() i32; }
    /** Adds a separate dispatch slot. */ interface Child extends Base { /** Reads another value. */ method extra() i32; }
    /** Supplies both methods. */
    class Item implements Child {
      /** Creates an item. */ init() {}
      /** Reads the value. */ method value() i32 { return 42; }
      /** Reads another value. */ method extra() i32 { return 7; }
    }
    /** Returns an owning ancestor. */ function convert(x: Child) Base { return x; }
    /** Consumes a parent. */ function read(x: Base) i32 { return x.value(); }
    /** Exercises transfer paths. */ function main() i32 {
      var a: Child = Item(); var b: Base = a;
      var c: Child = Item(); b = c;
      var d: Child = Item(); var e = convert(d);
      return read(e);
    }
  )"),
            42);
}

/** Checks that returned parent views live in the caller's frame. */
TEST(Interfaces, borrowed_ancestor_return) {
  EXPECT_EQ(executeString(R"(
    /** Requires a value. */ interface Base { /** Reads the value. */ const method value() i32; }
    /** Inherits the requirement. */ interface Child extends Base {}
    /** Implements the requirement. */ class Item implements Child {
      /** Creates an item. */ init() {}
      /** Reads the value. */ const method value() i32 { return 42; }
    }
    /** Forwards an ancestor borrow. */
    function parent(x: const ref Child) const ref Base { return x; }
    /** Keeps the returned view alive during dispatch. */
    function main() i32 { var x: Child = Item(); var p = parent(x); return p.value(); }
  )"),
            42);
}

/** Rejects cyclic inheritance before code generation. */
TEST(Interfaces, inheritance_cycle_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Forms a cycle. */ interface A extends B {}
    /** Closes the cycle. */ interface B extends A {}
    /** Entry point. */ function main() i32 { return 0; }
  )"),
                                "inheritance cycle");
}

/** Rejects a child field that would duplicate inherited storage. */
TEST(Interfaces, inherited_field_collision_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Owns the field contract. */ interface A { var value: i32; }
    /** Illegally repeats the field. */ interface B extends A { var value: i32; }
    /** Entry point. */ function main() i32 { return 0; }
  )"),
                                "redeclares an inherited field");
}

/** Rejects child methods that change inherited return types. */
TEST(Interfaces, inherited_contract_change_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Defines the contract. */ interface A { /** Reads a value. */ method value() i32; }
    /** Changes the contract. */ interface B extends A { /** Reads a value. */ method value() i64; }
    /** Entry point. */ function main() i32 { return 0; }
  )"),
                                "changes its inherited contract");
}

/** A bodyless child override removes an inherited default. */
TEST(Interfaces, child_can_require_implementation_again) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Supplies a default. */ interface A { /** Reads a value. */ method value() i32 { return 1; } }
    /** Requires a class implementation. */ interface B extends A { /** Reads a value. */ method value() i32; }
    /** Omits the required implementation. */ class Item implements B { /** Creates an item. */ init() {} }
    /** Entry point. */ function main() i32 { var x = Item(); return 0; }
  )"),
                                "does not implement required method");
}

/** Checks inherited defaults against the concrete implementation. */
TEST(Interfaces, default_uses_concrete_field_layout) {
  EXPECT_EQ(executeString(R"(
/** Has a field. */ interface Base { var value: i32; /** Reads the field. */ method read() i32 { return this.value; } }
/** Adds a field. */ interface Child extends Base { var extra: i32; }
/** Implements the fields. */ class Item implements Child { var own: i32; /** Initializes the fields. */ init() { this.own = 1; this.value = 42; this.extra = 3; } }
/** Exercises a default. */ function main() i32 { var x = Item(); return x.read(); }
  )"),
            42);
}

/** Checks inherited defaults against the concrete implementation. */
TEST(Interfaces, default_calls_inherited_requirement) {
  EXPECT_EQ(executeString(R"(
/** Requires a method. */ interface Base { /** Reads a value. */ method value() i32; }
/** Adds a default using the inherited requirement. */ interface Child extends Base { /** Uses the value. */ method read() i32 { return this.value(); } }
/** Implements the requirement. */ class Item implements Child { /** Initializes an item. */ init() {} /** Reads a value. */ method value() i32 { return 42; } }
/** Exercises a default. */ function main() i32 { var x = Item(); return x.read(); }
  )"),
            42);
}

/** Checks inherited defaults against the concrete implementation. */
TEST(Interfaces, generic_inherited_default) {
  EXPECT_EQ(executeString(R"(
/** Supplies a typed default. */ interface Parent<T> { /** Reads a value. */ method value() i32 { return 42; } }
/** Inherits the default. */ interface Child<T> extends Parent<T> {}
/** Uses the inherited default. */ class Item implements Child<i32> { /** Constructs an item. */ init() {} }
/** Exercises the default. */ function main() i32 { var x: Child<i32> = Item(); return x.value(); }
  )"),
            42);
}

/** Checks that an owned upcast destroys its concrete object exactly once. */
TEST(Interfaces, ancestor_conversion_drops_once) {
  EXPECT_EQ(executeString(R"(
    var dropped: i32 = 0;
    /** Defines the base contract. */ interface Base { /** Reads a value. */ method value() i32; }
    /** Adds another dispatch slot. */ interface Child extends Base { /** Reads another value. */ method extra() i32; }
    /** Records destruction. */ class Item implements Child {
      /** Creates the item. */ init() {}
      /** Records destruction. */ deinit() { dropped = dropped + 1; }
      /** Reads the value. */ method value() i32 { return 42; }
      /** Reads another value. */ method extra() i32 { return 3; }
    }
    /** Borrows through the ancestor. */ function read(x: ref Base) i32 { return x.value(); }
    /** Transfers ownership through the ancestor. */ function exercise() void {
      var child: Child = Item(); var ignored = read(child); var parent: Base = child;
    }
    /** Checks destruction after the owning scope ends. */ function main() i32 { exercise(); return dropped; }
  )"),
            1);
}

/** Rejects multiple parents rather than silently accepting the first. */
TEST(Interfaces, multiple_parents_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** First parent. */ interface A {}
    /** Second parent. */ interface B {}
    /** Invalid child. */ interface C extends A, B {}
    /** Entry point. */ function main() i32 { return 0; }
  )"),
                                "only one parent");
}

/** Rejects cycles that change their generic arguments at each edge. */
TEST(Interfaces, expanding_generic_cycle_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Wraps a parameter. */ class Box<T> { /** Creates an empty box. */ init() {} }
    /** Forms an expanding cycle. */ interface A<T> extends B<Box<T>> {}
    /** Closes the cycle. */ interface B<T> extends A<T> {}
    /** Entry point. */ function main() i32 { return 0; }
  )"),
                                "inheritance cycle");
}

/** Rejects ambiguous defaults even when the signatures match. */
TEST(Interfaces, unrelated_defaults_require_explicit_implementation) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** First default. */ interface A { /** Reads a value. */ method value() i32 { return 1; } }
    /** Second default. */ interface B { /** Reads a value. */ method value() i32 { return 2; } }
    /** Leaves the choice ambiguous. */ class Item implements A, B { /** Creates an item. */ init() {} }
    /** Entry point. */ function main() i32 { return 0; }
  )"),
                                "Conflicting interface defaults");
}

/** Does not make a parent's private method visible through a public child. */
TEST(Interfaces, inherited_private_member_keeps_defining_module) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    public module library {
      /** Contains a module-private requirement. */ public interface Base { /** Reads a secret. */ method secret() i32; }
    }
    /** Inherits without exposing the private member. */ interface Child extends library.Base {}
    /** Cannot call the private member. */ function read(x: ref Child) i32 { return x.secret(); }
    /** Entry point. */ function main() i32 { return 0; }
  )"),
                                "private");
}

/** Rebinds the parent's named lifetime before checking the child override. */
TEST(Interfaces, parent_lifetime_arguments_are_substituted) {
  EXPECT_EQ(executeString(R"(
    /** Relates a callback to its declared lifetime. */ interface Base<'a> {
      /** Accepts a related callback. */ method accept(cb: <'a>() => i32) void;
    }
    /** Renames the inherited lifetime. */ interface Child<'b> extends Base<'b> {
      /** Keeps the substituted relationship. */ method accept(cb: <'b>() => i32) void;
    }
    /** Entry point. */ function main() i32 { return 0; }
  )"),
            0);
}

/** Rejects an override that erases an inherited lifetime relationship. */
TEST(Interfaces, parent_lifetime_contract_cannot_be_erased) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Relates a callback to the receiver. */ interface Base {
      /** Accepts a related callback. */ method accept(cb: <'this>() => i32) void;
    }
    /** Removes the relationship. */ interface Child extends Base {
      /** Accepts an unrelated callback. */ method accept(cb: <'_>() => i32) void;
    }
    /** Entry point. */ function main() i32 { return 0; }
  )"),
                                "changes its inherited contract");
}

/** Generic method binders match by position across an override. */
TEST(Interfaces, inherited_generic_method_override) {
  EXPECT_EQ(executeString(R"(
    /** Supplies an identity operation. */ interface Base {
      /** Moves the supplied value back to its caller. */ method identity<T>(x: T) T;
    }
    /** Renames the method's generic binder. */ interface Child extends Base {
      /** Moves the supplied value back to its caller. */ method identity<U>(x: U) U;
    }
    /** Implements the generic contract. */ class Item implements Child {
      /** Constructs an item. */ init() {}
      /** Moves the supplied value back to its caller. */ method identity<V>(x: V) V { return x; }
    }
    /** Exercises static generic dispatch. */ function main() i32 { var x = Item(); return x.identity<i32>(42); }
  )"),
            42);
}

/** An owned upcast consumes the child handle. */
TEST(Interfaces, owning_upcast_rejects_use_after_move) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Base contract. */ interface Base { /** Reads a value. */ method value() i32; }
    /** Child contract. */ interface Child extends Base {}
    /** Concrete implementation. */ class Item implements Child {
      /** Constructs an item. */ init() {}
      /** Reads a value. */ method value() i32 { return 42; }
    }
    /** Uses the source after moving it. */ function main() i32 {
      var child: Child = Item(); var parent: Base = child; return child.value();
    }
  )"),
                                "moved");
}

/** Parent borrowing must not discard the source's constness. */
TEST(Interfaces, upcast_cannot_remove_const) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Base contract. */ interface Base { /** Mutates the value. */ method change() void; }
    /** Child contract. */ interface Child extends Base {}
    /** Requires mutable access. */ function change(x: ref Base) void { x.change(); }
    /** Attempts to discard constness. */ function bad(x: const ref Child) void { change(x); }
    /** Entry point. */ function main() i32 { return 0; }
  )"),
                                "");
}

/** Inherits a default with its own generic method parameters. */
TEST(Interfaces, inherited_generic_method_default) {
  EXPECT_EQ(executeString(R"(
/** Provides a generic default. */ interface Base { /** Returns the given value. */ method identity<T>(x:T) T { return x; } }
/** Inherits the generic default. */ interface Child extends Base {}
/** Uses the default. */ class Item implements Child { /** Constructs an item. */ init() {} }
/** Exercises static dispatch. */ function main() i32 { var x = Item(); return x.identity<i32>(42); }
  )"),
            42);
}

/** A parent view cannot outlive a locally owned child. */
TEST(Interfaces, borrowed_parent_cannot_escape_local_owner) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    /** Base contract. */ interface Base { /** Reads a value. */ method value() i32; }
    /** Child contract. */ interface Child extends Base {}
    /** Concrete implementation. */ class Item implements Child {
      /** Constructs an item. */ init() {}
      /** Reads a value. */ method value() i32 { return 42; }
    }
    /** Attempts to return a borrow of a local owner. */ function bad() ref Base {
      var child: Child = Item(); return child;
    }
    /** Entry point. */ function main() i32 { return 0; }
  )"),
                                "");
}

/** Borrowed views grant access to methods, not replacement of the erased owner.
 */
TEST(Interfaces, borrowed_view_cannot_replace_owner) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(R"(
    /** Base contract. */ interface Base { /** Reads a value. */ method value() i32; }
    /** Child contract. */ interface Child extends Base {}
    /** Attempts to replace a borrowed view's owner. */
    function replace(view: ref Base, next: Base) void { view = next; }
    /** Entry point. */ function main() i32 { return 0; }
  )"),
      "Cannot replace an interface owner through a borrowed view");
}

/** Private parent members stay private when referenced from a child's default.
 */
TEST(Interfaces, child_default_cannot_access_private_parent_method) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    public module library {
      /** Keeps the method module-private. */ public interface Base { /** Reads a secret. */ method secret() i32; }
    }
    /** Attempts to expose a private inherited method. */ interface Child extends library.Base {
      /** Reads the private method. */ method read() i32 { return this.secret(); }
    }
    /** Entry point. */ function main() i32 { return 0; }
  )"),
                                "private");
}

/** A child may add overloads while preserving the parent's method. */
TEST(Interfaces, inherited_overloads_dispatch_by_argument_type) {
  EXPECT_EQ(executeString(R"(
    /** Requires one overload. */ interface Base { /** Handles an integer. */ method read(x: i32) i32; }
    /** Adds another overload. */ interface Child extends Base { /** Handles a wider integer. */ method read(x: i64) i32; }
    /** Implements both overloads. */ class Item implements Child {
      /** Constructs an item. */ init() {}
      /** Handles an integer. */ method read(x: i32) i32 { return 40; }
      /** Handles a wider integer. */ method read(x: i64) i32 { return 2; }
    }
    /** Exercises both table slots. */ function main() i32 { var x: Child = Item(); return x.read(0) + x.read(0i64); }
  )"),
            42);
}

/** Descendants of the builtin error interface remain throwable and catchable.
 */
TEST(Interfaces, extends_builtin_error_interface) {
  EXPECT_EQ(executeString(R"(
    /** Adds an error category. */ interface Failure extends IError {}
    /** Supplies the inherited error contract. */ class MyError implements Failure {
      /** Constructs the error. */ init() {}
      /** Returns its error code. */ method code() i32 { return 42; }
      /** Describes the error. */ method message() static_ptr<u8> { return "failure"; }
    }
    /** Throws the descendant implementation. */ function fail() void throws IError { throw MyError(); }
    /** Catches through the ancestor. */ function main() i32 {
      try { fail(); } catch (error: IError) { return error.code(); }
      return 0;
    }
  )"),
            42);
}

/** Matching builtin constraints survive generic binder renaming. */
TEST(Interfaces, matching_generic_override_constraints) {
  EXPECT_EQ(executeString(R"(
    /** Restricts the method argument. */ interface Base { /** Accepts numeric values. */ method read<T: _Numeric>(x: T) i32; }
    /** Preserves the constraint. */ interface Child extends Base { /** Accepts numeric values. */ method read<U: _Numeric>(x: U) i32; }
    /** Entry point. */ function main() i32 { return 0; }
  )"),
            0);
}

/** Concrete implementations can replace an owning ancestor interface value. */
TEST(Interfaces, concrete_assignment_to_ancestor) {
  EXPECT_EQ(executeString(R"(
    /** Requires a readable value. */ interface Base { /** Reads the value. */ method read() i32; }
    /** Inherits the contract. */ interface Child extends Base {}
    /** Stores a concrete value. */ class Item implements Child {
      var value: i32;
      /** Initializes the value. */ init(value: i32) { this.value = value; }
      /** Reads the value. */ method read() i32 { return this.value; }
    }
    /** Replaces the owned implementation. */ function main() i32 {
      var value: Base = Item(1); value = Item(42); return value.read();
    }
  )"),
            42);
}

/** Borrowing concrete and erased objects must leave destruction to their owners. */
TEST(Interfaces, shared_table_borrows_preserve_ownership) {
  EXPECT_EQ(executeString(R"(
    var dropped: i32 = 0;
    /** Provides a read-only operation. */
    interface Base {
      /** Reads the value. */
      const method value() i32;
    }
    /** Adds an intermediate ancestor. */
    interface Middle extends Base {}
    /** Provides a distinct child view. */
    interface Child extends Middle {}
    /** Records destruction of each concrete object. */
    class Item implements Child {
      /** Creates an item. */
      init() {}
      /** Records destruction. */
      deinit() { dropped = dropped + 1; }
      /** Returns the stored result. */
      const method value() i32 { return 7; }
    }
    /** Returns a borrowed ancestor without taking ownership. */
    function parent(x: ref Child) ref Base { return x; }
    /** Reads through an immutable ancestor view. */
    function read(x: const ref Base) i32 { return x.value(); }
    /** Exercises stack borrows, erased borrows, and an owning conversion. */
    function exercise() i32 {
      var item = Item();
      var result = read(item);
      var child: Child = Item();
      result = result + read(parent(child));
      result = result + read(child);
      var owner: Base = child;
      result = result + read(owner);
      return result + dropped * 100;
    }
    /** Checks that neither borrowing nor upcasting destroys an object early. */
    function main() i32 {
      var result = exercise();
      return result + dropped;
    }
  )"), 30);
}

/** Both ownership modes emit one table per class/interface pair. */
TEST(Interfaces, shared_dispatch_table_layout) {
  sun::driver::initTestEnvironment();
  auto driver = sun::driver::Driver::createForAOT(
      "shared_interface_tables", "", false, false);
  driver->compileString(R"(
    /** Supplies the base operation. */
    interface Base {
      /** Reads a value. */
      method value() i32;
    }
    /** Supplies an additional operation. */
    interface Child extends Base {
      /** Reads an additional value. */
      method extra() i32;
    }
    /** Implements both operations. */
    class Item implements Child {
      /** Creates an item. */
      init() {}
      /** Reads the base value. */
      method value() i32 { return 7; }
      /** Reads the additional value. */
      method extra() i32 { return 3; }
    }
    /** Borrows a child and converts it to the parent. */
    function read(x: ref Child) i32 {
      var parent: ref Base = x;
      return parent.value() + x.extra();
    }
    /** Exercises borrowed and owned views of the same concrete class. */
    function main() i32 {
      var item = Item();
      var result = read(item);
      var owner: Child = item;
      return result + read(owner);
    }
  )");
  unsigned tableCount = 0;
  llvm::GlobalVariable* baseTable = nullptr;
  llvm::GlobalVariable* childTable = nullptr;
  for (auto& global : driver->getModule().globals()) {
    if (!global.hasInitializer()) continue;
    auto* entries = llvm::dyn_cast_or_null<llvm::ConstantStruct>(
        global.getInitializer());
    if (!entries || entries->getNumOperands() == 0 ||
        !llvm::isa<llvm::Function>(entries->getOperand(0)))
      continue;
    ++tableCount;
    EXPECT_TRUE(global.isConstant());
    auto* layout = llvm::dyn_cast<llvm::StructType>(global.getValueType());
    ASSERT_NE(layout, nullptr);
    if (layout->getNumElements() == 2) baseTable = &global;
    if (layout->getNumElements() == 4) childTable = &global;
  }
  EXPECT_EQ(tableCount, 2u);
  ASSERT_NE(baseTable, nullptr);
  ASSERT_NE(childTable, nullptr);
  EXPECT_EQ(childTable->getInitializer()->getOperand(3), baseTable);
  EXPECT_EQ(driver->getModule().getFunction("__sun_interface_borrow_drop"),
            nullptr);
}
