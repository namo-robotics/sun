// tests/stdlib/concurrency/test_threads.cpp - Tests for OS threads
//
// `spawn` and `Thread<T>` are stdlib/thread.sun, not compiler builtins, so
// every source here loads the standard library and says `using std.thread;`.
// What is still the compiler's is the pthread trampoline behind the _spawn /
// _thread_join intrinsics.

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"
#include "support/error.h"

// ============================================================================
// The shape of a spawn call
// ============================================================================

TEST(Stdlib_Concurrency_Threads, parse_spawn_lambda) {
  // Just verify it compiles without runtime execution
  EXPECT_NO_THROW(compileStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn(() => i32 { return 42; });
      return 0;
    }
  )"));
}

TEST(Stdlib_Concurrency_Threads, parse_spawn_with_captures) {
  EXPECT_NO_THROW(compileStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x: i32 = 10;
      var t = spawn(() => i32 { return x + 1; });
      return 0;
    }
  )"));
}

// ============================================================================
// Semantic Analysis Tests
// ============================================================================

// `spawn` is an ordinary generic function now, so what it will accept is its
// `<F: _Callable>` constraint rather than a rule of its own.
TEST(Stdlib_Concurrency_Threads, spawn_requires_a_callable) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x: i32 = 42;
      var t = spawn(x);
      return 0;
    }
  )"),
                                "does not satisfy constraint '_Callable'");
}

// A named function is a callable value and spawns like a lambda.
TEST(Stdlib_Concurrency_Threads, spawn_accepts_a_named_function) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;

    function double_it(n: i32) i32 {
        return n * 2;
    }

    function main() i32 {
        var t = spawn(double_it, 21);
        return t.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

// A function-pointer value rides through spawn's argument pack like any
// other one-word value — this is exactly how the test runner passes each
// test to std.test.run_one.
TEST(Stdlib_Concurrency_Threads, spawn_passes_function_pointer_arguments) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;

    function double_it(n: i32) i32 {
        return n * 2;
    }

    function apply(f: function (i32) i32, n: i32) i32 {
        return f(n);
    }

    function main() i32 {
        var t = spawn(apply, double_it, 21);
        return t.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

// The thread trampoline has no unwind handling, so anything that could
// throw across the spawn boundary is rejected up front — lambdas and named
// functions alike.
TEST(Stdlib_Concurrency_Threads, spawn_rejects_a_throwing_function) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileStringWithStdlib(R"(
    using std;
    using std.thread;

    function explode() void throws IError {
        throw Error(1, "boom");
    }

    function main() i32 {
        var t = spawn(explode);
        return 0;
    }
  )"),
                                "a spawned function must not throw");
}

// _is answers the same question the constraint asks: a named-function value
// is _Function and _Callable, but not _Lambda.
TEST(Stdlib_Concurrency_Threads, function_values_satisfy_function_traits) {
  auto value = executeString(R"(
    function double_it(n: i32) i32 {
        return n * 2;
    }

    function main() i32 {
        var f: function (i32) i32 = double_it;
        var l = (n: i32) => i32 { return n; };
        if (_is<_Function>(f) == false) { return 1; }
        if (_is<_Callable>(f) == false) { return 2; }
        if (_is<_Lambda>(f)) { return 3; }
        if (_is<_Function>(l)) { return 4; }
        if (_is<_Callable>(l) == false) { return 5; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// A lambda with parameters is fine; leaving them unfilled is not. The pack
// stands for exactly the parameters the lambda declares.
TEST(Stdlib_Concurrency_Threads, spawn_missing_argument_is_an_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn((x: i32) => i32 { return x; });
      return 0;
    }
  )"),
                                "which takes (i32); got ()");
}

TEST(Stdlib_Concurrency_Threads, spawn_extra_argument_is_an_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn(() => i32 { return 1; }, 5);
      return 0;
    }
  )"),
                                "which takes (); got (i32)");
}

