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
    function helper() i32, IError {
      var alloc = make_heap_allocator();
      var v = Vec<Owner>(alloc, 4);
      v.push(Owner(1));
      v.push(Owner(2));
      var popped = v.pop();
      // popped (id 2) is dropped at helper exit; v deinit drops id 1
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
