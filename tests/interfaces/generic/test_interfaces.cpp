// tests/interfaces/generic/test_interfaces.cpp - Tests for generic interfaces

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

TEST(Interfaces_Generic, simple_generic_interface_definition) {
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

TEST(Interfaces_Generic, generic_interface_with_method) {
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

TEST(Interfaces_Generic, class_implements_generic_interface) {
  auto value = executeString(R"(
    interface IBox<T> {
      var value: T;
      function get() T;
    }

    class IntBox implements IBox<i32> {
      init(v: i32) {
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

TEST(Interfaces_Generic, generic_class_implements_generic_interface) {
  auto value = executeString(R"(
    interface IContainer<T> {
      function get() T;
    }

    class Box<T> implements IContainer<T> {
      var item: T;
      
      init(v: T) {
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

TEST(Interfaces_Generic, two_type_parameters) {
  auto value = executeString(R"(
    interface IPair<A, B> {
      function first() A;
      function second() B;
    }

    class Pair<X, Y> implements IPair<X, Y> {
      var a: X;
      var b: Y;
      
      init(x: X, y: Y) {
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

TEST(Interfaces_Generic, multiple_instantiations) {
  auto value = executeString(R"(
    interface IWrapper<T> {
      function unwrap() T;
    }

    class IntWrapper implements IWrapper<i32> {
      var val: i32;
      
      init(v: i32) {
        this.val = v;
      }
      
      function unwrap() i32 {
        return this.val;
      }
    }

    class BoolWrapper implements IWrapper<bool> {
      var val: bool;
      
      init(v: bool) {
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

TEST(Interfaces_Generic, interface_field_inherited) {
  auto value = executeString(R"(
    interface IValue<T> {
      var data: T;
    }

    class IntValue implements IValue<i32> {
      init(v: i32) {
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
