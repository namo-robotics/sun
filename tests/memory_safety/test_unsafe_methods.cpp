/* Tests that unsafe method contracts survive calls, aliases, and interfaces. */

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

using sun::driver::executeString;
using sun::driver::executeStringWithStdlib;

TEST(MemorySafety_UnsafeMethods, direct_call_requires_block) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Reader {
      public unsafe method read() i32 { return 42; }
    }
    function main() i32 { var r = Reader(); return r.read(); }
  )"),
                                "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, block_allows_call) {
  EXPECT_EQ(executeString(R"(
    class Reader {
      public const unsafe method read() i32 { return 42; }
    }
    function main() i32 {
      const r = Reader();
      return unsafe { r.read(); };
    }
  )"),
            42);
}

TEST(MemorySafety_UnsafeMethods, unsafe_body_still_requires_blocks) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Reader {
      public unsafe method read(p: raw_ptr<i32>) i32 {
        return _load<i32>(p, 0);
      }
    }
    function main() i32 { return 0; }
  )"),
                                "unsafe block");
}

TEST(MemorySafety_UnsafeMethods, method_alias_keeps_requirement) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Reader {
      public unsafe method read() i32 { return 42; }
    }
    function main() i32 {
      var r = Reader();
      var read = r.read;
      return read();
    }
  )"),
                                "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, typed_alias_allows_unsafe_call) {
  EXPECT_EQ(executeString(R"(
    class Reader {
      public unsafe method read() i32 { return 42; }
    }
    function main() i32 {
      var r = Reader();
      var read: unsafe <'_>() => i32 = r.read;
      return unsafe { read(); };
    }
  )"),
            42);
}

TEST(MemorySafety_UnsafeMethods, alias_cannot_become_safe) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Reader {
      public unsafe method read() i32 { return 42; }
    }
    function main() i32 {
      var r = Reader();
      var read: <'_>() => i32 = r.read;
      return read();
    }
  )"),
                                "Cannot assign");
}

TEST(MemorySafety_UnsafeMethods, generic_method_requires_block) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Reader<T> {
      public unsafe method read<U>(value: U) U { return value; }
    }
    function main() i32 {
      var r = Reader<i32>();
      return r.read<i32>(42);
    }
  )"),
                                "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, generic_callback_keeps_requirement) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Reader {
      public unsafe method read() i32 { return 42; }
    }
    function invoke<F: _Callable>(f: F, args...: _params_of<F>) i32 { return f(args...); }
    function main() i32 {
      var r = Reader();
      return invoke(r.read);
    }
  )"),
                                "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, interface_dispatch_requires_block) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface IRead { public unsafe method read() i32; }
    class Reader implements IRead {
      public unsafe method read() i32 { return 42; }
    }
    function invoke(r: ref IRead) i32 { return r.read(); }
    function main() i32 { var r = Reader(); return invoke(r); }
  )"),
                                "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, unsafe_cannot_implement_safe_interface) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    interface IRead { public method read() i32; }
    class Reader implements IRead {
      public unsafe method read() i32 { return 42; }
    }
    function main() i32 { return 0; }
  )"),
                                "cannot implement a safe interface method");
}

TEST(MemorySafety_UnsafeMethods, imported_string_unsafe_access_requires_block) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = String(alloc, "hello");
      s.unsafe_set_at(0, b'H');
      return 0;
    }
  )"),
                                "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, implicit_index_requires_block) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Reader {
      public unsafe method __index__(indices: const ref array<i64>) i32 {
        return 42;
      }
    }
    function main() i32 { var r = Reader(); return r[0]; }
  )"),
                                "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, block_does_not_make_nested_lambda_unsafe) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Reader {
      public unsafe method read() i32 { return 42; }
    }
    function main() i32 {
      var r = Reader();
      return unsafe {
        var f = [ref r]() => i32 { return r.read(); };
        f();
      };
    }
  )"),
                                "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, unsafe_callback_cannot_be_spawned_as_safe) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std.thread;
    class Reader {
      public unsafe method read() i32 { return 42; }
    }
    function main() i32 {
      var r = Reader();
      var t = spawn(r.read);
      return t.join();
    }
  )"),
                                "spawned function must be safe");
}

