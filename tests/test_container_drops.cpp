// Tests that containers drop the elements they own: Vec/Map/LinkedList run
// element deinits on destruction, clear, overwrite, and removal — and do NOT
// drop elements whose ownership was moved out (pop/take/remove).

#include <gtest/gtest.h>

#include "execution_utils.h"

namespace {

// Owner models a real resource holder: like Unique<T>, its deinit is a no-op
// on moved-from (zeroed) storage — Sun's move semantics zero the source, so
// owning types must treat the all-zero state as "nothing to release".
const char* kOwnerPreamble = R"(
    using sun;

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

TEST(ContainerDropTest, vec_deinit_drops_all_elements) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function helper() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<Owner>(alloc, 4);
      v.push(Owner(1));
      v.push(Owner(2));
      v.push(Owner(3));
      return 0;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  EXPECT_EQ(value, 3);
}

TEST(ContainerDropTest, vec_clear_drops_elements) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<Owner>(alloc, 4);
      v.push(Owner(1));
      v.push(Owner(2));
      v.clear();
      return counter;
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(ContainerDropTest, vec_set_drops_overwritten_element_only) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<Owner>(alloc, 4);
      v.push(Owner(1));
      v.push(Owner(2));
      try {
        v.set(0, Owner(9));
      } catch (e: IError) {
        return -1;
      }
      return counter;
    }
  )"));
  // Only the overwritten element (id 1) dropped so far
  EXPECT_EQ(value, 1);
}

TEST(ContainerDropTest, vec_pop_moves_ownership_no_double_drop) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function helper() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<Owner>(alloc, 4);
      v.push(Owner(1));
      v.push(Owner(2));
      var popped = v.pop();
      // popped (Option.Some(id 2)) is dropped at helper exit through the
      // enum drop glue; v deinit drops id 1
      return match popped {
        Option.Some(o) => o.get_id(),
        Option.None => -1
      };
    }

    function main() i32 {
      var id: i32 = helper();
      if (id != 2) {
        return -2;
      }
      return counter;
    }
  )"));
  // Exactly two drops total: one for the popped value, one for the element
  // still in the Vec
  EXPECT_EQ(value, 2);
}

TEST(ContainerDropTest, vec_take_transfers_ownership_no_double_drop) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function helper() i32, IError {
      var alloc = make_heap_allocator();
      var v = Vec<Owner>(alloc, 4);
      v.push(Owner(1));
      v.push(Owner(2));
      var taken = v.take(0);
      // taken (id 1) dropped at helper exit; slot 0 is zeroed (no-op drop);
      // v deinit drops id 2
      return taken.get_id();
    }

    function main() i32 {
      var id: i32 = 0;
      try {
        id = helper();
      } catch (e: IError) {
        return -1;
      }
      if (id != 1) {
        return -2;
      }
      return counter;
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(ContainerDropTest, vec_of_vec_drops_recursively) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function main() i32 {
      var alloc = make_heap_allocator();
      if (true) {
        var outer = Vec<Vec<Owner>>(alloc, 2);
        var inner1 = Vec<Owner>(alloc, 2);
        inner1.push(Owner(1));
        inner1.push(Owner(2));
        var inner2 = Vec<Owner>(alloc, 2);
        inner2.push(Owner(3));
        outer.push(inner1);
        outer.push(inner2);
      }
      return counter;
    }
  )"));
  // outer's deinit drops each inner Vec, which drops its Owners
  EXPECT_EQ(value, 3);
}

TEST(ContainerDropTest, map_deinit_drops_entries) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function helper() i32 {
      var alloc = make_heap_allocator();
      var m = Map<i64, Owner>(alloc, 8);
      m.insert(1, Owner(10));
      m.insert(2, Owner(20));
      return 0;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(ContainerDropTest, map_insert_overwrite_drops_old_value) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function main() i32 {
      var alloc = make_heap_allocator();
      var m = Map<i64, Owner>(alloc, 8);
      m.insert(1, Owner(10));
      m.insert(1, Owner(11));
      return counter;
    }
  )"));
  // The overwritten value (id 10) dropped; the map still owns id 11
  EXPECT_EQ(value, 1);
}

TEST(ContainerDropTest, map_remove_moves_value_out) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function helper() i32, IError {
      var alloc = make_heap_allocator();
      var m = Map<i64, Owner>(alloc, 8);
      m.insert(1, Owner(10));
      var removed = m.remove(1);
      return removed.get_id();
    }

    function main() i32 {
      var id: i32 = 0;
      try {
        id = helper();
      } catch (e: IError) {
        return -1;
      }
      if (id != 10) {
        return -2;
      }
      return counter;
    }
  )"));
  // Exactly one drop: the removed value at helper exit (i64 keys drop as
  // no-ops)
  EXPECT_EQ(value, 1);
}