// Thread<T> is an ordinary class now, so a handle can be written out rather
// than only inferred from what spawn returned.
TEST(Stdlib_Concurrency_Threads, thread_handle_can_be_written_as_a_type) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t: Thread<i32> = spawn(() => i32 { return 42; });
      return t.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Type Inference Tests
// ============================================================================

TEST(Stdlib_Concurrency_Threads, thread_type_inferred) {
  EXPECT_NO_THROW(compileStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn(() => i64 { return 100; });
      // t should be inferred as Thread<i64>
      return 0;
    }
  )"));
}

// ============================================================================
// Runtime Tests (Basic)
// ============================================================================

TEST(Stdlib_Concurrency_Threads, spawn_and_join_basic) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn(() => i32 { return 42; });
      return t.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Concurrency_Threads, spawn_with_captured_value) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x: i32 = 10;
      var t = spawn(() => i32 { return x * 2; });
      return t.join();
    }
  )");
  EXPECT_EQ(value, 20);
}

// Issue #99: a void lambda has no result slot; join returns nothing
TEST(Stdlib_Concurrency_Threads, spawn_void_lambda_and_join) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn(() => void { });
      t.join();
      return 42;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Concurrency_Threads, spawn_void_lambda_without_join_compiles) {
  EXPECT_NO_THROW(compileStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      spawn(() => void { });
      return 0;
    }
  )"));
}

// Issue #99: a lambda held in a variable arrives as a fat struct value
TEST(Stdlib_Concurrency_Threads, spawn_lambda_variable) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var f = () => i32 { return 7; };
      var t = spawn(f);
      return t.join() * 6;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Concurrency_Threads, spawn_lambda_variable_with_capture) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x: i32 = 10;
      var f = () => i32 { return x * 2; };
      var t = spawn(f);
      return t.join() + 1;
    }
  )");
  EXPECT_EQ(value, 21);
}

TEST(Stdlib_Concurrency_Threads, multiple_threads) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t1 = spawn(() => i32 { return 10; });
      var t2 = spawn(() => i32 { return 20; });
      var t3 = spawn(() => i32 { return 30; });
      return t1.join() + t2.join() + t3.join();
    }
  )");
  EXPECT_EQ(value, 60);
}

// ============================================================================
// Scoped threads (issue #122): shared state through a by-ref capture
// ============================================================================

// The thread writes through the capture; the parent reads it after joining
TEST(Stdlib_Concurrency_Threads, byref_capture_shares_a_class) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    class Counter {
      public var n: i32;
      init() { this.n = 0; }
    }
    function main() i32 {
      var c = Counter();
      var t = spawn([ref c]() => i32 {
        var i: i32 = 0;
        while (i < 1000) { c.n = c.n + 1; i = i + 1; }
        return 0;
      });
      var r = t.join();
      return c.n;
    }
  )");
  EXPECT_EQ(value, 1000);
}

// A thread nobody joined by hand is joined when its handle's scope ends, so
// its writes are complete and its captures were alive throughout
TEST(Stdlib_Concurrency_Threads, unjoined_thread_joins_at_scope_exit) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    class Counter {
      public var n: i32;
      init() { this.n = 0; }
    }
    function main() i32 {
      var c = Counter();
      if (true) {
        var t = spawn([ref c]() => i32 {
          var i: i32 = 0;
          while (i < 100000) { c.n = c.n + 1; i = i + 1; }
          return 0;
        });
      }
      return c.n;
    }
  )");
  EXPECT_EQ(value, 100000);
}

// A return between spawn and join leaves the scope, so the join happens there
TEST(Stdlib_Concurrency_Threads, unjoined_thread_joins_on_early_return) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    class Counter {
      public var n: i32;
      init() { this.n = 0; }
    }
    function run() i32 {
      var c = Counter();
      var t = spawn([ref c]() => i32 {
        var i: i32 = 0;
        while (i < 100000) { c.n = c.n + 1; i = i + 1; }
        return 0;
      });
      return 5;
    }
    function main() i32 {
      return run();
    }
  )");
  EXPECT_EQ(value, 5);
}

