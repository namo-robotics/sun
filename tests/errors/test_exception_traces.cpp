/** Checks trace extraction, borrowing, reporting, and independent runtime
 * depths. */
#include "source_exception_test.h"

/** Catch metadata is exposed through intrinsics without any standard-library
 * dependency. */
TEST_P(Errors_SourceExceptions, borrowed_trace_survives_nested_catch) {
  testing::internal::CaptureStderr();
  auto result = run(R"(
/** Carries a failure code. */
exception Failure { var code: i32; }
/** Prints borrowed addresses through an ordinary helper. */
function show(frames: const ref array<u64>, status: u32) void {
  unsafe { print_trace(frames, status); };
}
/** Keeps an outer trace live while another exception is caught. */
function main() i32 {
  try { var error: Failure = {code: 42}; throw error; }
  catch (error: Failure) {
    var frames: const ref array<u64> = _exception_trace_frames(error);
    var status = _exception_trace_status(error);
    try { var nested: Failure = {code: 7}; throw nested; }
    catch (nested: Failure) { show(frames, status); }
    return error.code;
  }
  return 0;
}

  )");
  auto output = testing::internal::GetCapturedStderr();
  EXPECT_EQ(result, 42);
  EXPECT_NE(output.find("Exception trace\n"), std::string::npos);
  EXPECT_EQ(output.find("Exception trace", output.find("Exception trace") + 1),
            std::string::npos);
}

/** An ordinary exception object has no trace metadata to borrow. */
TEST_P(Errors_SourceExceptions, trace_rejects_unthrown_value) {
  EXPECT_THROW(run(R"(
    /** Carries an unthrown value. */
    exception Failure { var code: i32; }
    /** Attempts to borrow metadata that does not exist. */
    function main() i32 {
      var error: Failure = {code: 1};
      var frames: const ref array<u64> = _exception_trace_frames(error);
      return 0;
    }
  )"),
               sun::support::SunError);
}

/** A trace cannot retain a reference to catch storage after the function
 * returns. */
TEST_P(Errors_SourceExceptions, trace_rejects_escape_from_catch) {
  EXPECT_THROW(run(R"(
    /** Carries a failure. */
    exception Failure { var code: i32; }
    /** Attempts to return a reference into expired catch storage. */
    function escape() const ref array<u64> {
      try { var error: Failure = {code: 1}; throw error; }
      catch (error: Failure) { return _exception_trace_frames(error); }
    }
    /** Provides the executable entrypoint. */
    function main() i32 { return 0; }
  )"),
               sun::support::SunError);
}

/** Family catches cannot consume their retained value while its trace is still
 * borrowed. */
TEST_P(Errors_SourceExceptions, trace_rejects_use_after_rethrow) {
  EXPECT_THROW(run(R"(
/** Groups related failures. */
exception Failure {}
/** Carries a concrete failure. */
exception Concrete extends Failure { var code: i32; }
/** Keeps a borrow live across a consumed exception. */
function forward() void throws {
  try { var e: Concrete = {code: 42}; throw e; }
  catch (e: Failure) {
    var frames: const ref array<u64> = _exception_trace_frames(e);
    try { throw e; } catch {}
    unsafe { print_trace(frames, 0u32); };
  }
}
/** Handles the result. */
function main() i32 { try { forward(); } catch {} return 0; }

  )"),
               sun::support::SunError);
}

/** The safe writer applies ordinary sized-array borrowing conversions. */
TEST_P(Errors_SourceExceptions, trace_writer_accepts_sized_array) {
  testing::internal::CaptureStderr();
  auto result = run(R"(
    /** Prints caller-owned bounded addresses without interpreting them as pointers. */
    function main() i32 {
      var addresses: array<u64, 2> = [1u64, 2u64];
      unsafe { print_trace(addresses, 0u32); };
      return 42;
    }
  )");
  auto output = testing::internal::GetCapturedStderr();
  EXPECT_EQ(result, 42);
  EXPECT_NE(output.find("  0x2\n  └─ 0x1\n"), std::string::npos);
}

/** Generic intrinsic signatures reject invalid arity before catch-specific
 * lowering. */
TEST_P(Errors_SourceExceptions, trace_intrinsic_declaration_checks_arity) {
  for (const auto* call :
       {"_exception_trace_frames()", "_exception_trace_status(1, 2)"}) {
    try {
      run(std::string("/** Checks a malformed intrinsic call. */ function "
                      "main() i32 { ") +
          call + "; return 0; }");
      FAIL() << "Expected declaration-based argument checking";
    } catch (const sun::support::SunError& error) {
      EXPECT_NE(std::string(error.what()).find("expects 1 arguments, got"),
                std::string::npos);
    }
  }
}

