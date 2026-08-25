// Assigning a compound value (class / payload enum) to a global MOVES it:
// the global's old value is dropped once, the source is invalidated so its
// own drop is a no-op, and the global then owns the payload. Regression
// tests for issues #70 and #71 (globals of heap-owning types).

#include <gtest/gtest.h>

#include "driver/execution_utils.h"
#include "support/error.h"

namespace {

// Owner models a heap-owning class: like Unique<T>, deinit is a no-op on
// moved-from (zeroed) storage.
const char* kOwnerPreamble = R"(
    var counter: i32 = 0;

    class Owner {
      var id: i32;
      function init(id: i32) {
        this.id = id;
      }
      function deinit() void {
        if (this.id != 0) {
          counter = counter + 1;
          this.id = 0;
        }
      }
      function get_id() i32 {
        return this.id;
      }
    }
)";

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
               SunError);
}

// Issue #70: assigning a String to a global used to store the source's
// address over the global's data pointer.
TEST(MemorySafety_GlobalAssignment,
     string_global_assignment_transfers_ownership) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    var g: String = String(make_heap_allocator(), "");

    function replace() void {
      var a = make_heap_allocator();
      var rebuilt = String(a, "new contents");
      g = rebuilt;
    }

    function main() i64 {
      replace();
      if (g.at(0) != 110) {  // 'n'
        return -1;
      }
      return g.length();
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
    using sun;
    var g_queue: String = String(make_heap_allocator(), "");

    function append(q: ref String, s: ref String) void {
      q.append(s);
    }

    function queue_pop(alloc: ref HeapAllocator) String {
      var out = String(alloc, "");
      if (g_queue.length() > 0) {
        var nl: i64 = 0;
        while (nl < g_queue.length() and g_queue.at(nl) != 10) { nl = nl + 1; }
        for (var i: i64 = 0; i < nl; i = i + 1) { out.append_char(g_queue.at(i)); }

        var rest = String(alloc, "");
        for (var i: i64 = nl + 1; i < g_queue.length(); i = i + 1) {
          rest.append_char(g_queue.at(i));
        }
        g_queue.clear();
        append(g_queue, rest);
      }
      return out;
    }

    function fill(alloc: ref HeapAllocator) void {
      var q = String(alloc, "");
      g_queue = q;
      var a = String(alloc, "alpha\n");
      append(g_queue, a);
      var b = String(alloc, "beta\n");
      append(g_queue, b);
    }

    function main() i64 {
      var alloc = make_heap_allocator();
      fill(alloc);
      var p1 = queue_pop(alloc);
      if (p1.length() != 5 or p1.at(0) != 97) { return 1; }   // "alpha"
      if (g_queue.length() != 5 or g_queue.at(0) != 98) { return 2; }  // "beta\n"
      var p2 = queue_pop(alloc);
      if (p2.length() != 4 or p2.at(0) != 98) { return 3; }   // "beta"
      if (g_queue.length() != 0) { return 4; }
      var p3 = queue_pop(alloc);
      if (p3.length() != 0) { return 5; }
      return 0;
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
      function init(v: T, tag_: i32) {
        this.value = v;
        this.tag = tag_;
      }
      function get() T { return this.value; }
      function get_tag() i32 { return this.tag; }
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
    using sun;
    var alloc: HeapAllocator = HeapAllocator();
    var v: Vec<i64> = Vec<i64>(alloc, 8);

    function main() i64 {
      if (v.capacity() != 8) { return -1; }
      v.push(10);
      v.push(20);
      return v.size();
    }
  )");
  EXPECT_EQ(value, 2);
}

// The reproduction from issue #68: a global Map reported capacity 0 and then
// crashed with a divide-by-zero on the first insert.
TEST(MemorySafety_GlobalAssignment, map_global_is_constructed) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    var alloc: HeapAllocator = HeapAllocator();
    var m: Map<String, i32> = Map<String, i32>(alloc, 64);

    function main() i32 {
      if (m.capacity() != 64) { return -1; }
      var k = String(alloc, "key");
      m.insert(k, 3);
      if (m.size() != 1) { return -2; }
      var found = 0;
      try {
        var k2 = String(alloc, "key");
        found = m.get(k2);
      } catch (e: IError) {
        return -3;
      }
      return found;
    }
  )");
  EXPECT_EQ(value, 3);
}

// A global initialized from something other than a constructor call takes the
// returned value by move rather than staying zeroed.
TEST(MemorySafety_GlobalAssignment, global_initialized_from_factory_call) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    var alloc: HeapAllocator = HeapAllocator();

    function make() Vec<i64> {
      var v = Vec<i64>(alloc, 4);
      v.push(7);
      return v;
    }

    var g: Vec<i64> = make();

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
    using sun;
    var alloc: HeapAllocator = HeapAllocator();

    function main() i64 {
      var s = String(alloc, "hello");
      return s.length();
    }
  )");
  EXPECT_EQ(value, 5);
}