// Joining by hand leaves nothing for the scope exit to join
TEST(Stdlib_Concurrency_Threads, explicit_join_is_not_repeated_at_scope_exit) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x: i32 = 1;
      var t = spawn([ref x]() => i32 { x = 2; return 40; });
      var r = t.join();
      return r + x;
    }
  )");
  EXPECT_EQ(value, 42);
}

// A handle is a value like any other: binding it to a second name moves it,
// so exactly one name still joins the thread
TEST(Stdlib_Concurrency_Threads, handle_moves_to_a_new_variable) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn(() => i32 { return 11; });
      var t2 = t;
      return t2.join();
    }
  )");
  EXPECT_EQ(value, 11);
}

TEST(Stdlib_Concurrency_Threads, moved_handle_cannot_be_joined) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn(() => i32 { return 11; });
      var t2 = t;
      return t.join();
    }
  )"),
               SunError);
}

// Overwriting a handle joins the thread it held first
TEST(Stdlib_Concurrency_Threads, reassigned_handle_joins_previous_thread) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn(() => i32 { return 1; });
      t = spawn(() => i32 { return 2; });
      return t.join();
    }
  )");
  EXPECT_EQ(value, 2);
}

// An exception unwinding out of the scope joins the thread on its way past
TEST(Stdlib_Concurrency_Threads, unjoined_thread_joins_while_unwinding) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.thread;
    class Boom implements IError {
      init() {}
      method code() i32 { return 1; }
      method message() String { return String("boom"); }
    }
    class Counter {
      public var n: i32;
      init() { this.n = 0; }
    }
    function fail() i32 throws IError { throw Boom(); }
    function main() i32 {
      var c = Counter();
      try {
        var t = spawn([ref c]() => i32 {
          var i: i32 = 0;
          while (i < 100000) { c.n = c.n + 1; i = i + 1; }
          return 0;
        });
        var r = fail();
      } catch (e: IError) {
        return c.n;
      };
      return 0;
    }
  )");
  EXPECT_EQ(value, 100000);
}

// A mutable capture is exclusive, as it is for any two lambdas: two threads
// cannot yet share one object they both write (issue #122 follow-up)
TEST(Stdlib_Concurrency_Threads, two_threads_cannot_capture_one_ref) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x: i32 = 0;
      var t1 = spawn([ref x]() => i32 { x = 1; return 0; });
      var t2 = spawn([ref x]() => i32 { x = 2; return 0; });
      return t1.join() + t2.join();
    }
  )"),
               SunError);
}

// A [const ref x] capture only reads, so it is a shared loan: any number of
// threads can hold one at once
TEST(Stdlib_Concurrency_Threads, two_threads_can_capture_one_const_ref) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x: i32 = 0;
      var t1 = spawn([const ref x]() => i32 { return x + 1; });
      var t2 = spawn([const ref x]() => i32 { return x + 2; });
      return t1.join() + t2.join();
    }
  )");
  EXPECT_EQ(value, 3);
}

// A class is shared read-only the same way
TEST(Stdlib_Concurrency_Threads, threads_share_a_class_by_const_ref) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    class Config {
      public var limit: i32;
      init(limit: i32) { this.limit = limit; }
    }
    function main() i32 {
      var cfg = Config(20);
      var t1 = spawn([const ref cfg]() => i32 { return cfg.limit; });
      var t2 = spawn([const ref cfg]() => i32 { return cfg.limit; });
      return t1.join() + t2.join();
    }
  )");
  EXPECT_EQ(value, 40);
}

// The capture is read-only even though the variable is not
TEST(Stdlib_Concurrency_Threads, const_ref_capture_cannot_be_written) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x: i32 = 0;
      var t = spawn([const ref x]() => i32 { x = 1; return 0; });
      return t.join();
    }
  )"),
               SunError);
}

// A shared capture and a mutable one still conflict
TEST(Stdlib_Concurrency_Threads, const_ref_and_ref_captures_conflict) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x: i32 = 0;
      var t1 = spawn([ref x]() => i32 { x = 1; return 0; });
      var t2 = spawn([const ref x]() => i32 { return x; });
      return t1.join() + t2.join();
    }
  )"),
               SunError);
}
// ============================================================================
// Thread results that own resources
// ============================================================================

