// tests/test_classes.cpp - Tests for class support

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Basic Class Definition Tests
// ============================================================================

TEST(ClassTest, simple_class_definition) {
  auto value = executeString(R"(
    class Point {
      var x: i32;
      var y: i32;
    }

    function main() i32 {
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(ClassTest, class_with_method) {
  auto value = executeString(R"(
    class Counter {
      var value: i32;

      function get() i32 {
        return this.value;
      }
    }

    function main() i32 {
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Object Instantiation Tests
// ============================================================================

TEST(ClassTest, new_class_instance) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class Point {
      var x: i32;
      var y: i32;
      function init() {}
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var p = allocator.create<Point>();
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(ClassTest, class_that_allocates) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class B {
      var c: i32;
      function init() {
        this.c = 42;
      }
    }

    class A {
      var b: raw_ptr<B>;
      function init(alloc: ref HeapAllocator) {
        this.b = alloc.create<B>();
      }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var a: raw_ptr<A> = allocator.create<A>(allocator);
        var b: raw_ptr<B> = unsafe { a.b; };
        return unsafe { b.c; };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ClassTest, object_field_access) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class Point {
      var x: i32;
      var y: i32;
      
      function init(px: i32, py: i32) {
        this.x = px;
        this.y = py;
      }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var p = allocator.create<Point>(10, 20);
        return unsafe { p.x; };
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(ClassTest, object_field_read_y) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class Point {
      var x: i32;
      var y: i32;
      
      function init(px: i32, py: i32) {
        this.x = px;
        this.y = py;
      }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var p = allocator.create<Point>(10, 20);
        return unsafe { p.y; };
    }
  )");
  EXPECT_EQ(value, 20);
}

// ============================================================================
// Method Call Tests
// ============================================================================

TEST(ClassTest, method_call_no_args) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class Counter {
      var value: i32;
      
      function init(v: i32) {
        this.value = v;
      }
      
      function get() i32 {
        return this.value;
      }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var c = allocator.create<Counter>(42);
        return unsafe { c.get(); };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ClassTest, method_call_with_args) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class Calculator {
      var base: i32;
      
      function init(b: i32) {
        this.base = b;
      }
      
      function add(x: i32) i32 {
        return this.base + x;
      }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var calc = allocator.create<Calculator>(10);
        return unsafe { calc.add(5); };
    }
  )");
  EXPECT_EQ(value, 15);
}

TEST(ClassTest, chained_method_calls) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class Value {
      var n: i32;
      
      function init(x: i32) {
        this.n = x;
      }
      
      function double() i32 {
        return this.n * 2;
      }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var v1 = allocator.create<Value>(5);
        return unsafe { v1.double(); };
    }
  )");
  EXPECT_EQ(value, 10);
}

// ============================================================================
// Constructor Tests
// ============================================================================

TEST(ClassTest, constructor_with_no_args) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class Counter {
      var value: i32;
      
      function init() {
        this.value = 100;
      }
      
      function get() i32 {
        return this.value;
      }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var c = allocator.create<Counter>();
        return unsafe { c.get(); };
    }
  )");
  EXPECT_EQ(value, 100);
}

TEST(ClassTest, constructor_initializes_fields) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class Point3D {
      var x: i32;
      var y: i32;
      var z: i32;
      
      function init(px: i32, py: i32, pz: i32) {
        this.x = px;
        this.y = py;
        this.z = pz;
      }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var p = allocator.create<Point3D>(1, 2, 3);
        return unsafe { p.x; } + unsafe { p.y; } + unsafe { p.z; };
    }
  )");
  EXPECT_EQ(value, 6);
}

// ============================================================================
// Multiple Fields and Methods
// ============================================================================

TEST(ClassTest, multiple_methods) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class Box {
      var width: i32;
      var height: i32;
      
      function init(w: i32, h: i32) {
        this.width = w;
        this.height = h;
      }
      
      function area() i32 {
        return this.width * this.height;
      }
      
      function perimeter() i32 {
        return 2 * this.width + 2 * this.height;
      }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var box = allocator.create<Box>(3, 4);
        return unsafe { box.area(); };
    }
  )");
  EXPECT_EQ(value, 12);
}

TEST(ClassTest, multiple_objects_same_class) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class Number {
      var value: i32;
      
      function init(v: i32) {
        this.value = v;
      }
      
      function get() i32 {
        return this.value;
      }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var n1 = allocator.create<Number>(10);
        var n2 = allocator.create<Number>(20);
        return unsafe { n1.get(); } + unsafe { n2.get(); };
    }
  )");
  EXPECT_EQ(value, 30);
}

// ============================================================================
// Multiple Classes
// ============================================================================

