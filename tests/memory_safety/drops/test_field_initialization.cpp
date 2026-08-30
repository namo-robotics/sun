// Tests that a constructor's first write to a field drops nothing: the field
// has never held a value, so no deinit runs on its zeroed storage. Every
// later write to the same field still drops what it replaces — and a write
// where the compiler cannot tell which of the two it is, because only some of
// the paths reaching it assigned the field, is rejected.

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
      init(v: i32) {
        this.v = v;
      }
      deinit() {
        deinits = deinits + 1;
      }
      method get() i32 {
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
      init() {
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
      init(r: Res) {
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
      init() {
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
      init(r: Res) {
        this.r = r;
      }
      method replace(r: Res) void {
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

// A second write is fine when the field is known to hold a value already:
// there is no doubt about what it replaces.
TEST(MemorySafety_Drops_FieldInit, a_second_write_inside_init_drops_the_first) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init() {
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

// The same class of write is fine for a field that owns nothing: writing a
// number again releases nothing, so there is no rule to break.
TEST(MemorySafety_Drops_FieldInit, a_scalar_field_may_be_written_again) {
  auto value = executeString(withPreamble(R"(
    class Counter {
      var total: i32;
      init(n: i32) {
        this.total = 0;
        for (var i: i32 = 0; i < n; i = i + 1) {
          this.total = this.total + i;
        }
      }
      method get_total() i32 { return this.total; }
    }

    function main() i32 {
      var c = Counter(4);
      return c.get_total();
    }
  )"));
  EXPECT_EQ(value, 6);
}

TEST(MemorySafety_Drops_FieldInit, each_branch_of_a_choice_is_a_first_write) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init(flag: bool) {
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
     a_write_after_a_branch_that_may_have_written_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(withPreamble(R"(
    class Holder {
      var r: Res;
      init(flag: bool) {
        if (flag) {
          this.r = Res(1);
        }
        this.r = Res(2);
      }
    }

    function main() i32 { var h = Holder(true); return deinits; }
  )")),
                                "cannot tell whether field 'r' already holds");
}

TEST(MemorySafety_Drops_FieldInit,
     a_branch_that_returns_leaves_nothing_behind) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init(flag: bool) {
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
      init() {
        this.r = Res(1);
        this.refill();
      }
      method refill() void {
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
      init() {
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

// A write before the loop settles the question: every pass through the body
// replaces a value that is certainly there.
TEST(MemorySafety_Drops_FieldInit, a_write_in_a_loop_replaces_and_drops) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init() {
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

// Without that first write, the body's own write cannot tell whether it is
// the first pass or a later one.
TEST(MemorySafety_Drops_FieldInit, a_write_only_inside_a_loop_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(withPreamble(R"(
    class Holder {
      var r: Res;
      init() {
        var i: i32 = 0;
        while (i < 3) {
          this.r = Res(i);
          i = i + 1;
        }
      }
    }

    function main() i32 { var h = Holder(); return deinits; }
  )")),
                                "cannot tell whether field 'r' already holds");
}

TEST(MemorySafety_Drops_FieldInit,
     a_method_may_give_a_field_its_first_value) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init() {
        this.fill();
      }
      method fill() void {
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
  // A method's write always replaces and drops, whoever calls it. During
  // construction the field is still all zero, the state an owning deinit
  // treats as nothing to release — this Res counts unconditionally, so the
  // call on the zeroed storage is visible here: one for it, one at scope
  // exit.
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_FieldInit,
     a_method_reused_on_a_live_field_still_drops) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init() {
        this.r = Res(1);
        this.fill();
      }
      method fill() void {
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
  // The field is settled before fill is ever called, so fill's write always
  // replaces: Res(1) inside init, Res(7) on the later call, and the last
  // value at scope exit.
  EXPECT_EQ(value, 3);
}

TEST(MemorySafety_Drops_FieldInit,
     payload_enum_field_first_write_drops_nothing) {
  auto value = executeString(withPreamble(R"(
    enum Slot { Full(Res), Empty }

    class Holder {
      var s: Slot;
      init() {
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
      init(item: T) {
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

// A throw can leave a try block part-way through, so a catch clause cannot
// tell whether the block already gave the field its value.
TEST(MemorySafety_Drops_FieldInit, a_catch_that_reassigns_the_field_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(withPreamble(R"(
    class Boom implements IError {
      init() {}
      method code() i32 { return 1; }
      method message() static_ptr<u8> { return "boom"; }
    }

    function boom(f: bool) void throws IError { if (f) { throw Boom(); } }

    class Holder {
      var r: Res;
      init(f: bool) {
        try { this.r = Res(1); boom(f); } catch (e: Boom) { this.r = Res(2); }
      }
    }

    function main() i32 { var h = Holder(true); return deinits; }
  )")),
                                "cannot tell whether field 'r' already holds");
}

// Settling the field before the try leaves nothing in doubt: both the try and
// the catch replace a value that is certainly there.
TEST(MemorySafety_Drops_FieldInit, a_field_settled_before_a_try_is_certain) {
  auto value = executeString(withPreamble(R"(
    class Boom implements IError {
      init() {}
      method code() i32 { return 1; }
      method message() static_ptr<u8> { return "boom"; }
    }

    function boom(f: bool) void throws IError { if (f) { throw Boom(); } }

    class Holder {
      var r: Res;
      init(f: bool) {
        this.r = Res(0);
        try { boom(f); this.r = Res(1); } catch (e: Boom) { this.r = Res(2); }
      }
    }

    function main() i32 {
      if (true) {
        var h = Holder(true);
      }
      return deinits;
    }
  )"));
  // Res(0) replaced in the catch, then Res(2) at scope exit
  EXPECT_EQ(value, 2);
}

// The walk goes into the methods a constructor hands work to. One that calls
// itself round again cannot be followed, so the object has to be whole first.
TEST(MemorySafety_Drops_FieldInit, a_self_recursive_filler_is_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(withPreamble(R"(
    class Holder {
      var r: Res;
      init() {
        this.fill(2);
      }
      method fill(n: i32) void {
        if (n > 0) { this.fill(n - 1); }
        this.r = Res(n);
      }
    }

    function main() i32 { var h = Holder(); return deinits; }
  )")),
                                "Cannot call method 'fill'");
}

// A method may still assign fields that own nothing on the constructor's
// behalf — a write to a number releases nothing, so both callers agree on
// what it means.
TEST(MemorySafety_Drops_FieldInit, a_method_may_settle_scalar_fields) {
  auto value = executeString(withPreamble(R"(
    class Shape {
      var width: i32;
      var height: i32;
      init(w: i32, h: i32) {
        this.resize(w, h);
      }
      method resize(w: i32, h: i32) void {
        this.width = w;
        this.height = h;
      }
      method area() i32 { return this.width * this.height; }
    }

    function main() i32 {
      var s = Shape(3, 4);
      return s.area();
    }
  )"));
  EXPECT_EQ(value, 12);
}

// Constructors are checked after every method body is analyzed, so what the
// walk finds inside a helper does not depend on declaration order: a bound
// method reference is reported as one even when the helper is declared after
// the constructor.
TEST(MemorySafety_Drops_FieldInit,
     helper_diagnostics_do_not_depend_on_declaration_order) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileString(withPreamble(R"(
    class Holder {
      var r: Res;
      init() { this.fill(); }
      method fill() void {
        var cb = this.tick;
        this.r = Res(1);
        cb();
      }
      method tick() void { }
    }

    function main() i32 { var h = Holder(); return deinits; }
  )")),
                                "Cannot take a reference to method 'tick'");
}