// A thread returning a class hands its result to the joiner: the result slot
// is raw memory, so freeing it releases the result's own bytes and nothing
// they point at. The joiner owns what it took, and drops it once.
TEST(Stdlib_Concurrency_Threads, joined_class_result_is_owned_by_the_joiner) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    var dropped: i32 = 0;

    class Res {
      public var v: i32;
      init(v: i32) { this.v = v; }
      deinit() { dropped = dropped + 1; }
    }

    function run() i32 {
      var t = spawn(() => Res { return Res(7); });
      var r = t.join();
      return r.v;
    }

    function main() i32 {
      var v = run();
      return v * 10 + dropped;
    }
  )");
  EXPECT_EQ(value, 71);
}

// Nobody takes the result of a thread joined at scope exit, so its deinit
// runs there rather than the result being freed as raw bytes
TEST(Stdlib_Concurrency_Threads, unjoined_class_result_is_dropped) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    var dropped: i32 = 0;

    class Res {
      public var v: i32;
      init(v: i32) { this.v = v; }
      deinit() { dropped = dropped + 1; }
    }

    function run() void {
      var t = spawn(() => Res { return Res(7); });
    }

    function main() i32 {
      run();
      return dropped;
    }
  )");
  EXPECT_EQ(value, 1);
}

// The same holds for a class that owns heap memory: one release, no double
// free, whether the result is taken or dropped at scope exit
TEST(Stdlib_Concurrency_Threads, class_result_owning_heap_is_released_once) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    class Buffer {
      public var v: i32;
      public var data: raw_ptr<i8>;
      init(v: i32) {
        this.v = v;
        var size: i64 = 8;
        this.data = unsafe { _malloc(size); };
      }
      public method get() i32 { return this.v; }
      deinit() {
        if (this.data != null) {
          unsafe { _free(this.data); };
          this.data = null;
        }
      }
    }

    function taken() i32 {
      var t = spawn(() => Buffer { return Buffer(9); });
      var b = t.join();
      return b.get();
    }

    function dropped_at_scope_exit() void {
      var t = spawn(() => Buffer { return Buffer(4); });
    }

    function main() i32 {
      var v = taken();
      dropped_at_scope_exit();
      return v;
    }
  )");
  EXPECT_EQ(value, 9);
}

// An unjoined thread whose result is a payload enum drops the payload too
TEST(Stdlib_Concurrency_Threads, unjoined_enum_result_drops_its_payload) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    var dropped: i32 = 0;

    class Res {
      public var v: i32;
      init(v: i32) { this.v = v; }
      deinit() { dropped = dropped + 1; }
    }

    enum Maybe { Some(Res), Nothing }

    function run() void {
      var t = spawn(() => Maybe { return Maybe.Some(Res(7)); });
    }

    function main() i32 {
      run();
      return dropped;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Arguments: what the thread is given to work on
// ============================================================================
// A thread body no longer has to capture what it works on. spawn forwards its
// arguments to the lambda, moving them in, so the thread owns them from the
// moment it starts.

TEST(Stdlib_Concurrency_Threads, spawn_forwards_one_argument) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn((n: i32) => i32 { return n * 2; }, 21);
      return t.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Stdlib_Concurrency_Threads, spawn_forwards_several_arguments) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn((a: i32, b: i64, c: bool) => i32 {
        if (c) { return a + _convert<i32>(b); }
        return 0;
      }, 40, 2, true);
      return t.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

// A narrower literal widens to the parameter, the same as at any other call.
TEST(Stdlib_Concurrency_Threads, spawn_widens_a_narrow_argument) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var t = spawn((n: i64) => i64 { return n * 2; }, 21);
      return _convert<i32>(t.join());
    }
  )");
  EXPECT_EQ(value, 42);
}

// A lambda kept in a variable can be spawned as often as you like, with
// different arguments each time — nothing about it is per-thread.
TEST(Stdlib_Concurrency_Threads, one_lambda_spawned_twice) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var work = (n: i32) => i32 { return n * 2; };
      var a = spawn(work, 20);
      var b = spawn(work, 1);
      return a.join() + b.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

