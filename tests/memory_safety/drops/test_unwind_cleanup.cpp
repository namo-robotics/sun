// Tests that exception unwinding runs deinit for live owners in every scope
// the exception leaves: throws clean their own frame inline, and calls that
// can throw get per-call-site cleanup landing pads when owners are live.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

using sun::driver::executeString;

/** Keeps test fixtures and helpers local to this source file. */
namespace {

const char* kOwnerPreamble = R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), TestError(TestError) }

var counter: i32 = 0;

class Owner {
  init() {}
  deinit() {
    counter = counter + 1;
  }
}

class TestError implements IError {
  init() {}
  const method code() i32 {
    return 1;
  }
  const method message() static_ptr<u8> {
    return "test error";
  }
}

function thrower() Outcome<void> {
  return Outcome.TestError(TestError());

  return Outcome.Ok;
}
)";

/** Prepends shared fixture declarations to a test program. */
std::string withPreamble(const std::string& body) {
  return std::string(kOwnerPreamble) + body;
}

}  // namespace

TEST(MemorySafety_Drops_UnwindCleanup, callee_throw_drops_callers_frame_owner) {
  auto value = executeString(withPreamble(R"(
function middle() Outcome<void> {
  var o = Owner();
  try thrower();

  return Outcome.Ok;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<void> {
    try middle();

    return Outcome.Ok;
  };
  match attempt_0() {
    Outcome.Ok => {},
    (e: ref IError) => {
      return counter;
    }
  };
  return -1;
}
)"));
  // middle's owner must be dropped while the exception unwinds through it
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_UnwindCleanup, unwind_through_two_frames_drops_both) {
  auto value = executeString(withPreamble(R"(
function inner() Outcome<void> {
  var a = Owner();
  try thrower();

  return Outcome.Ok;
}

function outer() Outcome<void> {
  var b = Owner();
  try inner();

  return Outcome.Ok;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<void> {
    try outer();

    return Outcome.Ok;
  };
  match attempt_0() {
    Outcome.Ok => {},
    (e: ref IError) => {
      return counter;
    }
  };
  return -1;
}
)"));
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_UnwindCleanup,
     local_throw_drops_try_scoped_owner_exactly_once) {
  auto value = executeString(withPreamble(R"(
function helper() Outcome<i32> {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<void> {
    var o = Owner();
    return Outcome.TestError(TestError());

    return Outcome.Ok;
  };
  match attempt_0() {
    Outcome.Ok => {},
    (e: ref IError) => {
      return Outcome.Ok(counter);
    }
  };
  return Outcome.Ok(-1);
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_1 = () => Outcome<i32> {
    return Outcome.Ok(try helper());
  };
  return match attempt_1() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -2;
    }
  };
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
function helper() Outcome<i32> {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<void> {
    var a = Owner();
    if (true) {
      var b = Owner();
      return Outcome.TestError(TestError());
    }

    return Outcome.Ok;
  };
  match attempt_0() {
    Outcome.Ok => {},
    (e: ref IError) => {
      return Outcome.Ok(counter);
    }
  };
  return Outcome.Ok(-1);
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_1 = () => Outcome<i32> {
    return Outcome.Ok(try helper());
  };
  return match attempt_1() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -2;
    }
  };
  return -3;
}
)"));
  // throw leaves the if scope and the try body scope: both owners drop
  EXPECT_EQ(value, 2);
}

TEST(MemorySafety_Drops_UnwindCleanup,
     moved_owner_not_dropped_twice_on_unwind) {
  auto value = executeString(withPreamble(R"(
function middle() Outcome<void> {
  var a = Owner();
  var b = a;
  try thrower();

  return Outcome.Ok;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<void> {
    try middle();

    return Outcome.Ok;
  };
  match attempt_0() {
    Outcome.Ok => {},
    (e: ref IError) => {
      return counter;
    }
  };
  return -1;
}
)"));
  // a was moved into b: only b is live at the throwing call
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_UnwindCleanup,
     owner_declared_after_throwing_call_not_dropped) {
  auto value = executeString(withPreamble(R"(
function middle() Outcome<void> {
  var a = Owner();
  try thrower();
  var b = Owner();

  return Outcome.Ok;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<void> {
    try middle();

    return Outcome.Ok;
  };
  match attempt_0() {
    Outcome.Ok => {},
    (e: ref IError) => {
      return counter;
    }
  };
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
function helper() Outcome<i32> {
  var outside = Owner();
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<void> {
    var inside = Owner();
    try thrower();

    return Outcome.Ok;
  };
  match attempt_0() {
    Outcome.Ok => {},
    (e: ref IError) => {
      // inside dropped by the unwind edge; outside still live
    }
  };
  return Outcome.Ok(counter);
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_1 = () => Outcome<i32> {
    return Outcome.Ok(try helper());
  };
  return match attempt_1() {
    Outcome.Ok(value) => value,
    (e: ref IError) => {
      return -2;
    }
  };
  return -3;
}
)"));
  // At the return, only `inside` has been dropped (outside drops later, at
  // helper's exit)
  EXPECT_EQ(value, 1);
}

TEST(MemorySafety_Drops_UnwindCleanup, no_owners_unwind_still_works) {
  auto value = executeString(withPreamble(R"(
function middle() Outcome<void> {
  try thrower();

  return Outcome.Ok;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<void> {
    try middle();

    return Outcome.Ok;
  };
  match attempt_0() {
    Outcome.Ok => {},
    (e: ref IError) => {
      return 42;
    }
  };
  return -1;
}
)"));
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_Drops_UnwindCleanup,
     thrown_error_object_survives_frame_cleanup) {
  auto value = executeString(R"(
/** Owns successes and concrete fixture errors. */
enum Outcome<T> { Ok(T), Payload(Payload) }

var counter: i32 = 0;

class Payload implements IError {
  var errCode: i32;
  init(errCode: i32) {
    this.errCode = errCode;
  }
  deinit() {
    counter = counter + 1;
  }
  const method code() i32 {
    return this.errCode;
  }
  const method message() static_ptr<u8> {
    return "payload error";
  }
}

function thrower() Outcome<void> {
  var p = Payload(7);
  return Outcome.Payload(p);

  return Outcome.Ok;
}

function main() i32 {
  /** Executes a fallible operation. */
  var attempt_0 = () => Outcome<void> {
    try thrower();

    return Outcome.Ok;
  };
  match attempt_0() {
    Outcome.Ok => {},
    (e: ref Payload) => {
      return e.code();
    },
    (e: ref IError) => {
      return -2;
    }
  };
  return -1;
}
)");
  // The thrown object is moved into the exception buffer: frame cleanup must
  // not free its contents, and the catch binding still reads valid data
  EXPECT_EQ(value, 7);
}
