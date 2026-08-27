// Tests that a constructor's first write to a field drops nothing: the field
// has never held a value, so no deinit runs on its zeroed storage. Every
// later write to the same field still drops what it replaces.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

namespace {

// Res counts every deinit call unconditionally — the point of these tests is
// that no call reaches it for storage that never held a value. Real owning
// types guard on the all-zero state; this one does not, so a spurious call
// shows up in the count.
const char* kResPreamble = R"(
    var deinits: i32 = 0;

    class Res {
      var v: i32;
      function init(v: i32) {
        this.v = v;
      }
      function deinit() void {
        deinits = deinits + 1;
      }
      function get() i32 {
        return this.v;
      }
    }
)";

std::string withPreamble(const std::string& body) {
  return std::string(kResPreamble) + body;
}

}  // namespace

TEST(MemorySafety_Drops_FieldInit, first_write_in_init_drops_nothing) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      function init() {
        this.r = Res(7);
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder();
      }
      return deinits;
    }
  )"));
  // Only Res(7), when h goes out of scope
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_FieldInit, field_taken_from_a_parameter_drops_nothing) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      function init(r: Res) {
        this.r = r;
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder(Res(7));
      }
      return deinits;
    }
  )"));
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_FieldInit,
     every_field_of_a_constructor_is_a_first_write) {
  auto value = executeString(withPreamble(R"(
    class Pair {
      var a: Res;
      var b: Res;
      function init() {
        this.a = Res(1);
        this.b = Res(2);
      }
    }

    function main() i32 {
      if (true) {
        var p = Pair();
      }
      return deinits;
    }
  )"));
  // One drop per field at scope exit, none during init
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_FieldInit, a_later_write_still_drops_what_it_replaces) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      function init(r: Res) {
        this.r = r;
      }
      function replace(r: Res) void {
        this.r = r;
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder(Res(1));
        h.replace(Res(2));
      }
      return deinits;
    }
  )"));
  // Res(1) when it is replaced, Res(2) at scope exit
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_FieldInit, a_second_write_inside_init_drops_the_first) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      function init() {
        this.r = Res(1);
        this.r = Res(2);
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder();
      }
      return deinits;
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_FieldInit, each_branch_of_a_choice_is_a_first_write) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      function init(flag: bool) {
        if (flag) {
          this.r = Res(1);
        } else {
          this.r = Res(2);
        }
      }
    }

    function main() i32 {
      if (true) {
        var taken = Holder(true);
      }
      if (true) {
        var other = Holder(false);
      }
      return deinits;
    }
  )"));
  // One drop per holder at scope exit; the branch not taken assigns nothing
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_FieldInit,
     a_write_after_a_branch_that_may_have_written) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      function init(flag: bool) {
        if (flag) {
          this.r = Res(1);
        }
        this.r = Res(2);
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder(true);
      }
      return deinits;
    }
  )"));
  // Res(1) is replaced, Res(2) drops at scope exit
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_FieldInit,
     a_branch_that_returns_leaves_nothing_behind) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      function init(flag: bool) {
        if (flag) {
          this.r = Res(1);
          return;
        }
        this.r = Res(2);
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder(false);
      }
      return deinits;
    }
  )"));
  // The branch that wrote Res(1) returned, so it cannot have reached the
  // write below it: that write is still a first write
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_FieldInit, a_method_called_once_the_object_is_whole) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      function init() {
        this.r = Res(1);
        this.refill();
      }
      function refill() void {
        this.r = Res(2);
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder();
      }
      return deinits;
    }
  )"));
  // init's write starts the field's life and drops nothing; refill replaces
  // Res(1) and drops it; Res(2) drops at scope exit
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_FieldInit,
     reading_a_field_does_not_end_the_first_write) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var a: Res;
      var b: Res;
      function init() {
        this.a = Res(1);
        this.b = Res(this.a.get() + 1);
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder();
      }
      return deinits;
    }
  )"));
  // Reading this.a cannot bring this.b to life: two drops, both at scope exit
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_FieldInit, a_write_in_a_loop_replaces_and_drops) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      function init() {
        this.r = Res(0);
        var i: i32 = 1;
        while (i < 3) {
          this.r = Res(i);
          i = i + 1;
        }
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder();
      }
      return deinits;
    }
  )"));
  // The write before the loop starts the field's life and drops nothing. A
  // loop body runs again with the field live, so its write always replaces:
  // two replacements, then the drop at scope exit.
  EXPECT_EQ(value, 3);
}

TEST(MemorySafety_Drops_FieldInit,
     a_method_that_fills_the_fields_drops_nothing) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      function init() {
        this.fill();
      }
      function fill() void {
        this.r = Res(7);
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder();
      }
      return deinits;
    }
  )"));
  // fill's write lands on a field that has never held a value. Its body alone
  // cannot say so, so the storage is checked at run time, found all zero, and
  // nothing is dropped. One drop, at scope exit.
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_FieldInit,
     a_method_reused_on_a_live_field_still_drops) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      function init() {
        this.fill();
      }
      function fill() void {
        this.r = Res(7);
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder();
        h.fill();
      }
      return deinits;
    }
  )"));
  // The same write, reached with the field live: the run-time check finds
  // something there and drops it. Then the drop at scope exit.
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_FieldInit,
     payload_enum_field_first_write_drops_nothing) {
  auto value = executeString(withPreamble(R"(
    enum Slot { Full(Res), Empty }

    class Holder {
      var s: Slot;
      function init() {
        this.s = Slot.Full(Res(7));
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder();
      }
      return deinits;
    }
  )"));
  // A zeroed tag reads as the first variant, which here owns a Res, so the
  // drop of the zeroed field would reach Res.deinit
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_FieldInit, generic_class_first_write_drops_nothing) {
  auto value = executeString(withPreamble(R"(
    class Box<T> {
      var item: T;
      function init(item: T) {
        this.item = item;
      }
    }

    function main() i32 {
      if (true) {
        var b = Box<Res>(Res(7));
      }
      return deinits;
    }
  )"));
  EXPECT_EQ(value, 1);
}
