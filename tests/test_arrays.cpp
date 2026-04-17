// tests/test_generics.cpp - Tests for generic class support

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

TEST(Arrays, array_literal) {
  auto value = executeString(R"(
    function main() i32 {
        var x: array<i32, 5> = [1, 2, 3, 4, 5];
        return x[0] + x[1] + x[2] + x[3] + x[4];
    }
  )");
  EXPECT_EQ(value, 15);
}

TEST(Arrays, array_literal_nested) {
  auto value = executeString(R"(
    function main() i32 {
        var x: array<i32, 3, 2> = [[1, 2], [3, 4], [5, 6]];
        return x[0, 0] + x[0, 1] + x[1, 0] + x[1, 1] + x[2, 0] + x[2, 1];
    }
  )");
  EXPECT_EQ(value, 21);
}

TEST(Arrays, coerce_array) {
  auto value = executeString(R"(
    function sum(arr: ref array<i32>) i32 {
        return arr[0, 0] + arr[0, 1] + arr[1, 0] + arr[1, 1] + arr[2, 0] + arr[2, 1];
    }
    function main() i32 {
        var x: array<i32, 3, 2> = [[1, 2], [3, 4], [5, 6]];
        return sum(x);
    }
  )");
  EXPECT_EQ(value, 21);
}

TEST(Arrays, assign_to_array) {
  auto value = executeString(R"(
    function main() i32 {
        var x: array<i32, 2, 2> = [[1, 2], [3, 4]];
        x[0, 0] = 10;
        x[1, 0] = 20;
        return x[0, 0] + x[0, 1] + x[1, 0] + x[1, 1];
    }
  )");
  EXPECT_EQ(value, 10 + 2 + 20 + 4);
}

TEST(Arrays, assign_from_other_array) {
  auto value = executeString(R"(
    function main() i32 {
        var x = [[1,1], [1,1]];
        var y = [[-1,-1], [-1,-1]];
        x[0, 0] = y[1,1];
        return x[0, 0] + x[0, 1] + x[1, 0] + x[1, 1];
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Arrays, array_shape) {
  auto value = executeString(R"(
    function main() i32 {
        var x: array<i32, 3, 2> = [[1, 2], [3, 4], [5, 6]];
        var shape = x.shape();
        return shape[0] + shape[1];
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Arrays, array_in_class) {
  auto value = executeString(R"(
    class Matrix {
        var data: array<i32>;

        function init(d: ref array<i32>) {
            this.data = d;
        }

        function shape() array<i64> {
            return this.data.shape();
        }
    }
    function main() i32 {
        var x: array<i32, 3, 2> = [[1, 2], [3, 4], [5, 6]];
        var m = Matrix(x);
        var shape = m.shape();
        return shape[0] + shape[1];
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Arrays, array_in_class_in_class) {
  auto value = executeString(R"(

    class A {
        var data: array<i32>;

        function init(d: ref array<i32>) {
            this.data = d;
        }

        function shape() array<i64> {
            return this.data.shape();
        }
    }

    class B {
        var a: A;

        function init(d: ref array<i32>) {
            this.a = A(d);
        }

        function shape() array<i64> {
            return this.a.shape();
        }
    }
    function main() i32 {
        var x: array<i32, 3, 2> = [[1, 2], [3, 4], [5, 6]];
        var m = B(x);
        var shape = m.shape();
        return shape[0] + shape[1];
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Arrays, global_array) {
  auto value = executeString(R"(
    var x: array<i32, 5> = [1, 2, 3, 4, 5];

    function main() i32 {
        x[0] = 0;
        return x[0] + x[1] + x[2] + x[3] + x[4];
    }
  )");
  EXPECT_EQ(value, 14);
}

TEST(Arrays, class_with_array) {
  auto value = executeString(R"(
    class Box {
        var arr: array<i32, 5>; 
        function init(x: ref array<i32, 5>) {
            this.arr = x;
        }
    }
    function main() i32 {
        var x: array<i32, 5> = [1, 2, 3, 4, 5];
        var box = Box(x);
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Arrays, index_array_ref) {
  auto value = executeString(R"(
    function main() i32 {
        var x: array<i32, 5> = [1, 2, 3, 4, 5];
        ref y = x;
        return y[0] + y[1] + y[2] + y[3] + y[4];
    }
  )");
  EXPECT_EQ(value, 15);  // 1+2+3+4+5 = 15
}