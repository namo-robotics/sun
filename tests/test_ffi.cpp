// tests/test_ffi.cpp — C FFI (`extern function`) declarations
//
// These call real libc symbols. Under JIT they resolve through the
// DynamicLibrarySearchGenerator for the current process; AOT builds get them
// from the C runtime that `cc` links in.

#include <gtest/gtest.h>

#include <string>

#include "execution_utils.h"

// ============================================================================
// Calling C functions
// ============================================================================

TEST(FFITest, call_libc_abs) {
  auto value = executeString(R"(
    extern function abs(x: i32) i32;

    function main() i32 {
        return abs(-42);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(FFITest, extern_declared_after_use) {
  // The block pre-pass must declare externs up front, so an extern may be
  // used before the declaration appears in the file.
  auto value = executeString(R"(
    function main() i32 {
        return abs(-7);
    }

    extern function abs(x: i32) i32;
  )");
  EXPECT_EQ(value, 7);
}

TEST(FFITest, extern_called_twice_keeps_one_declaration) {
  // Re-declaring would give LLVM a uniqued name (abs.1) that no longer
  // matches the C symbol.
  auto value = executeString(R"(
    extern function abs(x: i32) i32;

    function main() i32 {
        return abs(-3) + abs(-4);
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(FFITest, extern_with_pointer_args_and_void_return) {
  auto value = executeString(R"(
    extern function malloc(size: i64) raw_ptr<u8>;
    extern function free(p: raw_ptr<u8>) void;

    function main() i32 {
        var p = malloc(64);
        free(p);
        return 5;
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(FFITest, extern_inside_module_via_using) {
  auto value = executeString(R"(
    module libc {
        extern function abs(x: i32) i32;
    }
    using libc;

    function main() i32 {
        return abs(-11);
    }
  )");
  EXPECT_EQ(value, 11);
}

TEST(FFITest, extern_coexists_with_sun_overload) {
  // The extern keeps the bare C symbol while the Sun function keeps its
  // mangled, param-suffixed name; overload resolution picks between them.
  auto value = executeString(R"(
    extern function abs(x: i32) i32;
    function abs(x: f64) f64 { return 100.0; }

    function main() i32 {
        var d = abs(-2.5);
        if (d > 99.0) { return abs(-9); }
        return 0;
    }
  )");
  EXPECT_EQ(value, 9);
}

// ============================================================================
// `declare` must not be treated as a C extern
// ============================================================================

TEST(FFITest, declare_forward_decl_keeps_sun_mangling) {
  // `declare function` and `extern function` are both bodyless, but only the
  // latter is a C symbol. A `declare` must still resolve to the Sun
  // definition's mangled name.
  auto value = executeString(R"(
    declare function isOdd(n: i32) bool;

    function isEven(n: i32) bool {
        if (n == 0) { return true; }
        return isOdd(n - 1);
    }

    function isOdd(n: i32) bool {
        if (n == 0) { return false; }
        return isEven(n - 1);
    }

    function main() i32 {
        if (isEven(4)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Signature validation
// ============================================================================

TEST(FFITest, extern_requires_explicit_return_type) {
  EXPECT_THROW(executeString(R"(
    extern function h(n: i32);
    function main() i32 { return 0; }
  )"),
               std::exception);
}

TEST(FFITest, extern_rejects_class_parameter) {
  // Aggregates need SysV classification (byval/sret) that codegen does not
  // emit yet, so they must be rejected rather than silently miscompiled.
  EXPECT_THROW(executeString(R"(
    class P { var x: i32; }
    extern function f(p: P) i32;
    function main() i32 { return 0; }
  )"),
               std::exception);
}

TEST(FFITest, extern_rejects_class_return) {
  EXPECT_THROW(executeString(R"(
    class P { var x: i32; }
    extern function g(n: i32) P;
    function main() i32 { return 0; }
  )"),
               std::exception);
}

TEST(FFITest, extern_rejects_static_ptr_parameter) {
  // static_ptr is a fat { ptr, i64 } struct; passing one to C needs the
  // data-pointer extract that is not implemented yet.
  EXPECT_THROW(executeString(R"(
    extern function s(p: static_ptr<u8>) i32;
    function main() i32 { return 0; }
  )"),
               std::exception);
}
