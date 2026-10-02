/** Checks ownership and lifetime boundaries of runtime-managed exceptions. */
#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

/** Keeps shared source fixtures private to these tests. */
namespace {
/** Counts concrete error destruction and cleanup of its owned field. */
const std::string trackedError = R"(
var dropped: i32 = 0;
var fields_dropped: i32 = 0;
var order: i32 = 0;
/** Records destruction of an owned payload. */
class Payload {
  /** Creates the payload. */
  init() {}
  /** Records field cleanup. */
  deinit() {
    fields_dropped += 1;
    order = order * 10 + 2;
  }
}
/** Owns a payload while the exception runtime owns this error. */
class Tracked implements IError {
  var payload: Payload;
  var id: i32;
  /** Initializes the owned payload. */
  init(id: i32) {
    this.payload = Payload();
    this.id = id;
  }
  /** Records cleanup before owned fields are dropped. */
  deinit() {
    dropped += 1;
    order = order * 10 + 1;
  }
  /** Reads the error code. */
  const method code() i32 {
    return this.id;
  }
  /** Reads a static error message. */
  const method message() static_ptr<u8> {
    return "tracked";
  }
}
/** Provides a different matching tag. */
class Other implements IError {
  /** Creates the error. */
  init() {}
  /** Reads the error code. */
  const method code() i32 {
    return 99;
  }
  /** Reads a static message. */
  const method message() static_ptr<u8> {
    return "other";
  }
}
/** Moves an error into runtime ownership. */
enum Outcome<T> { Ok(T), Error(Tracked), Other(Other) }
/** Returns an owned tracked failure. */
function fail() Outcome<void> {
  return Outcome.Error(Tracked(7));
}
)";

/** Executes a handler and checks cleanup after it exits. */
sun::driver::SunValue runHandler(const std::string& body) {
  return sun::driver::executeString(trackedError +
                                    "/** Exercises a handler exit. */ function "
                                    "exercise() Outcome<void> {" +
                                    body + R"(
        return Outcome.Ok;
      }
      /** Observes cleanup after exercise has completed. */
      function main() i32 {
        match exercise() { Outcome.Ok => {}, _ => { return -1; } };
        return dropped * 100 + fields_dropped * 10 + order;
      }
    )");
}
}  // namespace

/** Every ordinary catch exit drops the original error and its field once. */
TEST(Errors_Ownership, handler_exit_paths_drop_payload) {
  for (const auto* body : {
           "match fail() { (e: ref Tracked) => { e.id = 8; }, _ => {} };",
           "match fail() { (e: const ref Tracked) => { if (e.code() != 7) { "
           "return Outcome.Other(Other()); } }, _ => {} };",
           "match fail() { (e: ref IError) => { if (dropped != 0) { return "
           "Outcome.Other(Other()); } }, _ => {} };",
           "match fail() { (e: ref Tracked) => { return Outcome.Ok; }, _ => {} "
           "};",
           "match fail() { (e: ref Tracked) => { if (e.code() == 7) { return "
           "Outcome.Ok; } }, _ => {} };",
           "while (true) { match fail() { (e: ref Tracked) => { break; }, _ => "
           "{ break; } }; }",
       }) {
    SCOPED_TRACE(body);
    EXPECT_EQ(runHandler(body), 122);
  }
}

/** Continuing a loop ends each catch before entering the next iteration. */
TEST(Errors_Ownership, continue_drops_each_exception) {
  EXPECT_EQ(runHandler(R"(
    var i = 0;
    while (i < 2) {
      i += 1;
      match fail() { (e: ref Tracked) => { continue; }, _ => {} };
    }
  )"),
            1432);
}

