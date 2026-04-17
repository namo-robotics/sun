// tests/test_generics.cpp - Tests for generic class support

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

// ============================================================================
// Generic Class Definition Tests
// ============================================================================

TEST(GenericClass, generic_class_definition) {
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

TEST(GenericClass, generic_class_with_method) {
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

TEST(GenericClass, generic_class_with_constructor) {
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

// ============================================================================
// Generic Class Instantiation Tests
// ============================================================================

TEST(GenericClass, generic_instantiation_i32) {
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

TEST(GenericClass, generic_instantiation_f64) {
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

TEST(GenericClass, multiple_instantiations) {
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

TEST(GenericClass, generic_field_access) {
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

TEST(GenericClass, generic_method_returns_field) {
  auto value = executeString(R"(
    class Data<T> {
      var value: T;

      function init(v: T) {
        this.value = v;
      }

      function getValue() T {
        return this.value;
      }

      function setValue(v: T) {
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

TEST(GenericClass, generic_type_in_variable_declaration) {
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

TEST(GenericClass, two_type_parameters) {
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

TEST(GenericClass, mixed_type_parameters) {
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

TEST(GenericClass, generic_with_i64) {
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

TEST(GenericClass, generic_with_bool) {
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
        var f = Flag<bool>(1);
        var result = f.get();
        if (result) {
          return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}
