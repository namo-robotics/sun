// Tests the two-phase rule a constructor follows: it assigns every field
// first, and only once every field has a value may the object be read from,
// called, or passed on. A field left unassigned is rejected rather than
// silently left zero.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

namespace {

const char* kResPreamble = R"(
    class Res {
      var v: i32;
      init(v: i32) {
        this.v = v;
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

// --- Phase one: every field gets a value ---------------------------------

TEST(Classes_TwoPhaseInit, field_left_unassigned_is_rejected) {
  EXPECT_THROW(executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init() { }
    }

    function main() i32 {
      var h = Holder();
      return 0;
    }
  )")),
               std::exception);
}

TEST(Classes_TwoPhaseInit, one_of_two_fields_unassigned_is_rejected) {
  EXPECT_THROW(executeString(withPreamble(R"(
    class Holder {
      var a: Res;
      var b: Res;
      init() {
        this.a = Res(1);
      }
    }

    function main() i32 {
      var h = Holder();
      return 0;
    }
  )")),
               std::exception);
}

TEST(Classes_TwoPhaseInit, assigned_on_only_one_branch_is_rejected) {
  EXPECT_THROW(executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init(flag: bool) {
        if (flag) {
          this.r = Res(1);
        }
      }
    }

    function main() i32 {
      var h = Holder(true);
      return 0;
    }
  )")),
               std::exception);
}

TEST(Classes_TwoPhaseInit, assigned_on_both_branches_is_accepted) {
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
      method get() i32 { return this.r.get(); }
    }

    function main() i32 {
      var h = Holder(false);
      return h.get();
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(Classes_TwoPhaseInit, assigned_only_in_a_loop_is_rejected) {
  // A loop body may run no times at all, so it guarantees nothing
  EXPECT_THROW(executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init(n: i32) {
        var i: i32 = 0;
        while (i < n) {
          this.r = Res(i);
          i = i + 1;
        }
      }
    }

    function main() i32 {
      var h = Holder(3);
      return 0;
    }
  )")),
               std::exception);
}

TEST(Classes_TwoPhaseInit, returning_before_a_field_is_assigned_is_rejected) {
  EXPECT_THROW(executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init(flag: bool) {
        if (flag) {
          return;
        }
        this.r = Res(1);
      }
    }

    function main() i32 {
      var h = Holder(false);
      return 0;
    }
  )")),
               std::exception);
}

// --- Phase one: nothing but assigning ------------------------------------

TEST(Classes_TwoPhaseInit, reading_a_field_before_it_is_assigned_is_rejected) {
  EXPECT_THROW(executeString(withPreamble(R"(
    class Holder {
      var n: Res;
      var r: Res;
      init() {
        this.n = Res(this.r.get());
        this.r = Res(1);
      }
    }

    function main() i32 {
      var h = Holder();
      return 0;
    }
  )")),
               std::exception);
}

TEST(Classes_TwoPhaseInit, reading_a_field_after_assigning_it_is_accepted) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var a: Res;
      var b: Res;
      init() {
        this.a = Res(7);
        this.b = Res(this.a.get() + 1);
      }
      method get() i32 { return this.b.get(); }
    }

    function main() i32 {
      var h = Holder();
      return h.get();
    }
  )"));
  EXPECT_EQ(value, 8);
}

// --- Phase one: handing the work to the object's own methods -------------

TEST(Classes_TwoPhaseInit, a_method_may_assign_the_fields) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init() {
        this.fill();
      }
      method fill() void {
        this.r = Res(7);
      }
      method get() i32 { return this.r.get(); }
    }

    function main() i32 {
      var h = Holder();
      return h.get();
    }
  )"));
  EXPECT_EQ(value, 7);
}

TEST(Classes_TwoPhaseInit, several_methods_may_finish_the_job_between_them) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var a: Res;
      var b: Res;
      init() {
        this.fill_a();
        this.fill_b();
      }
      method fill_a() void { this.a = Res(3); }
      method fill_b() void { this.b = Res(4); }
      method sum() i32 { return this.a.get() + this.b.get(); }
    }

    function main() i32 {
      var h = Holder();
      return h.sum();
    }
  )"));
  EXPECT_EQ(value, 7);
}

TEST(Classes_TwoPhaseInit, a_method_that_leaves_a_field_unassigned) {
  EXPECT_THROW(executeString(withPreamble(R"(
    class Holder {
      var a: Res;
      var b: Res;
      init() {
        this.fill_a();
      }
      method fill_a() void { this.a = Res(3); }
    }

    function main() i32 {
      var h = Holder();
      return 0;
    }
  )")),
               std::exception);
}

TEST(Classes_TwoPhaseInit, a_method_that_reads_a_field_with_no_value_yet) {
  EXPECT_THROW(executeString(withPreamble(R"(
    class Holder {
      var a: Res;
      var b: Res;
      init() {
        this.copy_a_into_b();
        this.a = Res(1);
      }
      method copy_a_into_b() void { this.b = Res(this.a.get()); }
    }

    function main() i32 {
      var h = Holder();
      return 0;
    }
  )")),
               std::exception);
}

TEST(Classes_TwoPhaseInit, a_method_may_read_a_field_the_constructor_assigned) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var a: Res;
      var b: Res;
      init() {
        this.a = Res(5);
        this.double_a_into_b();
      }
      method double_a_into_b() void { this.b = Res(this.a.get() * 2); }
      method get() i32 { return this.b.get(); }
    }

    function main() i32 {
      var h = Holder();
      return h.get();
    }
  )"));
  EXPECT_EQ(value, 10);
}

TEST(Classes_TwoPhaseInit, a_method_assigning_on_only_one_branch) {
  EXPECT_THROW(executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init(flag: bool) {
        this.maybe_fill(flag);
      }
      method maybe_fill(flag: bool) void {
        if (flag) {
          this.r = Res(1);
        }
      }
    }

    function main() i32 {
      var h = Holder(true);
      return 0;
    }
  )")),
               std::exception);
}

TEST(Classes_TwoPhaseInit, calling_a_method_once_the_object_is_whole) {
  auto value = executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init() {
        this.r = Res(1);
        this.bump();
      }
      method bump() void {
        this.r = Res(this.r.get() + 1);
      }
      method get() i32 { return this.r.get(); }
    }

    function main() i32 {
      var h = Holder();
      return h.get();
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(Classes_TwoPhaseInit, passing_this_before_the_object_is_whole) {
  EXPECT_THROW(executeString(withPreamble(R"(
    class Holder {
      var r: Res;
      init() {
        take(this);
        this.r = Res(1);
      }
    }

    function take(h: ref Holder) void { }

    function main() i32 {
      var h = Holder();
      return 0;
    }
  )")),
               std::exception);
}
