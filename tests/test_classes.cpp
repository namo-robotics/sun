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
        return a.b.c;
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
        return p.x;
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
        return p.y;
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
        return c.get();
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
        return calc.add(5);
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
        return v1.double();
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
        return c.get();
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
        return p.x + p.y + p.z;
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
        return box.area();
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
        return n1.get() + n2.get();
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
        return f.a + s.b;
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
        return p.x + p.y;
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
