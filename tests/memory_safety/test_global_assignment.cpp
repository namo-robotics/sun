// Assigning a compound value (class / payload enum) to a global MOVES it:
// the global's old value is dropped once, the source is invalidated so its
// own drop is a no-op, and the global then owns the payload. Regression
// tests for issues #70 and #71 (globals of heap-owning types).

#include <gtest/gtest.h>

#include "driver/execution_utils.h"
#include "support/error.h"

using sun::driver::executeString;
using sun::driver::executeStringWithStdlib;

/** Keeps test fixtures and helpers local to this source file. */
namespace {

// Owner models a heap-owning class: like Unique<T>, deinit is a no-op on
// moved-from (zeroed) storage.
const char* kOwnerPreamble = R"(
    var counter: i32 = 0;

    class Owner {
      var id: i32;
      init(id: i32) {
        this.id = id;
      }
      deinit() {
        if (this.id != 0) {
          counter = counter + 1;
          this.id = 0;
        }
      }
      method get_id() i32 {
        return this.id;
      }
    }
)";

/** Prepends shared fixture declarations to a test program. */
std::string withPreamble(const std::string& body) {
  return std::string(kOwnerPreamble) + body;
}

}  // namespace

TEST(MemorySafety_GlobalAssignment, class_assignment_moves_into_global) {
  auto value = executeString(withPreamble(R"(
    var g: Owner = Owner(1);

    function replace() void {
      var fresh = Owner(7);
      g = fresh;
      // Owner(1) dropped by the overwrite; `fresh` is moved, so its scope
      // exit must not drop Owner(7)
    }

    function main() i32 {
      replace();
      if (counter != 1) {
        return -counter;
      }
      return g.get_id();
    }
  )"));
  EXPECT_EQ(value, 7);
}

TEST(MemorySafety_GlobalAssignment, class_assignment_from_temporary) {
  auto value = executeString(withPreamble(R"(
    var g: Owner = Owner(1);

    function main() i32 {
      g = Owner(9);
      if (counter != 1) {
        return -counter;
      }
      return g.get_id();
    }
  )"));
  EXPECT_EQ(value, 9);
}

TEST(MemorySafety_GlobalAssignment, use_after_move_into_global_rejected) {
  EXPECT_THROW(executeString(withPreamble(R"(
    var g: Owner = Owner(1);

    function main() i32 {
      var fresh = Owner(2);
      g = fresh;
      return fresh.get_id();
    }
  )")),
               sun::support::SunError);
}

// Issue #70: assigning a String to a global used to store the source's
// address over the global's data pointer.
TEST(MemorySafety_GlobalAssignment,
     string_global_assignment_transfers_ownership) {
  auto value = executeStringWithStdlib(R"(
using std;

/** Handles this global initializer’s failure at the startup boundary. */
function initialize_g() String {
  return match String(make_heap_allocator(), "") {
    AllocationResult.Ok(value) => value,
    _ => {
      std.panic("unexpected global initialization failure");
      initialize_g();
    }
  };
}
var g: String = initialize_g();

function replace() Result<void> {
  var a = make_heap_allocator();
  var rebuilt = try String(a, "new contents");
  g = rebuilt;

  return Result.Ok;
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i64> {
  try replace();
  if (unsafe { g.unsafe_at(0); } != 110) {
    // 'n'
    return Result.Ok(-1);
  }
  return Result.Ok(g.length());
}
)");
  EXPECT_EQ(value, 12);
}

// Issue #71: a global String assigned in one function and mutated in place
// afterwards must keep owning its own heap buffer, so Strings built later in
// nested scopes never alias it.
TEST(MemorySafety_GlobalAssignment,
     string_global_queue_pop_keeps_buffers_distinct) {
  auto value = executeStringWithStdlib(R"(
using std;

/** Handles this global initializer’s failure at the startup boundary. */
function initialize_g_queue() String {
  return match String(make_heap_allocator(), "") {
    AllocationResult.Ok(value) => value,
    _ => {
      std.panic("unexpected global initialization failure");
      initialize_g_queue();
    }
  };
}
var g_queue: String = initialize_g_queue();

function append(q: ref String, s: ref String) Result<void> {
  try q.append(s);

  return Result.Ok;
}

function queue_pop(alloc: ref HeapAllocator) Result<String> {
  var out = try String(alloc, "");
  if (g_queue.length() > 0) {
    var nl: i64 = 0;
    while (nl < g_queue.length() and unsafe { g_queue.unsafe_at(nl); } != 10) {
      nl = nl + 1;
    }
    for (var i: i64 = 0; i < nl; i = i + 1) {
      try out.append_char(unsafe { g_queue.unsafe_at(i); });
    }

    var rest = try String(alloc, "");
    for (var i: i64 = nl + 1; i < g_queue.length(); i = i + 1) {
      try rest.append_char(unsafe { g_queue.unsafe_at(i); });
    }
    g_queue.clear();
    try append(g_queue, rest);
  }
  return Result.Ok(out);
}

function fill(alloc: ref HeapAllocator) Result<void> {
  var q = try String(alloc, "");
  g_queue = q;
  var a = try String(alloc, "alpha\n");
  try append(g_queue, a);
  var b = try String(alloc, "beta\n");
  try append(g_queue, b);

  return Result.Ok;
}

/** Runs the fixture with explicit error propagation. */
function main() Result<i64> {
  var alloc = make_heap_allocator();
  try fill(alloc);
  var p1 = try queue_pop(alloc);
  if (p1.length() != 5 or unsafe { p1.unsafe_at(0); } != 97) {
    return Result.Ok(1);  // "alpha"
  }
  if (g_queue.length() != 5 or unsafe { g_queue.unsafe_at(0); } != 98) {
    return Result.Ok(2);  // "beta\n"
  }
  var p2 = try queue_pop(alloc);
  if (p2.length() != 4 or unsafe { p2.unsafe_at(0); } != 98) {
    return Result.Ok(3);  // "beta"
  }
  if (g_queue.length() != 0) {
    return Result.Ok(4);
  }
  var p3 = try queue_pop(alloc);
  if (p3.length() != 0) {
    return Result.Ok(5);
  }
  return Result.Ok(0);
}
)");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// Issue #68: a global whose type is a generic class must run its constructor
// ============================================================================

// `Class<T>(args)` parses as a generic call, which the static initializer used
// to ignore — the global stayed zeroed and the first use crashed.
TEST(MemorySafety_GlobalAssignment, generic_class_global_runs_constructor) {
  auto value = executeString(R"(
    class Box<T> {
      var value: T;
      var tag: i32;
      init(v: T, tag_: i32) {
        this.value = v;
        this.tag = tag_;
      }
      method get() T { return this.value; }
      method get_tag() i32 { return this.tag; }
    }

    var b: Box<i32> = Box<i32>(41, 1);

    function main() i32 {
      return b.get() + b.get_tag();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(MemorySafety_GlobalAssignment, vec_global_is_constructed) {
  auto value = executeStringWithStdlib(R"(
using std;
var alloc: HeapAllocator = HeapAllocator();
/** Handles this global initializer’s failure at the startup boundary. */
function initialize_v() Vec<i64> {
  return match Vec<i64>(alloc, 8) {
    AllocationResult.Ok(value) => value,
    _ => {
      std.panic("unexpected global initialization failure");
      initialize_v();
    }
  };
}
var v: Vec<i64> = initialize_v();

/** Runs the fixture with explicit error propagation. */
function main() Result<i64> {
  if (v.capacity() != 8) {
    return Result.Ok(-1);
  }
  try v.push(10);
  try v.push(20);
  return Result.Ok(v.size());
}
)");
  EXPECT_EQ(value, 2);
}

// The reproduction from issue #68: a global Map reported capacity 0 and then
// crashed with a divide-by-zero on the first insert.
TEST(MemorySafety_GlobalAssignment, map_global_is_constructed) {
  auto value = executeStringWithStdlib(R"(
using std;
var alloc: HeapAllocator = HeapAllocator();
/** Handles this global initializer’s failure at the startup boundary. */
function initialize_m() Map<String, i32> {
  return match Map<String, i32>(alloc, 64) {
    AllocationResult.Ok(value) => value,
    _ => {
      std.panic("unexpected global initialization failure");
      initialize_m();
    }
  };
}
var m: Map<String, i32> = initialize_m();

/** Runs the fixture with explicit error propagation. */
function main() Result<i32> {
  if (m.capacity() != 64) {
    return Result.Ok(-1);
  }
  var k = try String(alloc, "key");
  try m.insert(k, 3);
  if (m.size() != 1) {
    return Result.Ok(-2);
  }
  var found = 0;
  if (true) {
    var k2 = try String(alloc, "key");
    found = (match m.get(k2) {
      std.LookupResult.Ok(value) => value,
      std.LookupResult.NotFound(_) => {
        return Result.Ok(-100);
      }
    });
  }
  return Result.Ok(found);
}
)");
  EXPECT_EQ(value, 3);
}

// A global initialized from something other than a constructor call takes the
// returned value by move rather than staying zeroed.
TEST(MemorySafety_GlobalAssignment, global_initialized_from_factory_call) {
  auto value = executeStringWithStdlib(R"(

    using std;
    var alloc: HeapAllocator = HeapAllocator();

    function make() Result<Vec<i64>> {
      var v = try Vec<i64>(alloc, 4);
      try v.push(7);
      return Result.Ok( v);
    }

    /** Handles this global initializer’s failure at the startup boundary. */
function initialize_g() Vec<i64> {
 return match make() {
 Result.Ok(value) => value,
 _ => { std.panic("unexpected global initialization failure"); initialize_g(); }
 };
}
var g: Vec<i64> = initialize_g();

    function main() i64 {
      if (g.capacity() != 4) { return -1; }
      return g.size();
    }
  )");
  EXPECT_EQ(value, 1);
}

// A constructor taking `ref T` accepts a global as that argument: globals live
// in module storage, not in a function's scope.
TEST(MemorySafety_GlobalAssignment, constructor_takes_global_by_ref) {
  auto value = executeStringWithStdlib(R"(
using std;
var alloc: HeapAllocator = HeapAllocator();

/** Runs the fixture with explicit error propagation. */
function main() Result<i64> {
  var s = try String(alloc, "hello");
  return Result.Ok(s.length());
}
)");
  EXPECT_EQ(value, 5);
}