TEST(ClassTest, multiple_classes) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class First {
      var a: i32;
      function init(v: i32) { this.a = v; }
    }
    
    class Second {
      var b: i32;
      function init(v: i32) { this.b = v; }
    }

    function main() i32 {
        var allocator = make_heap_allocator();
        var f = allocator.create<First>(5);
        var s = allocator.create<Second>(7);
        return unsafe { f.a; } + unsafe { s.b; };
    }
  )");
  EXPECT_EQ(value, 12);
}

// ============================================================================
// Float fields
// ============================================================================

TEST(ClassTest, float_fields) {
  auto value = executeString(R"(
    import "stdlib/allocator.sun";
    using sun;
    
    class PointF {
      var x: f64;
      var y: f64;
      
      function init(px: f64, py: f64) {
        this.x = px;
        this.y = py;
      }
    }

    function main() f64 {
        var allocator = make_heap_allocator();
        var p = allocator.create<PointF>(1.5, 2.5);
        return unsafe { p.x; } + unsafe { p.y; };
    }
  )");
  EXPECT_TRUE(std::holds_alternative<double>(value));
  EXPECT_NEAR(std::get<double>(value), 4.0, 0.001);
}

// ============================================================================
// Error Tests
// ============================================================================

TEST(ClassTest, duplicate_field_error_has_location) {
  try {
    executeString(R"(
    class Point {
      var x: i32;
      var x: i32;
    }

    function main() i32 { return 0; }
  )");
    FAIL() << "Expected SunError for duplicate field";
  } catch (const SunError& e) {
    std::string msg = e.what();
    // Error message should contain field name
    EXPECT_TRUE(msg.find("x") != std::string::npos)
        << "Error should mention field name 'x', got: " << msg;
    // Error message should contain class name
    EXPECT_TRUE(msg.find("Point") != std::string::npos)
        << "Error should mention class name 'Point', got: " << msg;
    // Error message should contain location info (format: "line:column:")
    // The duplicate field 'x' is on line 4
    EXPECT_TRUE(msg.find("4:") != std::string::npos)
        << "Error should contain line number, got: " << msg;
  }
}

