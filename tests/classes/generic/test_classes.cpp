// tests/classes/generic/test_classes.cpp - Tests for generic class support

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "error.h"
#include "execution_utils.h"

// ============================================================================
// Generic Class Definition Tests
// ============================================================================

TEST(Classes_Generic, generic_class_definition) {
  // Just parsing and semantic analysis, no instantiation
  auto value = executeString(R"(
    class Box<T> {
      var value: T;
    }

    function main() i32 {
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Classes_Generic, generic_class_with_method) {
  auto value = executeString(R"(
    class Container<T> {
      var data: T;

      function get() T {
        return this.data;
      }
    }

    function main() i32 {
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Classes_Generic, generic_class_with_constructor) {
  auto value = executeString(R"(
    class Wrapper<T> {
      var value: T;

      function init(v: T) {
        this.value = v;
      }

      function get() T {
        return this.value;
      }
    }

    function main() i32 {
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// A generic constructor call is resolved against the specialization's init
// overloads like any other class; a mismatch must be a compile error, not a
// silently skipped constructor.
TEST(Classes_Generic, generic_constructor_wrong_argument_count) {
  EXPECT_THROW(executeString(R"(
    class Wrapper<T> {
      var value: T;
      function init(v: T) { this.value = v; }
    }

    function main() i32 {
        var w = Wrapper<i32>();
        return 0;
    }
  )"),
               SunError);
}

TEST(Classes_Generic, generic_constructor_wrong_argument_type) {
  EXPECT_THROW(executeString(R"(
    class Wrapper<T> {
      var value: T;
      function init(v: T) { this.value = v; }
    }

    function main() i32 {
        var w = Wrapper<i32>(true);
        return 0;
    }
  )"),
               SunError);
}

// ============================================================================
// Generic Class Instantiation Tests
// ============================================================================

TEST(Classes_Generic, generic_instantiation_i32) {
  auto value = executeString(R"(
    class Box<T> {
      var value: T;

      function init(v: T) {
        this.value = v;
      }

      function get() T {
        return this.value;
      }
    }

    function main() i32 {
        var b = Box<i32>(42);
        return b.get();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Classes_Generic, generic_instantiation_f64) {
  auto value = executeString(R"(
    class Box<T> {
      var value: T;

      function init(v: T) {
        this.value = v;
      }

      function get() T {
        return this.value;
      }
    }

    function main() i32 {
        var b = Box<f64>(3.14);
        var result = b.get();
        if (result > 3.0) {
          return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Classes_Generic, multiple_instantiations) {
  // Same generic class instantiated with different types
  auto value = executeString(R"(
    class Holder<T> {
      var value: T;

      function init(v: T) {
        this.value = v;
      }

      function get() T {
        return this.value;
      }
    }

    function main() i32 {
        var intHolder = Holder<i32>(100);
        var floatHolder = Holder<f64>(2.5);
        
        var intVal = intHolder.get();
        var floatVal = floatHolder.get();
        
        if (intVal == 100) {
          if (floatVal > 2.0) {
            return 1;
          }
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Classes_Generic, generic_field_access) {
  auto value = executeString(R"(
    class Pair<T> {
      var first: T;
      var second: T;

      function init(a: T, b: T) {
        this.first = a;
        this.second = b;
      }
    }

    function main() i32 {
        var p = Pair<i32>(10, 20);
        return p.first + p.second;
    }
  )");
  EXPECT_EQ(value, 30);
}

TEST(Classes_Generic, generic_method_returns_field) {
  auto value = executeString(R"(
    class Data<T> {
      var value: T;

      function init(v: T) {
        this.value = v;
      }

      function getValue() T {
        return this.value;
      }

      function setValue(v: T) void {
        this.value = v;
      }
    }

    function main() i32 {
        var d = Data<i32>(5);
        d.setValue(10);
        return d.getValue();
    }
  )");
  EXPECT_EQ(value, 10);
}

// ============================================================================
// Generic Type Annotation Tests
// ============================================================================

TEST(Classes_Generic, generic_type_in_variable_declaration) {
  auto value = executeString(R"(
    class Num<T> {
      var val: T;

      function init(v: T) {
        this.val = v;
      }

      function get() T {
        return this.val;
      }
    }

    function main() i32 {
        var n: Num<i32> = Num<i32>(77);
        return n.get();
    }
  )");
  EXPECT_EQ(value, 77);
}

// ============================================================================
// Multiple Type Parameters Tests
// ============================================================================

TEST(Classes_Generic, two_type_parameters) {
  auto value = executeString(R"(
    class Pair<A, B> {
      var first: A;
      var second: B;

      function init(a: A, b: B) {
        this.first = a;
        this.second = b;
      }

      function getFirst() A {
        return this.first;
      }

      function getSecond() B {
        return this.second;
      }
    }

    function main() i32 {
        var p = Pair<i32, i32>(10, 20);
        return p.getFirst() + p.getSecond();
    }
  )");
  EXPECT_EQ(value, 30);
}

TEST(Classes_Generic, mixed_type_parameters) {
  auto value = executeString(R"(
    class KeyValue<K, V> {
      var key: K;
      var value: V;

      function init(k: K, v: V) {
        this.key = k;
        this.value = v;
      }

      function getKey() K {
        return this.key;
      }

      function getValue() V {
        return this.value;
      }
    }

    function main() i32 {
        var kv = KeyValue<i32, f64>(42, 3.14);
        var k = kv.getKey();
        var v = kv.getValue();
        if (k == 42) {
          if (v > 3.0) {
            return 1;
          }
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Generic Class with i64 and Other Integer Types
// ============================================================================

TEST(Classes_Generic, generic_with_i64) {
  auto value = executeString(R"(
    class BigBox<T> {
      var value: T;

      function init(v: T) {
        this.value = v;
      }

      function get() T {
        return this.value;
      }
    }

    function main() i32 {
        var b = BigBox<i64>(3000000000);
        var result = b.get();
        if (result > 2999999999) {
          return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Classes_Generic, generic_with_bool) {
  auto value = executeString(R"(
    class Flag<T> {
      var value: T;

      function init(v: T) {
        this.value = v;
      }

      function get() T {
        return this.value;
      }
    }

    function main() i32 {
        var f = Flag<bool>(true);
        var result = f.get();
        if (result) {
          return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Generic bodies are analyzed in their definition scope, not the requester's
// ============================================================================

TEST(Classes_Generic_Scoping, method_body_cannot_see_requesters_locals) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Box<T> {
        var v: T;
        function init(v: T) { this.v = v; }
        function get() T { return this.v + secret; }
    }
    function main() i32 {
        var secret: i32 = 5;
        var b = Box<i32>(1);
        return b.get();
    }
  )"), "Unknown variable");
}

TEST(Classes_Generic_Scoping, qualified_cross_module_instantiation_without_using) {
  auto value = executeString(R"(
    public module lib {
        function bonus() i32 { return 100; }
        public class Thing<T> {
            var v: T;
            public function init(v: T) { this.v = v; }
            public function get() T { return this.v + bonus(); }
        }
    }
    public module app {
        public function run() i32 { var t = lib.Thing<i32>(7); return t.get(); }
    }
    function main() i32 { return app.run(); }
  )");
  EXPECT_EQ(value, 107);
}

// ============================================================================
// Unresolvable Generic Types Are Fatal
//
// A generic type that cannot be instantiated used to print an error and
// return a null type, letting the compiler carry on and emit a binary that
// hung at runtime. It must abort the compilation instead.
// ============================================================================

TEST(Classes_Generic_Errors, error_on_generic_type_not_in_scope) {
  // `using sun;` outside the module block does not reach inside it, so Vec is
  // not visible where the return type is written. Uses the file path because
  // the single-string path scopes top-level `using` differently.
  EXPECT_THROW(compileFileWithStdlib("tests/programs/using_outside_module.sun"),
               SunError);
}

TEST(Classes_Generic_Errors, error_on_unknown_generic_type_name) {
  EXPECT_THROW(executeString(R"(
    class Box<T> {
      var v: T;
      function init(x: T) { this.v = x; }
    }

    function main() i32 {
      var b: Bocks<i32> = Box<i32>(1);
      return 0;
    };
  )"),
               SunError);
}

TEST(Classes_Generic_Errors, error_on_wrong_type_argument_count) {
  EXPECT_THROW(executeString(R"(
    class Pair<A, B> {
      var a: A;
      var b: B;
      function init(x: A, y: B) { this.a = x; this.b = y; }
    }

    function main() i32 {
      var p: Pair<i32> = Pair<i32, i32>(1, 2);
      return 0;
    };
  )"),
               SunError);
}

TEST(Classes_Generic_Errors, using_inside_module_resolves_generic) {
  auto value = executeStringWithStdlib(R"(
    public module namo {
      using sun;

      public function make(a: ref HeapAllocator) Vec<String> {
        var v = Vec<String>(a, 4);
        v.push(String(a, "hi"));
        return v;
      }
    }

    using sun;

    function main() i32 {
      var alloc = make_heap_allocator();
      var v = namo.make(alloc);
      return _convert<i32>(v.size());
    }
  )");
  EXPECT_EQ(value, 1);
}