// A global lambda captures nothing — globals are read directly — so it is the
// natural way to write a thread body that takes what it needs as arguments.
TEST(Stdlib_Concurrency_Threads, spawn_a_global_lambda) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    var work = (n: i32) => i32 { return n * 2; };
    function main() i32 {
      var t = spawn(work, 21);
      return t.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Arguments the thread takes ownership of
// ============================================================================

// A compound argument moves into the thread. Rebinding it inside the body
// makes it a local the body owns, so it is released when the thread ends —
// a by-value parameter left alone is nobody's to drop.
TEST(Stdlib_Concurrency_Threads, class_argument_moves_into_the_thread) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    var drops: i32 = 0;
    class Res {
      public var v: i32;
      init(v: i32) { this.v = v; }
      deinit() {
        if (this.v != 0) { drops = drops + 1; this.v = 0; }
      }
    }
    var take = (r: Res) => i32 {
      var owned = r;
      return owned.v;
    };
    function main() i32 {
      var got: i32 = 0;
      if (true) {
        var r = Res(7);
        var t = spawn(take, r);
        got = t.join();
      }
      return got * 10 + drops;
    }
  )");
  // 7 came back, and the clone was released exactly once — inside the thread
  EXPECT_EQ(value, 71);
}

// What the thread took over cannot be used by the spawner afterwards.
TEST(Stdlib_Concurrency_Threads, class_argument_cannot_be_used_after_spawn) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    class Res {
      public var v: i32;
      init(v: i32) { this.v = v; }
      deinit() { }
    }
    var take = (r: Res) => i32 { var o = r; return o.v; };
    function main() i32 {
      var r = Res(42);
      var t = spawn(take, r);
      return r.v;
    }
  )"),
                                "Borrow check failed");
}

// ============================================================================
// A thread over borrowed locals must not outlive the frame it borrows from
// ============================================================================

// The handle spawn returns keeps the lambda - and so its pointers into this
// frame - alive until the join. Storing it in a field would let the object
// carry it past the frame's death, so that is rejected.
TEST(Stdlib_Concurrency_Threads, ref_capturing_thread_cannot_be_stored_in_a_field) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    class Worker {
      var t: Thread<i32>;
      init() {
        var x = 3;
        this.t = spawn([ref x]() => i32 { return x; });
      }
    }
    function main() i32 {
      var w = Worker();
      return 0;
    }
  )"),
                                "Borrow check failed");
}

// An owned capture pins the thread to the frame just as a borrow does: the
// closure's environment - where the owned value lives - is frame storage.
TEST(Stdlib_Concurrency_Threads, owned_capture_thread_cannot_be_stored_in_a_field) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    class Worker {
      var t: Thread<i32>;
      init() {
        var x = 3;
        this.t = spawn([x]() => i32 { return x; });
      }
    }
    function main() i32 {
      var w = Worker();
      return 0;
    }
  )"),
                                "Borrow check failed");
}

// Returning the handle is the same escape without the object in between.
TEST(Stdlib_Concurrency_Threads, ref_capturing_thread_cannot_be_returned) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    function makeThread() Thread<i32> {
      var x = 3;
      return spawn([ref x]() => i32 { return x; });
    }
    function main() i32 {
      var t = makeThread();
      return t.join();
    }
  )"),
                                "Borrow check failed");
}

// Passing the handle through a local first changes nothing: the local is
// frame-bound too.
TEST(Stdlib_Concurrency_Threads,
     ref_capturing_thread_cannot_be_returned_via_a_variable) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    function makeThread() Thread<i32> {
      var x = 3;
      var t = spawn([ref x]() => i32 { return x; });
      return t;
    }
    function main() i32 {
      var t = makeThread();
      return t.join();
    }
  )"),
                                "Borrow check failed");
}

// The legitimate scoped use is untouched: borrow, spawn, join, all in one
// frame.
TEST(Stdlib_Concurrency_Threads, ref_capturing_thread_joined_in_frame_is_fine) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x = 40;
      var t = spawn([ref x]() => i32 { return x + 2; });
      return t.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

