// tests/interfaces/generic/test_interfaces.cpp - Tests for generic interfaces

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

using sun::driver::executeString;

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
      method get() T;
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
      method get() T;
    }

    class IntBox implements IBox<i32> {
      init(v: i32) {
        this.value = v;
      }
      
      method get() i32 {
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
      method get() T;
    }

    class Box<T> implements IContainer<T> {
      var item: T;
      
      init(v: T) {
        this.item = v;
      }
      
      method get() T {
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
      method first() A;
      method second() B;
    }

    class Pair<X, Y> implements IPair<X, Y> {
      var a: X;
      var b: Y;
      
      init(x: X, y: Y) {
        this.a = x;
        this.b = y;
      }
      
      method first() X {
        return this.a;
      }
      
      method second() Y {
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
      method unwrap() T;
    }

    class IntWrapper implements IWrapper<i32> {
      var val: i32;
      
      init(v: i32) {
        this.val = v;
      }
      
      method unwrap() i32 {
        return this.val;
      }
    }

    class BoolWrapper implements IWrapper<bool> {
      var val: bool;
      
      init(v: bool) {
        this.val = v;
      }
      
      method unwrap() bool {
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
      
      method get() i32 {
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

TEST(Interfaces_Generic, type_test_uses_resolved_specialization) {
  EXPECT_EQ(executeString(R"(
    interface Readable<T> { method get() T; }
    class Value implements Readable<i32> { init() {} method get() i32 { return 7; } }
    function main() i32 {
      var value = Value();
      if (_is<Readable<i32>>(value) == false) { return 1; }
      if (_is<Readable<i64>>(value)) { return 2; }
      return 0;
    }
  )"),
            0);
}

TEST(Interfaces_Generic, generic_method_binders_match_by_position) {
  auto value = executeString(R"(
    interface Factory<A> {
      method identity<T>(value: T) T;
    }
    class IntFactory implements Factory<i32> {
      method identity<U>(value: U) U { return value; }
    }
    function main() i32 {
      var factory = IntFactory();
      return factory.identity<i32>(42);
    }
  )");
  EXPECT_EQ(value, 42);
}
