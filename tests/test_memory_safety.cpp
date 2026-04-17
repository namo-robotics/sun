// tests/test_memory_safety.cpp - Memory safety tests for Sun language
// Tests borrow checker enforcement: use-after-move, borrow conflicts, etc.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

#include "error.h"
#include "execution_utils.h"

// Helper macro for testing SunError with message content
#define EXPECT_SUN_ERROR_WITH_MESSAGE(stmt, expected_substr)            \
  do {                                                                  \
    try {                                                               \
      stmt;                                                             \
      FAIL() << "Expected SunError to be thrown";                       \
    } catch (const SunError& e) {                                       \
      EXPECT_NE(std::strstr(e.what(), expected_substr), nullptr)        \
          << "Expected error message to contain: \"" << expected_substr \
          << "\"\n"                                                     \
          << "Actual message: \"" << e.what() << "\"";                  \
    }                                                                   \
  } while (0)

// ============================================================================
// Use After Move - Basic Cases
// ============================================================================

// Simplest case: move to another variable, then use original
TEST(MemorySafety, use_after_move_simple) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      import "build/stdlib.moon";
    using sun;
      
      function main() i64 {
          var allocator = make_heap_allocator();
          var s = String(allocator, "Hello");
          var s2 = s;        // s is moved here
          return s.length(); // ERROR: use of moved variable
      }
    )"),
                                "Borrow check failed");
}

// Move then access field directly
TEST(MemorySafety, use_after_move_field_access) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      import "build/stdlib.moon";
    using sun;
      
      function main() i64 {
          var allocator = make_heap_allocator();
          var s = String(allocator, "Test");
          var s2 = s;
          return s.len;  // ERROR: accessing field of moved variable
      }
    )"),
                                "Borrow check failed");
}

// ============================================================================
// Use After Move - Function Calls
// ============================================================================

// Pass by value (actually ref under the hood) doesn't move
TEST(MemorySafety, ref_param_no_move) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function get_len(s: ref String) i64 {
        return s.length();
    }
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello");
        var len = get_len(s);  // Borrow, not move
        return s.length();     // OK: s still valid
    }
  )");
  EXPECT_EQ(value, 5);
}

// ============================================================================
// Use After Move - Multiple Operations
// ============================================================================

// Double move should fail on second move
TEST(MemorySafety, double_move) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      import "build/stdlib.moon";
    using sun;
      
      function main() i64 {
          var allocator = make_heap_allocator();
          var s = String(allocator, "Hello");
          var s2 = s;  // First move
          var s3 = s;  // ERROR: s already moved
          return 0;
      }
    )"),
                                "Borrow check failed");
}

// ============================================================================
// Valid Patterns - Should Compile
// ============================================================================

// Using the moved-to variable is fine
TEST(MemorySafety, use_moved_to_variable) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var s = String(allocator, "Hello");
        var s2 = s;
        return s2.length();  // OK: s2 owns the data now
    }
  )");
  EXPECT_EQ(value, 5);
}

// Multiple ref borrows conflict (refs are mutable by default in Sun)
TEST(MemorySafety, multiple_ref_borrows_conflict) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          var x: i32 = 42;
          ref r1 = x;
          ref r2 = x;  // ERROR: x already mutably borrowed
          return r1 + r2;
      }
    )"),
                                "Borrow check failed");
}

// Ref goes out of scope, original usable again
TEST(MemorySafety, ref_scope_ends) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        if (true) {
            ref r = x;
            r = 20;
        };
        // r out of scope
        x = x + 5;  // OK: no active borrows
        return x;
    }
  )");
  EXPECT_EQ(value, 25);
}

// ============================================================================
// Matrix Move Semantics
// ============================================================================

// Matrix move then use is error
TEST(MemorySafety, matrix_use_after_move) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      import "build/stdlib.moon";
    using sun;
      
      function main() i64 {
          var allocator = make_heap_allocator();
          var shape: array<i64, 1> = [10];
          var m = Matrix<i64>(allocator, shape);
          var m2 = m;  // Move
          return m.size();  // ERROR: m was moved
      }
    )"),
                                "Borrow check failed");
}

// Matrix ref borrow is fine
TEST(MemorySafety, matrix_ref_borrow) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function get_size(m: ref Matrix<i64>) i64 {
        return m.size();
    }
    
    function main() i64 {
        var allocator = make_heap_allocator();
        var shape: array<i64, 1> = [10];
        var m = Matrix<i64>(allocator, shape);
        var sz = get_size(m);  // Borrow
        return m.size();       // OK: m still valid
    }
  )");
  EXPECT_EQ(value, 10);
}

// ============================================================================
// Allocator Move Semantics
// ============================================================================

// Allocator move then use is error
TEST(MemorySafety, allocator_use_after_move) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      import "build/stdlib.moon";
    using sun;
      
      function main() i64 {
          var a = make_heap_allocator();
          var a2 = a;  // Move
          var s = String(a, "Hello");  // ERROR: a was moved
          return s.length();
      }
    )"),
                                "Borrow check failed");
}

