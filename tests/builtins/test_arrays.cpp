// tests/builtins/test_arrays.cpp - Tests for array types
//
// A sized array<T, N> owns its elements inline and moves like any other
// compound value; an unsized array<T> is a view that exists only behind ref.

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"

namespace {

// A global drop counter and a class whose deinit bumps it, for tests that
// watch when array elements are released.
const char* kOwnerPreamble = R"(
    var drops: i32 = 0;

    class Owner {
      var id: i32;
      init(id: i32) { this.id = id; }
      deinit() { drops = drops + 1; }
    }
)";

std::string withOwner(const std::string& body) {
  return std::string(kOwnerPreamble) + body;
}

}  // namespace

TEST(Builtins_Arrays, array_literal) {
  auto value = executeString(R"(
    function main() i32 {
        var x: array<i32, 5> = [1, 2, 3, 4, 5];
        return x[0] + x[1] + x[2] + x[3] + x[4];
    }
  )");
  EXPECT_EQ(value, 15);
}

TEST(Builtins_Arrays, array_literal_nested) {
  auto value = executeString(R"(
    function main() i32 {
        var x: array<i32, 3, 2> = [[1, 2], [3, 4], [5, 6]];
        return x[0, 0] + x[0, 1] + x[1, 0] + x[1, 1] + x[2, 0] + x[2, 1];
    }
  )");
  EXPECT_EQ(value, 21);
}