TEST(ContainerDropTest, map_clear_drops_entries) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function main() i32 {
      var alloc = make_heap_allocator();
      var m = Map<i64, Owner>(alloc, 8);
      m.insert(1, Owner(10));
      m.insert(2, Owner(20));
      m.clear();
      return counter;
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(ContainerDropTest, linked_list_clear_drops_payloads) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function main() i32 {
      var alloc = make_heap_allocator();
      var list = LinkedList<Owner>(alloc);
      list.push_back(Owner(1));
      list.push_back(Owner(2));
      list.clear();
      return counter;
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(ContainerDropTest, linked_list_deinit_drops_payloads) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function helper() i32 {
      var alloc = make_heap_allocator();
      var list = LinkedList<Owner>(alloc);
      list.push_back(Owner(1));
      list.push_back(Owner(2));
      list.push_front(Owner(3));
      return 0;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  EXPECT_EQ(value, 3);
}

TEST(ContainerDropTest, linked_list_set_drops_old_payload) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function main() i32 {
      var alloc = make_heap_allocator();
      var list = LinkedList<Owner>(alloc);
      list.push_back(Owner(1));
      try {
        list.set(0, Owner(9));
      } catch (e: IError) {
        return -1;
      }
      return counter;
    }
  )"));
  EXPECT_EQ(value, 1);
}

TEST(ContainerDropTest, linked_list_pop_moves_ownership) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    function helper() i32, IError {
      var alloc = make_heap_allocator();
      var list = LinkedList<Owner>(alloc);
      list.push_back(Owner(1));
      list.push_back(Owner(2));
      var popped = list.pop_back();
      return popped.get_id();
    }

    function main() i32 {
      var id: i32 = 0;
      try {
        id = helper();
      } catch (e: IError) {
        return -1;
      }
      if (id != 2) {
        return -2;
      }
      return counter;
    }
  )"));
  // popped dropped at helper exit + remaining element dropped by list deinit
  EXPECT_EQ(value, 2);
}

TEST(ContainerDropTest, vec_of_string_no_crash) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<String>(alloc, 4);
      v.push(String(alloc, "hello"));
      v.push(String(alloc, "world"));
      v.clear();
      v.push(String(alloc, "again"));
      return v.size();
    }
  )");
  // Vec<String> drops element strings without crashing (heap buffers freed
  // exactly once)
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Peek accessors borrow (issue #69)
//
// get()/first()/last()/c[i] and iteration hand back a reference: the container
// keeps ownership of the element. Returning a bitwise copy instead left the
// copy and the stored element both owning the same heap buffer, and both
// released it — "double free detected in tcache 2" on scope exit.
// ============================================================================

TEST(ContainerDropTest, vec_get_borrows_owning_element) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<String>(alloc, 4);
      v.push(String(alloc, "hello"));
      v.push(String(alloc, "world"));
      var n: i64 = 0;
      try {
        var s = v.get(0);
        n = s.length();
      } catch (e: IError) {
        return -1;
      }
      return n;
    }
  )");
  // Borrowed, so the Vec still drops both strings exactly once at scope exit
  EXPECT_EQ(value, 5);
}

TEST(ContainerDropTest, vec_index_and_peeks_borrow_owning_elements) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<String>(alloc, 4);
      v.push(String(alloc, "aa"));
      v.push(String(alloc, "bbb"));
      var total: i64 = v[0].length() + v[1].length();
      total = total + match v.first() { Option.Some(s) => s.length(), Option.None => 0 };
      total = total + match v.last() { Option.Some(s) => s.length(), Option.None => 0 };
      for (var s: String in v) { total = total + s.length(); }
      return total;
    }
  )");
  // 5 (index) + 2 (first) + 3 (last) + 5 (iteration)
  EXPECT_EQ(value, 15);
}

TEST(ContainerDropTest, map_and_list_peeks_borrow_owning_values) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
      var alloc = make_heap_allocator();
      var m = Map<i64, String>(alloc, 16);
      m.insert(1, String(alloc, "alpha"));
      var total: i64 = 0;
      try { total = total + m.get(1).length(); } catch (e: IError) { return -1; }
      total = total + match m.find(1) { Option.Some(s) => s.length(), Option.None => 0 };

      var ll = LinkedList<String>(alloc);
      ll.push_back(String(alloc, "beta"));
      try { total = total + ll.get(0).length(); } catch (e: IError) { return -2; }
      total = total + match ll.first() { Option.Some(s) => s.length(), Option.None => 0 };
      return total;
    }
  )");
  // 5 + 5 (map get/find) + 4 + 4 (list get/first)
  EXPECT_EQ(value, 18);
}

// A borrow is a real reference, not a copy that happens to read the same value
TEST(ContainerDropTest, writing_through_a_borrow_hits_the_container) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<i32>(alloc, 4);
      v.push(10);
      v.push(20);
      try {
        var r = v.get(1);
        r = 99;
      } catch (e: IError) {
        return -1;
      }
      return match v.last() { Option.Some(x) => x, Option.None => 0 };
    }
  )");
  EXPECT_EQ(value, 99);
}

// take()/pop() still move the element out, leaving the slot to drop as a no-op
TEST(ContainerDropTest, take_moves_the_element_out) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<String>(alloc, 4);
      v.push(String(alloc, "hello"));
      v.push(String(alloc, "hi"));
      var n: i64 = 0;
      try {
        var owned = v.take(0);
        n = owned.length();
      } catch (e: IError) {
        return -1;
      }
      return n + v.size();
    }
  )");
  // owned dropped at scope exit, the Vec drops what is left — each buffer once
  EXPECT_EQ(value, 7);
}