TEST(ClassTest, global_class_var) {
  auto value = executeString(R"(
    class Point {
      var x: i32;
      var y: i32;
    }

    var p: Point = Point(1, 2);

    function main() i32 {
        return p.x + p.y;
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(ClassTest, constructor_wrong_argument_count) {
  EXPECT_THROW(executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        function init(x_: i32, y_: i32) {
            this.x = x_;
            this.y = y_;
        }
    }

    function main() i32 {
        var p = Point(1);
        return 0;
    }
  )"),
               SunError);
}

TEST(ClassTest, class_redefinition_error) {
  try {
    executeString(R"(
    class Foo {
      var x: i32;
    }

    class Foo {
      var x: i32;
    }

    function main() i32 { return 0; }
  )");
    FAIL() << "Expected SunError for class redefinition";
  } catch (const SunError& e) {
    std::string msg = e.what();
    EXPECT_TRUE(msg.find("redecl") != std::string::npos ||
                msg.find("Redefinition") != std::string::npos)
        << "Error should mention redefinition/redeclaration, got: " << msg;
    EXPECT_TRUE(msg.find("Foo") != std::string::npos)
        << "Error should mention class name 'Foo', got: " << msg;
  }
}

TEST(ClassTest, diamond_import_class_no_error) {
  // Diamond dependency: two imports bring the same class - should compile fine
  auto value = executeString(R"(
    import "tests/programs/diamond/left_class.sun";
    import "tests/programs/diamond/right_class.sun";
    function main() i32 {
      return left_get() + right_get();
    }
  )");
  EXPECT_EQ(value, 30);
}

// ============================================================================
// Partial Class Tests
// ============================================================================

TEST(ClassTest, partial_class_adds_methods) {
  // Partial class methods should work alongside primary methods
  auto value = executeString(R"(
    import "tests/programs/class_x_part_a.sun";
    function main() i32 {
      var x = X(10, 20);
      return x.get_a() + x.get_b();  // get_a from primary, get_b from partial
    }
  )");
  EXPECT_EQ(value, 30);
}

TEST(ClassTest, partial_class_mutual_calls) {
  // Primary can call partial methods and vice versa
  auto value = executeString(R"(
    import "tests/programs/class_x_part_a.sun";
    function main() i32 {
      var x = X(10, 20);
      // sum() in partial calls get_a() from primary
      // double_b() in primary calls get_b() from partial
      return x.sum() + x.double_b();  // (10+20) + (20*2) = 70
    }
  )");
  EXPECT_EQ(value, 70);
}

TEST(ClassTest, partial_class_no_fields_error) {
  // Partial classes cannot define fields
  try {
    executeString(R"(
      class Foo { var x: i32; function init() { this.x = 0; } }
      partial class Foo { var y: i32; }
      function main() i32 { return 0; }
    )");
    FAIL() << "Expected parsing error for field in partial class";
  } catch (const SunError& e) {
    std::string msg = e.what();
    EXPECT_TRUE(msg.find("cannot define fields") != std::string::npos)
        << "Error should mention 'cannot define fields', got: " << msg;
  }
}

TEST(ClassTest, partial_class_no_constructor_error) {
  // Partial classes cannot define constructors
  try {
    executeString(R"(
      class Foo { var x: i32; function init() { this.x = 0; } }
      partial class Foo { function init() { this.x = 1; } }
      function main() i32 { return 0; }
    )");
    FAIL() << "Expected parsing error for constructor in partial class";
  } catch (const SunError& e) {
    std::string msg = e.what();
    EXPECT_TRUE(msg.find("cannot define constructors") != std::string::npos)
        << "Error should mention 'cannot define constructors', got: " << msg;
  }
}

TEST(ClassTest, partial_class_duplicate_method_error) {
  // Partial classes cannot redefine methods from primary
  try {
    executeString(R"(
      class Foo { 
        var x: i32; 
        function init() { this.x = 0; }
        function get() i32 { return this.x; }
      }
      partial class Foo { 
        function get() i32 { return this.x + 1; }
      }
      function main() i32 { return 0; }
    )");
    FAIL() << "Expected error for duplicate method in partial class";
  } catch (const SunError& e) {
    std::string msg = e.what();
    EXPECT_TRUE(msg.find("already defined") != std::string::npos)
        << "Error should mention 'already defined', got: " << msg;
  }
}

TEST(ClassTest, partial_class_inline_simple) {
  // Simple inline partial class test (no file imports)
  auto value = executeString(R"(
    class Counter {
      var value: i32;
      function init(v: i32) { this.value = v; }
      function get() i32 { return this.value; }
    }
    
    partial class Counter {
      function increment() void { this.value = this.value + 1; }
      function add(n: i32) void { this.value = this.value + n; }
    }
    
    function main() i32 {
      var c = Counter(10);
      c.increment();      // value = 11
      c.add(5);           // value = 16
      return c.get();
    }
  )");
  EXPECT_EQ(value, 16);
}

// ============================================================================
// Member Assignment Tests
// ============================================================================

TEST(ClassTest, simple_member_assignment) {
  // Test a.b = x on a local variable
  auto value = executeString(R"(
    class Point {
      var x: i32;
      var y: i32;
      function init() {
        this.x = 0;
        this.y = 0;
      }
    }

    function main() i32 {
      var p = Point();
      p.x = 42;
      return p.x;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ClassTest, nested_member_assignment) {
  // Test a.b.c = x with nested class fields
  auto value = executeString(R"(
    class Inner {
      var value: i32;
      function init() { this.value = 0; }
    }

    class Outer {
      var inner: Inner;
      function init() { this.inner = Inner(); }
    }

    function main() i32 {
      var o = Outer();
      o.inner.value = 99;
      return o.inner.value;
    }
  )");
  EXPECT_EQ(value, 99);
}

TEST(ClassTest, deep_nested_member_assignment) {
  // Test a.b.c.d = x with multiple levels of nesting
  auto value = executeString(R"(
    class Level3 {
      var data: i32;
      function init() { this.data = 0; }
    }

    class Level2 {
      var l3: Level3;
      function init() { this.l3 = Level3(); }
    }

    class Level1 {
      var l2: Level2;
      function init() { this.l2 = Level2(); }
    }

    function main() i32 {
      var root = Level1();
      root.l2.l3.data = 123;
      return root.l2.l3.data;
    }
  )");
  EXPECT_EQ(value, 123);
}

TEST(ClassTest, this_nested_member_assignment) {
  // Test this.a.b = x inside a method
  auto value = executeString(R"(
    class Inner {
      var x: i32;
      function init() { this.x = 0; }
    }

    class Container {
      var inner: Inner;
      
      function init() {
        this.inner = Inner();
      }
      
      function setInnerValue(v: i32) void {
        this.inner.x = v;
      }
      
      function getInnerValue() i32 {
        return this.inner.x;
      }
    }

    function main() i32 {
      var c = Container();
      c.setInnerValue(77);
      return c.getInnerValue();
    }
  )");
  EXPECT_EQ(value, 77);
}

TEST(ClassTest, nested_member_access_as_argument) {
  // Test passing a.b.c.d to a function (read access)
  auto value = executeString(R"(
    class Level3 {
      var data: i32;
      function init(v: i32) { this.data = v; }
    }

    class Level2 {
      var l3: Level3;
      function init(v: i32) { this.l3 = Level3(v); }
    }

    class Level1 {
      var l2: Level2;
      function init(v: i32) { this.l2 = Level2(v); }
    }

    function double_it(x: i32) i32 {
      return x * 2;
    }

    function main() i32 {
      var root = Level1(21);
      // Pass deeply nested member to a function
      return double_it(root.l2.l3.data);
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Member Access on Temporary Values
// ============================================================================

TEST(ClassTest, member_access_on_function_return) {
  // Test accessing a field on the return value of a function
  auto value = executeString(R"(
    class Point {
      var x: i32;
      var y: i32;
      function init(px: i32, py: i32) {
        this.x = px;
        this.y = py;
      }
    }

    function make_point() Point {
      return Point(10, 20);
    }

    function main() i32 {
      var x = make_point().x;
      var y = make_point().y;
      return x + y;
    }
  )");
  EXPECT_EQ(value, 30);
}

TEST(ClassTest, member_access_on_constructor) {
  // Test accessing a field directly on a constructor call
  auto value = executeString(R"(
    class Point {
      var x: i32;
      var y: i32;
      function init(px: i32, py: i32) {
        this.x = px;
        this.y = py;
      }
    }

    function main() i32 {
      var x = Point(3, 7).x;
      var y = Point(3, 7).y;
      return x + y;
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(ClassTest, method_call_on_function_return) {
  // Test calling a method on the return value of a function
  auto value = executeString(R"(
    class Point {
      var x: i32;
      var y: i32;
      function init(px: i32, py: i32) {
        this.x = px;
        this.y = py;
      }
      function sum() i32 {
        return this.x + this.y;
      }
    }

    function make_point(a: i32, b: i32) Point {
      return Point(a, b);
    }

    function main() i32 {
      return make_point(15, 27).sum();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ClassTest, nested_member_access_on_temporary) {
  // Test nested member access on a temporary value
  auto value = executeString(R"(
    class Inner {
      var value: i32;
      function init(v: i32) { this.value = v; }
    }

    class Outer {
      var inner: Inner;
      function init(v: i32) { this.inner = Inner(v); }
    }

    function make_outer(v: i32) Outer {
      return Outer(v);
    }

    function main() i32 {
      return make_outer(42).inner.value;
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Passing Temporaries as Function Arguments
// ============================================================================

TEST(ClassTest, temporary_as_argument_primitive) {
  // Test passing a function result (primitive) as an argument
  auto value = executeString(R"(
    function get_value() i32 {
      return 10;
    }

    function double_it(x: i32) i32 {
      return x * 2;
    }

    function main() i32 {
      return double_it(get_value());
    }
  )");
  EXPECT_EQ(value, 20);
}

TEST(ClassTest, temporary_class_as_ref_argument) {
  // Test passing a temporary class as a ref argument
  auto value = executeString(R"(
    class Point {
      var x: i32;
      var y: i32;
      function init(px: i32, py: i32) {
        this.x = px;
        this.y = py;
      }
    }

    function make_point() Point {
      return Point(5, 7);
    }

    function sum_point(p: ref Point) i32 {
      return p.x + p.y;
    }

    function main() i32 {
      return sum_point(make_point());
    }
  )");
  EXPECT_EQ(value, 12);
}

TEST(ClassTest, constructor_as_ref_argument) {
  // Test passing a constructor call directly as a ref argument
  auto value = executeString(R"(
    class Point {
      var x: i32;
      var y: i32;
      function init(px: i32, py: i32) {
        this.x = px;
        this.y = py;
      }
    }

    function sum_point(p: ref Point) i32 {
      return p.x + p.y;
    }

    function main() i32 {
      return sum_point(Point(13, 29));
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ClassTest, chained_member_access_as_argument) {
  // Test passing a chained member access expression as an argument
  // Pattern: foo(bar().a.b)
  auto value = executeString(R"(
    class Inner {
      var value: i32;
      function init(v: i32) { this.value = v; }
    }

    class Outer {
      var inner: Inner;
      function init(v: i32) { this.inner = Inner(v); }
    }

    function make_outer(v: i32) Outer {
      return Outer(v);
    }

    function triple(x: i32) i32 {
      return x * 3;
    }

    function main() i32 {
      return triple(make_outer(14).inner.value);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ClassTest, chained_method_and_member_as_argument) {
  // Test passing result of chained method call and member access
  // Pattern: foo(bar().method().field)
  auto value = executeString(R"(
    class Data {
      var result: i32;
      function init(v: i32) { this.result = v; }
    }

    class Builder {
      var value: i32;
      function init(v: i32) { this.value = v; }
      function build() Data {
        return Data(this.value * 2);
      }
    }

    function make_builder(v: i32) Builder {
      return Builder(v);
    }

    function add_one(x: i32) i32 {
      return x + 1;
    }

    function main() i32 {
      // make_builder(10) returns Builder
      // .build() returns Data
      // .result is a primitive field
      return add_one(make_builder(10).build().result);
    }
  )");
  EXPECT_EQ(value, 21);
}
