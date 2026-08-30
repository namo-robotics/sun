// Tests that exception unwinding runs deinit for live owners in every scope
// the exception leaves: throws clean their own frame inline, and calls that
// can throw get per-call-site cleanup landing pads when owners are live.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

namespace {

const char* kOwnerPreamble = R"(
    var counter: i32 = 0;

    class Owner {
      init() {}
      deinit() {
        counter = counter + 1;
      }
    }

    class TestError implements IError {
      init() {}
      method code() i32 { return 1; }
      method message() static_ptr<u8> { return "test error"; }
    }

    function thrower() void throws IError {
      throw TestError();
    }
)";

std::string withPreamble(const std::string& body) {
  return std::string(kOwnerPreamble) + body;
}

}  // namespace

TEST(MemorySafety_Drops_UnwindCleanup, callee_throw_drops_callers_frame_owner) {
  auto value = executeString(withPreamble(R"(
    function middle() void throws IError {
      var o = Owner();
      thrower();
    }

    function main() i32 {
      try {
        middle();
      } catch (e: IError) {
        return counter;
      }
      return -1;
    }
  )"));
  // middle's owner must be dropped while the exception unwinds through it
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_UnwindCleanup, unwind_through_two_frames_drops_both) {
  auto value = executeString(withPreamble(R"(
    function inner() void throws IError {
      var a = Owner();
      thrower();
    }

    function outer() void throws IError {
      var b = Owner();
      inner();
    }

    function main() i32 {
      try {
        outer();
      } catch (e: IError) {
        return counter;
      }
      return -1;
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_UnwindCleanup,
     local_throw_drops_try_scoped_owner_exactly_once) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 throws IError {
      try {
        var o = Owner();
        throw TestError();
      } catch (e: IError) {
        return counter;
      }
      return -1;
    }

    function main() i32 {
      try {
        return helper();
      } catch (e: IError) {
        return -2;
      }
      return -3;
    }
  )"));
  // The throw cleans the try-body scope inline; the fallthrough cleanup is a
  // different control path, so exactly one drop
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_UnwindCleanup,
     local_throw_from_nested_block_drops_all_left_scopes) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 throws IError {
      try {
        var a = Owner();
        if (true) {
          var b = Owner();
          throw TestError();
        }
      } catch (e: IError) {
        return counter;
      }
      return -1;
    }

    function main() i32 {
      try {
        return helper();
      } catch (e: IError) {
        return -2;
      }
      return -3;
    }
  )"));
  // throw leaves the if scope and the try body scope: both owners drop
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_UnwindCleanup,
     moved_owner_not_dropped_twice_on_unwind) {
  auto value = executeString(withPreamble(R"(
    function middle() void throws IError {
      var a = Owner();
      var b = a;
      thrower();
    }

    function main() i32 {
      try {
        middle();
      } catch (e: IError) {
        return counter;
      }
      return -1;
    }
  )"));
  // a was moved into b: only b is live at the throwing call
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_UnwindCleanup,
     owner_declared_after_throwing_call_not_dropped) {
  auto value = executeString(withPreamble(R"(
    function middle() void throws IError {
      var a = Owner();
      thrower();
      var b = Owner();
    }

    function main() i32 {
      try {
        middle();
      } catch (e: IError) {
        return counter;
      }
      return -1;
    }
  )"));
  // b is never constructed on the unwind path: the cleanup pad for the
  // thrower() call must only drop a
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_UnwindCleanup,
     caught_locally_then_normal_exit_no_extra_drops) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 throws IError {
      var outside = Owner();
      try {
        var inside = Owner();
        thrower();
      } catch (e: IError) {
        // inside dropped by the unwind edge; outside still live
      }
      return counter;
    }

    function main() i32 {
      try {
        return helper();
      } catch (e: IError) {
        return -2;
      }
      return -3;
    }
  )"));
  // At the return, only `inside` has been dropped (outside drops later, at
  // helper's exit)
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_UnwindCleanup, no_owners_unwind_still_works) {
  auto value = executeString(withPreamble(R"(
    function middle() void throws IError {
      thrower();
    }

    function main() i32 {
      try {
        middle();
      } catch (e: IError) {
        return 42;
      }
      return -1;
    }
  )"));
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_Drops_UnwindCleanup,
     thrown_error_object_survives_frame_cleanup) {
  auto value = executeString(R"(
    var counter: i32 = 0;

    class Payload implements IError {
      var errCode: i32;
      init(errCode: i32) {
        this.errCode = errCode;
      }
      deinit() {
        counter = counter + 1;
      }
      method code() i32 { return this.errCode; }
      method message() static_ptr<u8> { return "payload error"; }
    }

    function thrower() void throws IError {
      var p = Payload(7);
      throw p;
    }

    function main() i32 {
      try {
        thrower();
      } catch (e: Payload) {
        return e.code();
      } catch (e: IError) {
        return -2;
      }
      return -1;
    }
  )");
  // The thrown object is moved into the exception buffer: frame cleanup must
  // not free its contents, and the catch binding still reads valid data
  EXPECT_EQ(value, 7);
}