/** Explicit generic arguments use the same registered trace declaration and
 * borrow rules. */
TEST_P(Errors_SourceExceptions,
       trace_intrinsic_accepts_explicit_type_argument) {
  EXPECT_EQ(run(R"(
    /** Carries a failure code. */
    exception Failure { var code: i32; }
    /** Resolves the compiler-owned generic signature explicitly. */
    function main() i32 {
      try { var error: Failure = {code: 42}; throw error; }
      catch (error: Failure) {
        var frames: const ref array<u64> = _exception_trace_frames<Failure>(error);
        var status = _exception_trace_status<Failure>(error);
        if (frames.dim(0) > 0 and status < 4u32) { return error.code; }
      }
      return 0;
    }
  )"),
            42);
}

/** A trace cannot retain a reference to catch storage after the function
 * returns. */
TEST_P(Errors_SourceExceptions, explicit_trace_rejects_escape_from_catch) {
  EXPECT_THROW(run(R"(
    /** Carries a failure. */
    exception Failure { var code: i32; }
    /** Attempts to return a reference into expired catch storage. */
    function escape() const ref array<u64> {
      try { var error: Failure = {code: 1}; throw error; }
      catch (error: Failure) { return _exception_trace_frames<Failure>(error); }
    }
    /** Provides the executable entrypoint. */
    function main() i32 { return 0; }
  )"),
               sun::support::SunError);
}

/** Family catches cannot consume their retained value while its trace is still
 * borrowed. */
TEST_P(Errors_SourceExceptions, explicit_trace_rejects_use_after_rethrow) {
  EXPECT_THROW(run(R"(
/** Groups related failures. */
exception Failure {}
/** Carries a concrete failure. */
exception Concrete extends Failure { var code: i32; }
/** Keeps a borrow live across a consumed exception. */
function forward() void throws {
  try { var e: Concrete = {code: 42}; throw e; }
  catch (e: Failure) {
    var frames: const ref array<u64> = _exception_trace_frames<Failure>(e);
    try { throw e; } catch {}
    unsafe { print_trace(frames, 0u32); };
  }
}
/** Handles the result. */
function main() i32 { try { forward(); } catch {} return 0; }

  )"),
               sun::support::SunError);
}

/** A holder constructed from a returned borrow must keep the catch borrowed. */
TEST_P(Errors_SourceExceptions, trace_holder_rejects_use_after_rethrow) {
  const std::string source = R"(/** Carries a failure. */
exception Failure {}
/** Retains a borrowed frame array. */
class View {
  var frames: const ref array<u64>;
  /** Borrows the frames. */
  init(frames: const ref array<u64>) { this.frames = frames; }
  /** Reads the retained count. */
  public const method count() u64 { return this.frames.dim(0); }
}
/** Must not retain a trace after consuming its catch. */
function main() i32 {
  try { throw Failure(); }
  catch (error: Failure) {
    var view = View(_exception_trace_frames(error));
    try { throw error; } catch {}
    return _convert<i32>(view.count());
  }
  return 0;
}
)";
  EXPECT_THROW(run(source), sun::support::SunError);
  auto explicitCall = source;
  const auto call = explicitCall.find("_exception_trace_frames(error)");
  explicitCall.replace(call,
                       std::string("_exception_trace_frames(error)").size(),
                       "_exception_trace_frames<Failure>(error)");
  EXPECT_THROW(run(explicitCall), sun::support::SunError);
}

/** Simultaneously live JIT instances resolve their own depth and native TLS
 * storage. */
TEST_P(Errors_SourceExceptions, independent_runtime_depths) {
  auto shallow =
      sun::driver::Driver::createForJIT("shallow", false, GetParam(), 1);
  auto disabled =
      sun::driver::Driver::createForJIT("disabled", false, GetParam(), 0);
  auto deep = sun::driver::Driver::createForJIT("deep", false, GetParam(), 256);
  const std::string source = R"(
/** Carries a bounded failure. */
exception Failure { var code: i32; }
/** Adds physical frames before capturing the original trace. */
function fail(depth: i32) void throws {
  if (depth > 0) { fail(depth - 1); }
  var error: Failure = {code: 42};
  throw error;
}
/** Returns the captured frame count, checking disabled status when empty. */
function main() i32 {
  try { fail(4); }
  catch (error: Failure) {
    var frames: const ref array<u64> = _exception_trace_frames(error);
    if (frames.dim(0) == 0 and _exception_trace_status(error) != 4u32) { return -1; }
    return _convert<i32>(frames.dim(0));
  }
  return -1;
}
)";
  EXPECT_EQ(shallow->executeString(source), 1);
  EXPECT_EQ(disabled->executeString(source), 0);
  EXPECT_GT(std::get<std::int32_t>(deep->executeString(source)), 1);
}
