// Tests that returning and propagating concrete errors drops live owners
// in each exited scope.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

using sun::driver::executeString;

/** Keeps test fixtures and helpers local to this source file. */
namespace {

const char* kOwnerPreamble = R"(

/** Owns successes and concrete fixture errors. */


var counter: i32 = 0;

class Owner {
  init() {}
  deinit() {
    counter = counter + 1;
  }
}

exception TestError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function thrower() void throws {
  throw TestError();

  return;
}
)";

/** Prepends shared fixture declarations to a test program. */
std::string withPreamble(const std::string& body) {
  return std::string(kOwnerPreamble) + body;
}

}  // namespace

TEST(MemorySafety_Drops_UnwindCleanup, callee_throw_drops_callers_frame_owner) {
  auto value = executeString(withPreamble(R"(
function middle() void throws {
  var o = Owner();
   thrower();

  return;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => void throws {
     middle();

    return;
  };
  try { attempt_0();
 } catch (e: TestError) {
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
function inner() void throws {
  var a = Owner();
   thrower();

  return;
}

function outer() void throws {
  var b = Owner();
   inner();

  return;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => void throws {
     outer();

    return;
  };
  try { attempt_0();
 } catch (e: TestError) {
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

function helper() i32 throws {
  /** Executes a fallible operation. */
  var attempt_0 = () => void throws {
    var o = Owner();
    throw TestError();

    return;
  };
  try { attempt_0();
 } catch (e: TestError) {
      return counter;
     }
  return -1;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_1 = () => i32 throws {
    return  helper();
  };
  try { var value = attempt_1();
return value; } catch (e: TestError) { return -2; }
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

function helper() i32 throws {
  /** Executes a fallible operation. */
  var attempt_0 = () => void throws {
    var a = Owner();
    if (true) {
      var b = Owner();
      throw TestError();
    }

    return;
  };
  try { attempt_0();
 } catch (e: TestError) {
      return counter;
     }
  return -1;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_1 = () => i32 throws {
    return  helper();
  };
  try { var value = attempt_1();
return value; } catch (e: TestError) { return -2; }
  return -3;
}
)"));
  // throw leaves the if scope and the try body scope: both owners drop
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_UnwindCleanup,
     moved_owner_not_dropped_twice_on_unwind) {
  auto value = executeString(withPreamble(R"(
function middle() void throws {
  var a = Owner();
  var b = a;
   thrower();

  return;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => void throws {
     middle();

    return;
  };
  try { attempt_0();
 } catch (e: TestError) {
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
function middle() void throws {
  var a = Owner();
   thrower();
  var b = Owner();

  return;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => void throws {
     middle();

    return;
  };
  try { attempt_0();
 } catch (e: TestError) {
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

function helper() i32 throws {
  var outside = Owner();
  /** Executes a fallible operation. */
  var attempt_0 = () => void throws {
    var inside = Owner();
     thrower();

    return;
  };
  try { attempt_0();
 } catch (e: TestError) {
      // inside dropped by the unwind edge; outside still live
     }
  return counter;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_1 = () => i32 throws {
    return  helper();
  };
  try { var value = attempt_1();
return value; } catch (e: TestError) { return -2; }
  return -3;
}
)"));
  // At the return, only `inside` has been dropped (outside drops later, at
  // helper's exit)
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_UnwindCleanup, no_owners_unwind_still_works) {
  auto value = executeString(withPreamble(R"(
function middle() void throws {
   thrower();

  return;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => void throws {
     middle();

    return;
  };
  try { attempt_0();
 } catch (e: TestError) {
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

/** Owns successes and concrete fixture errors. */


var counter: i32 = 0;

exception Payload {
  var errCode: i32 = 0;
  init(errCode: i32) {
    this.errCode = errCode;
  }
  const method code() i32 {
    return this.errCode;
  }
  const method message() static_ptr<u8> {
    return "payload error";
  }
}

function thrower() void throws {
  var p = Payload(7);
  throw p;

  return;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => void throws {
     thrower();

    return;
  };
  try { attempt_0();
 } catch (e: Payload) {
      return e.code();
     }
  return -1;
}
)");
  // The thrown object is moved into the exception buffer: frame cleanup must
  // not free its contents, and the catch binding still reads valid data
  EXPECT_EQ(value, 7);
}