// A sized array decays to a `ref array<T>` view at the call; the view keeps
// the rank and every dimension.
TEST(Builtins_Arrays, coerce_array) {
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

TEST(Builtins_Arrays, view_reports_rank_and_dimensions) {
  auto value = executeString(R"(
    function describe(arr: const ref array<i32>) i64 {
        return arr.ndims() * 100 + arr.dim(0) * 10 + arr.dim(1);
    }
    function main() i32 {
        var x: array<i32, 3, 2> = [[1, 2], [3, 4], [5, 6]];
        return _convert<i32>(describe(x));
    }
  )");
  EXPECT_EQ(value, 232);
}

TEST(Builtins_Arrays, assign_to_array) {
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

TEST(Builtins_Arrays, assign_from_other_array) {
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

// ndims() and dim(i) on a sized array are compile-time constants.
TEST(Builtins_Arrays, sized_array_rank_and_dimensions) {
  auto value = executeString(R"(
    function main() i32 {
        var x: array<i32, 3, 2> = [[1, 2], [3, 4], [5, 6]];
        return _convert<i32>(x.ndims() * 100 + x.dim(0) * 10 + x.dim(1));
    }
  )");
  EXPECT_EQ(value, 232);
}

// The #201 program: a sized array field embeds its elements in the object,
// so they survive the constructor's frame. The scrub call overwrites the
// stack where the constructor's local lived.
TEST(Builtins_Arrays, sized_field_survives_frame) {
  auto value = executeString(R"(
    class Dims {
        var d: array<i64, 4>;

        init() {
            var local: array<i64, 4> = [1, 2, 3, 4];
            this.d = local;
        }

        public const method get(i: i64) i64 {
            return this.d[i];
        }
    }

    function scrub() i64 {
        var junk: array<i64, 64> = [9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
                                    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
                                    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
                                    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9];
        return junk[63];
    }

    function main() i32 {
        var dims = Dims();
        var noise = scrub();
        return _convert<i32>(dims.get(2) + noise - 9);
    }
  )");
  EXPECT_EQ(value, 3);
}

// A class holding a sized array copies the elements in through a by-value
// parameter; the caller's array is moved from.
TEST(Builtins_Arrays, array_in_class) {
  auto value = executeString(R"(
    class Grid {
        var data: array<i32, 3, 2>;

        init(d: array<i32, 3, 2>) {
            this.data = d;
        }

        method rows() i64 {
            return this.data.dim(0);
        }

        method cols() i64 {
            return this.data.dim(1);
        }
    }
    function main() i32 {
        var x: array<i32, 3, 2> = [[1, 2], [3, 4], [5, 6]];
        var g = Grid(x);
        return _convert<i32>(g.rows() + g.cols()) + g.data[2, 1];
    }
  )");
  EXPECT_EQ(value, 11);
}

TEST(Builtins_Arrays, array_in_class_in_class) {
  auto value = executeString(R"(
    class A {
        var data: array<i32, 3, 2>;

        init(d: array<i32, 3, 2>) {
            this.data = d;
        }

        method total() i32 {
            return this.data[0, 0] + this.data[2, 1];
        }
    }

    class B {
        var a: A;

        init(d: array<i32, 3, 2>) {
            this.a = A(d);
        }

        method total() i32 {
            return this.a.total();
        }
    }
    function main() i32 {
        var x: array<i32, 3, 2> = [[1, 2], [3, 4], [5, 6]];
        var b = B(x);
        return b.total();
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(Builtins_Arrays, global_array) {
  auto value = executeString(R"(
    var x: array<i32, 5> = [1, 2, 3, 4, 5];

    function main() i32 {
        x[0] = 0;
        return x[0] + x[1] + x[2] + x[3] + x[4];
    }
  )");
  EXPECT_EQ(value, 14);
}

TEST(Builtins_Arrays, global_multidim_array) {
  auto value = executeString(R"(
    var g: array<i32, 2, 2> = [[1, 2], [3, 4]];

    function main() i32 {
        g[1, 1] = 7;
        return g[0, 0] + g[1, 1];
    }
  )");
  EXPECT_EQ(value, 8);
}

// Storing a sized array in a field moves the elements: the field has its own
// storage, so a later write to the source is not seen through the field.
TEST(Builtins_Arrays, class_with_array) {
  auto value = executeString(R"(
    class Box {
        var arr: array<i32, 5>;
        init(x: array<i32, 5>) {
            this.arr = x;
        }
    }
    function main() i32 {
        var x: array<i32, 5> = [1, 2, 3, 4, 5];
        var box = Box(x);
        return box.arr[0] + box.arr[4];
    }
  )");
  EXPECT_EQ(value, 6);
}

// A borrow of a sized array may not be read into an owning field: that would
// give the field and the borrowed array the same elements.
TEST(Builtins_Arrays, sized_ref_read_into_field_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Box {
        var arr: array<i32, 5>;
        init(x: ref array<i32, 5>) {
            this.arr = x;
        }
    }
    function main() i32 {
        var x: array<i32, 5> = [1, 2, 3, 4, 5];
        var box = Box(x);
        return 0;
    }
  )"),
                                "Cannot assign value of type 'ref array<i32, 5>'");
}

TEST(Builtins_Arrays, index_array_ref) {
  auto value = executeString(R"(
    function main() i32 {
        var x: array<i32, 5> = [1, 2, 3, 4, 5];
        ref y = x;
        return y[0] + y[1] + y[2] + y[3] + y[4];
    }
  )");
  EXPECT_EQ(value, 15);  // 1+2+3+4+5 = 15
}

TEST(Builtins_Arrays, ref_sized_array_param_indexes_statically) {
  auto value = executeString(R"(
    function corner(a: ref array<i32, 2, 2>) i32 {
        a[0, 0] = 40;
        return a[1, 0] + a[0, 0];
    }
    function main() i32 {
        var m: array<i32, 2, 2> = [[1, 2], [3, 4]];
        var c = corner(m);
        return c + m[0, 0];
    }
  )");
  EXPECT_EQ(value, 83);
}

// A returned array is an ordinary value: its elements come back with it.
TEST(Builtins_Arrays, return_array_by_value) {
  auto value = executeString(R"(
    function make_array() array<i32, 3> {
        var arr: array<i32, 3> = [10, 20, 30];
        return arr;
    }

    function main() i32 {
        var result = make_array();
        return result[0] + result[1] + result[2];
    }
  )");
  EXPECT_EQ(value, 60);  // 10+20+30 = 60
}

// A class with a sized array field embeds the elements: its size is theirs.
TEST(Builtins_Arrays, sizeof_sized_array) {
  auto value = executeString(R"(
    class Cells {
        var data: array<i32, 3, 2>;
        init() { this.data = [[1, 2], [3, 4], [5, 6]]; }
    }
    function main() i32 {
        return _convert<i32>(_sizeof<Cells>());
    }
  )");
  EXPECT_EQ(value, 24);
}

// -------------------------------------------------------------------
// Ownership: arrays move, and arrays of owning elements drop them
// -------------------------------------------------------------------

// Passing or binding an array by value moves it; the source is gone.
TEST(Builtins_Arrays, array_moves_on_by_value_transfer) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function main() i32 {
        var a: array<i32, 3> = [1, 2, 3];
        var b = a;
        return a[0] + b[0];
    }
  )"),
                                "use of moved variable 'a'");
}

TEST(Builtins_Arrays, moved_array_keeps_its_elements) {
  auto value = executeString(R"(
    function main() i32 {
        var a: array<i32, 3> = [1, 2, 3];
        var b = a;
        b[0] = 9;
        return b[0] + b[1] + b[2];
    }
  )");
  EXPECT_EQ(value, 14);
}

// A literal of class values stores the values themselves, not their
// addresses; each element is reachable by field afterwards.
TEST(Builtins_Arrays, array_of_classes_literal_stores_values) {
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        init(x: i32, y: i32) { this.x = x; this.y = y; }
    }
    function main() i32 {
        var pts = [Point(1, 2), Point(3, 4)];
        return pts[1].x + pts[0].y;
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(Builtins_Arrays, array_of_droppable_elements_drops_each) {
  auto value = executeString(withOwner(R"(
    function fill() i32 {
        var owners = [Owner(1), Owner(2), Owner(3)];
        return owners[1].id;
    }
    function main() i32 {
        var id = fill();
        return drops * 10 + id;
    }
  )"));
  EXPECT_EQ(value, 32);
}

TEST(Builtins_Arrays, array_field_of_droppable_elements_drops_with_owner) {
  auto value = executeString(withOwner(R"(
    class Pair {
        var items: array<Owner, 2>;
        init() { this.items = [Owner(1), Owner(2)]; }
    }
    function build() i32 {
        var p = Pair();
        return p.items[0].id;
    }
    function main() i32 {
        var id = build();
        return drops * 10 + id;
    }
  )"));
  EXPECT_EQ(value, 21);
}

// Moving an array of owners moves the elements once: they drop with the
// destination, not twice.
TEST(Builtins_Arrays, moved_array_of_owners_drops_once) {
  auto value = executeString(withOwner(R"(
    function take(owners: array<Owner, 2>) i32 {
        return owners[1].id;
    }
    function run() i32 {
        var owners = [Owner(1), Owner(2)];
        return take(owners);
    }
    function main() i32 {
        var id = run();
        return drops * 10 + id;
    }
  )"));
  EXPECT_EQ(value, 22);
}

TEST(Builtins_Arrays, indexed_assignment_drops_old_element) {
  auto value = executeString(withOwner(R"(
    function main() i32 {
        var owners = [Owner(1), Owner(2)];
        owners[0] = Owner(3);
        var after_replace = drops;
        return after_replace * 10 + owners[0].id;
    }
  )"));
  EXPECT_EQ(value, 13);
}

// An element of an array of owners is borrowed, never moved out.
TEST(Builtins_Arrays, element_move_out_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(withOwner(R"(
    function main() i32 {
        var owners = [Owner(1), Owner(2)];
        var first = owners[0];
        return first.id;
    }
  )")),
                                "Cannot move an element out of an array");
}

TEST(Builtins_Arrays, element_borrowed_in_place) {
  auto value = executeString(withOwner(R"(
    function main() i32 {
        var owners = [Owner(1), Owner(2)];
        const ref first = owners[0];
        return first.id + owners[1].id;
    }
  )"));
  EXPECT_EQ(value, 3);
}