// A thread with no captures owns everything it touches, so a constructor may
// start it and store the handle in a field: the object joins it on drop.
TEST(Stdlib_Concurrency_Threads, capture_free_thread_may_live_in_a_field) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    class Worker {
      var t: Thread<i32>;
      init() {
        this.t = spawn(() => i32 { return 7; });
      }
      public method take() i32 { return this.t.join(); }
    }
    function main() i32 {
      var w = Worker();
      return w.take() + 35;
    }
  )");
  EXPECT_EQ(value, 42);
}

// A bound method holds its receiver by reference, so spawning it works in
// the receiver's frame - and, like any borrowing thread, no further.
TEST(Stdlib_Concurrency_Threads, bound_method_thread_joined_in_frame_is_fine) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    class Counter {
      var count: i32;
      init() { this.count = 40; }
      public method get(extra: i32) i32 { return this.count + extra; }
    }
    function main() i32 {
      var c = Counter();
      var t = spawn(c.get, 2);
      return t.join();
    }
  )");
  EXPECT_EQ(value, 42);
}

// A thread over a bound method points into the receiver, so the handle must
// not leave the receiver's frame either.
TEST(Stdlib_Concurrency_Threads, bound_method_thread_cannot_be_returned) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    class Counter {
      var count: i32;
      init() { this.count = 40; }
      public method get() i32 { return this.count; }
    }
    function makeThread() Thread<i32> {
      var c = Counter();
      return spawn(c.get);
    }
    function main() i32 {
      var t = makeThread();
      return t.join();
    }
  )"),
                                "Borrow check failed");
}

// A class runs its own method on a thread and hands the result to a caller-
// supplied callback once the thread is done: bind the method (`this.work`)
// as the thread body, join, then call the lambda the caller passed in.
TEST(Stdlib_Concurrency_Threads, method_on_a_thread_reports_to_a_callback) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    class Job {
      var base: i32;
      init(base: i32) { this.base = base; }
      // The work that runs on the thread.
      public method work() i32 { return this.base * 2; }
      // Run work() on a thread; when it finishes, hand what it returned to
      // the callback and pass the callback's answer on.
      public method runThen(callback: (i32) => i32) i32 {
        var t = spawn(this.work);
        var result = t.join();
        return callback(result);
      }
    }
    function main() i32 {
      var j = Job(20);
      return j.runThen((r: i32) => i32 { return r + 2; });
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// A frame-bound handle must not cross a call boundary by value (issue #160)
// ============================================================================

// The issue's reproduction: a container element was the one unchecked
// escape route. The push itself is now rejected - the callee could keep the
// handle past this frame's death, and nothing in its parameter type says so.
TEST(Stdlib_Concurrency_Threads, frame_bound_handle_cannot_be_pushed_into_a_vec) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std;
    using std.thread;
    function makeThreads(alloc: const ref HeapAllocator) Vec<Thread<i32>> {
      var x = 3;
      var v = Vec<Thread<i32>>(alloc, 4);
      v.push(spawn([ref x]() => i32 { return x; }));
      return v;
    }
    function main() i32 {
      var alloc = make_heap_allocator();
      var v = makeThreads(alloc);
      match (v.pop()) {
        Option.Some(t) => { return t.join(); },
        Option.None => { return -1; },
      };
      return 0;
    }
  )"),
                                "Borrow check failed");
}

// Handles over capture-free lambdas own everything they touch, so a pool of
// them may live in a container and leave the frame with it.
TEST(Stdlib_Concurrency_Threads, capture_free_handles_may_leave_in_a_vec) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.thread;
    function makeThreads(alloc: const ref HeapAllocator) Vec<Thread<i32>> {
      var v = Vec<Thread<i32>>(alloc, 4);
      v.push(spawn(() => i32 { return 20; }));
      v.push(spawn(() => i32 { return 22; }));
      return v;
    }
    function main() i32 {
      var alloc = make_heap_allocator();
      var v = makeThreads(alloc);
      var total = 0;
      match (v.pop()) {
        Option.Some(t) => { total = total + t.join(); },
        Option.None => { return -1; },
      };
      match (v.pop()) {
        Option.Some(t) => { total = total + t.join(); },
        Option.None => { return -1; },
      };
      return total;
    }
  )");
  EXPECT_EQ(value, 42);
}