// Use allocator.copy() to get a new allocator
TEST(MemorySafety, allocator_copy_valid) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        var a = make_heap_allocator();
        var a2 = a.copy();  // Get a copy, a still valid
        var s1 = String(a, "Hello");
        var s2 = String(a2, "World");
        return s1.length() + s2.length();
    }
  )");
  EXPECT_EQ(value, 10);
}

// ============================================================================
// Null Reference Prevention
// ============================================================================

// Can't create ref to null literal
TEST(MemorySafety, ref_to_null_literal_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          ref r = null;  // ERROR: reference must be bound to a variable
          return 0;
      }
    )"),
                                "Borrow check failed");
}

// Can't create ref to numeric literal (temporary)
TEST(MemorySafety, ref_to_literal_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function main() i32 {
          ref r = 42;  // ERROR: reference must be bound to a variable
          return r;
      }
    )"),
                                "Borrow check failed");
}

// Can't create ref to function call result (temporary)
TEST(MemorySafety, ref_to_temporary_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function make_value() i32 {
          return 42;
      }
      
      function main() i32 {
          ref r = make_value();  // ERROR: reference must be bound to a variable
          return r;
      }
    )"),
                                "Borrow check failed");
}

// ============================================================================
// Dangling Reference Prevention
// ============================================================================

// Can't return reference type from function
TEST(MemorySafety, cannot_return_reference_type) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
      function get_ref(x: ref i32) ref i32 {
          return x;  // ERROR: functions cannot return reference types
      }
      
      function main() i32 {
          var x: i32 = 42;
          ref r = get_ref(x);
          return r;
      }
    )"),
                                "Borrow check failed");
}

// ============================================================================
// Reference Field Prevention
// ============================================================================

// When FORBID_REF_FIELDS_IN_CLASSES is true, classes can't have reference-typed
// fields. When false (current config), ref fields are allowed.
// This test verifies ref fields are accepted when the config allows them.
TEST(MemorySafety, class_ref_field_allowed_when_config_permits) {
  // With FORBID_REF_FIELDS_IN_CLASSES = false, ref fields are allowed
  // This test just verifies no error is thrown during semantic analysis
  // for ref fields in classes.
  // Note: Actually using ref fields safely requires careful lifetime management
  // which Sun doesn't enforce yet - this just tests the config flag works.
  EXPECT_NO_THROW(compileString(R"(
      class Holder {
          var r: ref i32;

          function init(self: ref Holder, val: ref i32) {
              this.r = val;
          }
      }

      function main() i32 {
          return 0;
      }
    )"));
}

// ============================================================================
// Raw Pointer Safety
// ============================================================================

// raw_ptr can be null (for C interop), but dereferencing null is UB
// These tests verify the type system allows null for raw_ptr
TEST(MemorySafety, raw_ptr_null_assignment) {
  auto value = executeString(R"(
    function main() i32 {
        var p: raw_ptr<i32> = null;  // OK: raw_ptr can be null
        // Note: dereferencing would be UB, but type system allows it
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// raw_ptr null check pattern
TEST(MemorySafety, raw_ptr_null_check_pattern) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i32 {
        var alloc = make_heap_allocator();
        var p: raw_ptr<i32> = alloc.alloc<i32>(1);
        // In real code you'd check if p is null before use
        // This test just verifies the allocation works
        _store<i32>(p, 0, 42);
        var loaded: i32 = _load<i32>(p, 0);
        alloc.dealloc(p, 4);
        return loaded;
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Sequential Borrows (Valid After Prior Borrow Ends)
// ============================================================================

// Can reborrow after first borrow ends (in sequence, not overlapping)
TEST(MemorySafety, sequential_borrows_valid) {
  auto value = executeString(R"(
    function use_ref(r: ref i32) i32 {
        return r;
    }
    
    function main() i32 {
        var x: i32 = 10;
        var a: i32 = use_ref(x);  // First borrow ends after call
        var b: i32 = use_ref(x);  // Second borrow is fine
        return a + b;
    }
  )");
  EXPECT_EQ(value, 20);
}

// ============================================================================
// Nested Function Borrows
// ============================================================================

// Passing same ref to nested function calls is valid (borrows are sequential)
TEST(MemorySafety, nested_borrow_sequential) {
  auto value = executeString(R"(
    function increment(r: ref i32) i32 {
        r = r + 1;
        return r;
    }
    
    function main() i32 {
        var x: i32 = 0;
        increment(x);  // Borrow 1
        increment(x);  // Borrow 2 (after 1 ends)
        increment(x);  // Borrow 3 (after 2 ends)
        return x;
    }
  )");
  EXPECT_EQ(value, 3);
}

// ============================================================================
// Ownership Through Containers
// ============================================================================

// Creating string and using it locally is fine (no return by value needed)
TEST(MemorySafety, owned_value_local_use) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    
    function main() i64 {
        var alloc = make_heap_allocator();
        var s = String(alloc, "Hello");
        return s.length();
    }
  )");
  EXPECT_EQ(value, 5);
}