TEST(MemorySafety_UnsafeMethods, imported_string_unsafe_read_requires_block) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = String(alloc, "hello");
      return _convert<i32>(s.unsafe_at(0));
    }
  )"),
                                "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods,
     imported_buffer_unchecked_call_requires_block) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var b = ContiguousBuffer<i32>(alloc, 1);
      b.set_unchecked(0, 42);
      return 0;
    }
  )"),
                                "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods,
     imported_vector_unchecked_call_requires_block) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeStringWithStdlib(R"(
    using std;
    function main() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<i32>(alloc, 1);
      v.push(42);
      return v.get_unchecked(0);
    }
  )"),
                                "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, expression_allows_calls_and_aliases) {
  EXPECT_EQ(executeString(R"(
    class Reader {
      public const unsafe method read() i32 { return 21; }
    }
    function main() i32 {
      const r = Reader();
      var read = r.read;
      return unsafe r.read() + unsafe read();
    }
  )"), 42);
}

TEST(MemorySafety_UnsafeMethods, expression_allows_intrinsics_and_void_calls) {
  EXPECT_EQ(executeString(R"(
    function main() i32 {
      var p = unsafe _malloc(4);
      unsafe _store<i32>(p, 0, 42);
      var result = unsafe _load<i32>(p, 0);
      unsafe _free(p);
      return result;
    }
  )"), 42);
}

TEST(MemorySafety_UnsafeMethods, expression_includes_indexing_and_call_arguments) {
  EXPECT_EQ(executeString(R"(
    class Reader {
      public unsafe method __index__(indices: const ref array<i64>) i32 {
        return 21;
      }
      public unsafe method twice(value: i32) i32 { return value * 2; }
    }
    function main() i32 {
      var r = Reader();
      return unsafe r.twice(r[0]);
    }
  )"), 42);
}

TEST(MemorySafety_UnsafeMethods, expression_stops_before_binary_operand) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Reader { public unsafe method read() i32 { return 21; } }
    function main() i32 {
      var r = Reader();
      return unsafe r.read() + r.read();
    }
  )"), "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, parentheses_extend_unsafe_expression) {
  EXPECT_EQ(executeString(R"(
    class Reader { public unsafe method read() i32 { return 21; } }
    function main() i32 {
      var r = Reader();
      return unsafe (r.read() + r.read());
    }
  )"), 42);
}

TEST(MemorySafety_UnsafeMethods, expression_stops_before_next_statement) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Reader { public unsafe method read() i32 { return 42; } }
    function main() i32 {
      var r = Reader();
      unsafe r.read();
      return r.read();
    }
  )"), "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, expression_does_not_make_nested_lambda_unsafe) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Reader { public unsafe method read() i32 { return 42; } }
    function main() i32 {
      var r = Reader();
      var f = unsafe [ref r]() => i32 { return r.read(); };
      return f();
    }
  )"), "requires an unsafe block");
}

TEST(MemorySafety_UnsafeMethods, expression_supports_unary_operators_and_nesting) {
  EXPECT_EQ(executeString(R"(
    class Reader { public unsafe method read() i32 { return 21; } }
    function main() i32 {
      var r = Reader();
      return -unsafe -r.read() + unsafe unsafe r.read();
    }
  )"), 42);
}

TEST(MemorySafety_UnsafeMethods, expression_transfers_owned_values_once) {
  EXPECT_EQ(executeString(R"(
    var drops: i32 = 0;
    class Owner {
      deinit() { drops = drops + 1; }
      method tag() i32 { return 40; }
    }
    function make() Owner { return Owner(); }
    function use() i32 {
      var owner = unsafe make();
      unsafe make();
      return owner.tag() + drops;
    }
    function main() i32 {
      var result = use();
      return result + drops;
    }
  )"), 42);
}