// The same rejection for a plain free function: the borrow checker cannot
// see what the callee does with a by-value argument, so a frame-bound one
// must stay in this frame.
TEST(Stdlib_Concurrency_Threads, frame_bound_handle_cannot_pass_by_value) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    function consume(t: Thread<i32>) i32 {
      return t.join();
    }
    function main() i32 {
      var x = 3;
      var t = spawn([ref x]() => i32 { return x; });
      return consume(t);
    }
  )"),
                                "Borrow check failed");
}

// Passing by ref stays legal: a borrow cannot be kept by the callee.
TEST(Stdlib_Concurrency_Threads, frame_bound_handle_may_pass_by_ref) {
  auto value = executeStringWithStdlib(R"(
    using std.thread;
    function ticket(t: ref Thread<i32>) i32 {
      return 2;
    }
    function main() i32 {
      var x = 40;
      var t = spawn([ref x]() => i32 { return x; });
      var extra = ticket(t);
      return t.join() + extra;
    }
  )");
  EXPECT_EQ(value, 42);
}

// A constructor is a call like any other: the new object could carry the
// handle anywhere.
TEST(Stdlib_Concurrency_Threads, frame_bound_handle_cannot_enter_a_constructor) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    class Box {
      var t: Thread<i32>;
      init(t: Thread<i32>) { this.t = t; }
    }
    function main() i32 {
      var x = 3;
      var t = spawn([ref x]() => i32 { return x; });
      var b = Box(t);
      return 0;
    }
  )"),
                                "Borrow check failed");
}

// An enum payload is a by-value parameter too: wrapping the handle would
// let the enum value smuggle it onward.
TEST(Stdlib_Concurrency_Threads, frame_bound_handle_cannot_enter_an_enum_payload) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std;
    using std.thread;
    function main() i32 {
      var x = 3;
      var t = spawn([ref x]() => i32 { return x; });
      var o = Option.Some(t);
      return 0;
    }
  )"),
                                "Borrow check failed");
}

// A call through a lambda value is covered as well.
TEST(Stdlib_Concurrency_Threads, frame_bound_handle_cannot_pass_to_a_lambda) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x = 3;
      var sink = (t: Thread<i32>) => i32 { return t.join(); };
      var t = spawn([ref x]() => i32 { return x; });
      return sink(t);
    }
  )"),
                                "Borrow check failed");
}

// An indexed slot is a container element by another name.
TEST(Stdlib_Concurrency_Threads, frame_bound_handle_cannot_enter_an_indexed_slot) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      var x = 3;
      var arr = [spawn(() => i32 { return 0; })];
      arr[0] = spawn([ref x]() => i32 { return x; });
      return 0;
    }
  )"),
                                "Borrow check failed");
}

// An array literal built from a frame-bound handle is frame-bound as a
// whole, so it cannot be returned either.
TEST(Stdlib_Concurrency_Threads, array_literal_with_frame_bound_handle_cannot_return) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    function make() array<Thread<i32>, 1> {
      var x = 3;
      var a = [spawn([ref x]() => i32 { return x; })];
      return a;
    }
    function main() i32 {
      return 0;
    }
  )"),
                                "Borrow check failed");
}

// A module-level global outlives every frame, so a frame-bound handle
// cannot be parked there even though its type is clean.
TEST(Stdlib_Concurrency_Threads, frame_bound_handle_cannot_be_assigned_to_a_global) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    var gt = spawn(() => i32 { return 0; });
    function park() void {
      var x = 3;
      gt = spawn([ref x]() => i32 { return x; });
      return;
    }
    function main() i32 {
      park();
      return 0;
    }
  )"),
                                "Borrow check failed");
}
