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
    /** Creates the payload. */ init() {}
    /** Records field cleanup. */ deinit() { fields_dropped += 1; order = order * 10 + 2; }
  }
  /** Owns a payload while the exception runtime owns this error. */
  class Tracked implements IError {
    var payload: Payload;
    var id: i32;
    /** Initializes the owned payload. */ init(id: i32) { this.payload = Payload(); this.id = id; }
    /** Records cleanup before owned fields are dropped. */ deinit() { dropped += 1; order = order * 10 + 1; }
    /** Reads the error code. */ const method code() i32 { return this.id; }
    /** Reads a static error message. */ const method message() static_ptr<u8> { return "tracked"; }
  }
  /** Provides a different matching tag. */
  class Other implements IError {
    /** Creates the error. */ init() {}
    /** Reads the error code. */ method code() i32 { return 99; }
    /** Reads a static message. */ method message() static_ptr<u8> { return "other"; }
  }
  /** Moves an error into runtime ownership. */
  function fail() void throws IError { throw Tracked(7); }
)";

/** Executes a handler and checks cleanup after it exits. */
sun::driver::SunValue runHandler(const std::string& body) {
  return sun::driver::executeString(trackedError +
                                    "/** Exercises a handler exit. */ function "
                                    "exercise() void throws IError {" +
                                    body + R"(
      }
      /** Observes cleanup after exercise has completed. */
      function main() i32 {
        try { exercise(); } catch (unexpected: ref IError) { return -1; }
        return dropped * 100 + fields_dropped * 10 + order;
      }
    )");
}
}  // namespace

/** Every ordinary catch exit drops the original error and its field once. */
TEST(Errors_Ownership, handler_exit_paths_drop_payload) {
  for (const auto* body : {
           "try { fail(); } catch (e: ref Tracked) { e.id = 8; }",
           "try { fail(); } catch (e: const ref Tracked) { if (e.code() != 7) "
           "{ throw Other(); } }",
           "try { fail(); } catch (e: ref IError) { if (dropped != 0) { throw "
           "Other(); } }",
           "try { fail(); } catch (e: ref Tracked) { return; }",
           "try { fail(); } catch (e: ref Tracked) { if (e.code() == 7) { "
           "return; } }",
           "while (true) { try { fail(); } catch (e: ref Tracked) { break; } }",
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
      try { fail(); } catch (e: ref Tracked) { continue; }
    }
  )"),
            1432);
}

/** Rethrows retain the concrete tag and mutated payload without copying. */
TEST(Errors_Ownership, rethrow_preserves_type_and_single_owner) {
  for (const auto* body : {
           "try { try { fail(); } catch (e: ref Tracked) { e.id = 42; throw e; "
           "} } catch (e: ref Tracked) { if (e.id != 42 or dropped != 0) { "
           "throw Other(); } }",
           "try { try { fail(); } catch (e: ref IError) { throw e; } } catch "
           "(e: ref Tracked) { if (e.id != 7 or dropped != 0) { throw Other(); "
           "} }",
           "try { try { fail(); } catch (e: const ref Tracked) { throw e; } } "
           "catch (e: ref Tracked) { e.id = 42; }",
           "try { try { fail(); } catch (e: ref Other) {} } catch (e: ref "
           "Tracked) { if (dropped != 0) { throw Other(); } }",
       }) {
    SCOPED_TRACE(body);
    EXPECT_EQ(runHandler(body), 122);
  }
}

/** Throwing a different error or calling a throwing function releases the old
 * error. */
TEST(Errors_Ownership, errors_leaving_handlers_drop_both_payloads) {
  for (const auto* body : {
           "try { try { fail(); } catch (e: ref Tracked) { throw Tracked(8); } "
           "} catch (e: ref Tracked) { if (dropped != 1 or e.id != 8) { throw "
           "Other(); } }",
           "try { try { fail(); } catch (e: ref Tracked) { fail(); } } catch "
           "(e: ref Tracked) { if (dropped != 1) { throw Other(); } }",
           "try { fail(); } catch (outer: ref Tracked) { try { fail(); } catch "
           "(inner: ref Tracked) {} if (dropped != 1 or outer.id != 7) { throw "
           "Other(); } }",
       }) {
    SCOPED_TRACE(body);
    EXPECT_EQ(runHandler(body), 1432);
  }
}

/** Handler locals are destroyed before the exception they may borrow. */
TEST(Errors_Ownership, handler_locals_drop_before_exception) {
  EXPECT_EQ(runHandler("try { fail(); } catch (e: ref Tracked) { var local = "
                       "Payload(); return; }"),
            332);
}

/** Catch bindings cannot copy, escape, or impersonate a different active
 * exception. */
TEST(Errors_Ownership, invalid_catch_ownership_is_rejected) {
  for (const auto* body : {
           "/** Rejects a value catch. */ function main() i32 { try { fail(); "
           "} catch (e: Tracked) {} return 0; }",
           "/** Rejects an erased value catch. */ function main() i32 { try { "
           "fail(); } catch (e: IError) {} return 0; }",
       }) {
    SCOPED_TRACE(body);
    EXPECT_SUN_ERROR_WITH_MESSAGE(
        sun::driver::executeString(trackedError + body),
        "Catch bindings must use ref or const ref");
  }
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(trackedError + R"(
    /** Attempts to escape the handler. */
    function bad() ref Tracked { try { fail(); } catch (e: ref Tracked) { return e; } var local = Tracked(0); return local; }
    /** Supplies an entry point. */ function main() i32 { return 0; }
  )"),
                                "dangling reference");
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(trackedError + R"(
    /** Attempts to mutate a const catch. */
    function main() i32 { try { fail(); } catch (e: const ref Tracked) { e.id = 3; } return 0; }
  )"),
                                "const");
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(trackedError + R"(
    /** Attempts to throw an ordinary borrow. */
    function bad(e: ref Tracked) void throws IError { throw e; }
    /** Supplies an entry point. */ function main() i32 { return 0; }
  )"),
                                "Cannot throw a borrowed error");
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(trackedError + R"(
    /** Attempts to rethrow an outer error while handling a different one. */
    function bad() void throws IError {
      try { fail(); } catch (outer: ref Tracked) {
        try { fail(); } catch (inner: ref Tracked) { throw outer; }
      }
    }
    /** Supplies an entry point. */ function main() i32 { return 0; }
  )"),
                                "innermost catch binding");
}