/** Rethrows retain the concrete tag and mutated payload without copying. */
TEST(Errors_Ownership, rethrow_preserves_type_and_single_owner) {
  for (int mode : {0, 1, 2, 3}) {
    SCOPED_TRACE(mode);
    EXPECT_EQ(runHandler(R"(
      /** Forwards an owned error after matching or borrowing it. */
      var forward = () => Outcome<void> {
        var mode = )" + std::to_string(mode) +
                         R"(;
        if (mode == 3) { try fail(); return Outcome.Ok; }
        return match fail() {
          Outcome.Error(e) => {
            if (mode == 0) { e.id = 42; }
            if (mode == 1) { var view: ref IError = e; if (view.code() != 7) { return Outcome.Other(Other()); } }
            if (mode == 2) { var view: const ref Tracked = e; if (view.code() != 7) { return Outcome.Other(Other()); } }
            return Outcome.Error(e);
          },
          _ => Outcome.Ok
        };
      };
      match forward() {
        (e: ref Tracked) => {
          if (dropped != 0 or (e.id != 7 and e.id != 42)) { return Outcome.Other(Other()); }
          e.id = 42;
        },
        _ => { return Outcome.Other(Other()); }
      };
    )"),
              122);
  }
}

/** Throwing a different error or calling a throwing function releases the old
 * error. */
TEST(Errors_Ownership, errors_leaving_handlers_drop_both_payloads) {
  for (const auto* body : {
           "var replace = () => Outcome<void> { match fail() { (e: ref "
           "Tracked) => { return Outcome.Error(Tracked(8)); }, _ => {} }; "
           "return Outcome.Ok; }; match replace() { (e: ref Tracked) => { if "
           "(dropped != 1 or e.id != 8) { return Outcome.Other(Other()); } }, "
           "_ => {} };",
           "var replace = () => Outcome<void> { match fail() { (e: ref "
           "Tracked) => { try fail(); }, _ => {} }; return Outcome.Ok; }; "
           "match replace() { (e: ref Tracked) => { if (dropped != 1) { return "
           "Outcome.Other(Other()); } }, _ => {} };",
           "match fail() { (outer: ref Tracked) => { if (true) { match fail() "
           "{ _ => {} }; } if (dropped != 1 or outer.id != 7) { return "
           "Outcome.Other(Other()); } }, _ => {} };",
       }) {
    SCOPED_TRACE(body);
    EXPECT_EQ(runHandler(body), 1432);
  }
}

/** Handler locals are destroyed before the exception they may borrow. */
TEST(Errors_Ownership, handler_locals_drop_before_exception) {
  EXPECT_EQ(runHandler("match fail() { (e: ref Tracked) => { var local = "
                       "Payload(); return Outcome.Ok; }, _ => {} };"),
            332);
}

/** Catch bindings cannot copy, escape, or impersonate a different active
 * exception. */
TEST(Errors_Ownership, invalid_catch_ownership_is_rejected) {
  for (const auto* statement : {
           "var copy: Tracked = e;",
           "var copy: IError = e;",
       }) {
    SCOPED_TRACE(statement);
    EXPECT_THROW(sun::driver::executeString(
                     trackedError +
                     "/** Rejects implicit copies from a borrow. */ function "
                     "main() i32 { match fail() { (e: ref Tracked) => {" +
                     statement + "}, _ => {} }; return 0; }"),
                 sun::support::SunError);
  }
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(trackedError + R"(
    /** Rejects a borrow that escapes its result owner. */
    function bad() ref Tracked {
      var result = fail();
      match result { (e: ref Tracked) => { return e; }, _ => {} };
      var local = Tracked(0); return local;
    }
    /** Supplies an entry point. */ function main() i32 { return 0; }
  )"),
                                "dangling reference");
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(trackedError + R"(
    /** Rejects mutation through a const payload borrow. */
    function main() i32 { match fail() { (e: const ref Tracked) => { e.id = 3; }, _ => {} }; return 0; }
  )"),
                                "const");
  EXPECT_THROW(sun::driver::executeString(trackedError + R"(
    /** An owned result cannot take ownership through an ordinary borrow. */
    function bad(e: ref Tracked) Outcome<void> { return Outcome.Error(e); }
    /** Supplies an entry point. */ function main() i32 { return 0; }
  )"),
               sun::support::SunError);
  EXPECT_THROW(sun::driver::executeString(trackedError + R"(
    /** Nested matches cannot move an outer borrowed payload. */
    function bad() Outcome<void> {
      match fail() { (outer: ref Tracked) => {
        match fail() { (inner: ref Tracked) => { return Outcome.Error(outer); }, _ => {} };
      }, _ => {} }; return Outcome.Ok;
    }
    /** Supplies an entry point. */ function main() i32 { return 0; }
  )"),
               sun::support::SunError);
}
