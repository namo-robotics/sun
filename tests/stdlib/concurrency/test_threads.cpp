// tests/stdlib/concurrency/test_threads.cpp - Compile-level tests for OS
// threads: parsing the spawn call, semantic negatives, and the borrow-check
// rules that keep a frame-bound handle in its frame.
//
// `spawn` and `Thread<T>` are stdlib/thread.sun, not compiler builtins, so
// every source here loads the standard library and says `using std.thread;`.
// The runtime behavior tests (spawn/join, captures, thread results,
// forwarded arguments) live in stdlib/thread_tests.sun and run through
// `sun test`.

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

TEST(Stdlib_Concurrency_Threads, spawn_void_lambda_without_join_compiles) {
  EXPECT_NO_THROW(compileStringWithStdlib(R"(
    using std.thread;
    function main() i32 {
      spawn(() => void { });
      return 0;
    }
  )"));
}

// ============================================================================
// A handle owns its thread
// ============================================================================

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
