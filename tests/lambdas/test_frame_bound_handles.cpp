// tests/lambdas/test_frame_bound_handles.cpp - Values bound to a frame by
// what was moved into them
//
// A lambda with captures is bound to the frame its environment lives in, and
// test_ref_lambda_types.cpp covers how that shows in its type. This file
// covers the other half of the rule: a value such a lambda was moved into is
// bound to the same frame even though nothing in its own type says so. The
// borrow checker tracks that by provenance (borrow_checker.cpp,
// forbidFrameBoundByValueArgs and the return check next to it), and the one
// thing that produces such a value is std.thread.spawn - its Thread<T> handle
// keeps the lambda alive until the join. So every program here spawns.
//
// The runtime side (a borrowing thread joined in its own frame is fine, a
// capture-free handle may go anywhere) is in stdlib/thread_tests.sun.

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

// ============================================================================
// A handle over borrowed locals must not outlive the frame it borrows from
// ============================================================================

// Storing the handle in a field would let the object carry it past the
// frame's death.
TEST(Lambdas_FrameBoundHandles,
     ref_capturing_thread_cannot_be_stored_in_a_field) {
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
// closure's environment, where the owned value lives, is frame storage.
TEST(Lambdas_FrameBoundHandles,
     owned_capture_thread_cannot_be_stored_in_a_field) {
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
TEST(Lambdas_FrameBoundHandles, ref_capturing_thread_cannot_be_returned) {
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
TEST(Lambdas_FrameBoundHandles,
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
TEST(Lambdas_FrameBoundHandles, bound_method_thread_cannot_be_returned) {
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
// escape route. The push itself is rejected - the callee could keep the
// handle past this frame's death, and nothing in its parameter type says so.
TEST(Lambdas_FrameBoundHandles,
     frame_bound_handle_cannot_be_pushed_into_a_vec) {
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
TEST(Lambdas_FrameBoundHandles, frame_bound_handle_cannot_pass_by_value) {
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
TEST(Lambdas_FrameBoundHandles, frame_bound_handle_cannot_enter_a_constructor) {
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
TEST(Lambdas_FrameBoundHandles,
     frame_bound_handle_cannot_enter_an_enum_payload) {
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
TEST(Lambdas_FrameBoundHandles, frame_bound_handle_cannot_pass_to_a_lambda) {
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
TEST(Lambdas_FrameBoundHandles,
     frame_bound_handle_cannot_enter_an_indexed_slot) {
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
TEST(Lambdas_FrameBoundHandles,
     array_literal_with_frame_bound_handle_cannot_return) {
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
TEST(Lambdas_FrameBoundHandles,
     frame_bound_handle_cannot_be_assigned_to_a_global) {
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
