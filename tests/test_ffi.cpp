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

// ============================================================================
// Passing strings to C (static_ptr<T> -> raw_ptr<T>)
// ============================================================================
// A static_ptr is a fat { ptr, i64 }; the type system accepts it where a
// raw_ptr is expected, so codegen must narrow it to the data pointer or the
// call fails LLVM verification.

TEST(FFITest, string_literal_to_raw_ptr_param) {
  auto value = executeString(R"(
    extern function strlen(s: raw_ptr<u8>) i64;

    function main() i32 {
        return strlen("hello, ffi");
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(FFITest, static_ptr_variable_to_raw_ptr_param) {
  auto value = executeString(R"(
    extern function strlen(s: raw_ptr<u8>) i64;

    function main() i32 {
        var s: static_ptr<u8> = "via variable";
        return strlen(s);
    }
  )");
  EXPECT_EQ(value, 12);
}

// ============================================================================
// C varargs
// ============================================================================

TEST(FFITest, varargs_call_with_no_variadic_args) {
  auto value = executeString(R"(
    extern function printf(fmt: raw_ptr<u8>, ...) i32;

    function main() i32 {
        return printf("abcde\n");
    }
  )");
  EXPECT_EQ(value, 6);
}

TEST(FFITest, varargs_call_with_mixed_args) {
  // printf returns the number of characters written, which pins down that
  // every promoted argument arrived intact.
  auto value = executeString(R"(
    extern function printf(fmt: raw_ptr<u8>, ...) i32;

    function main() i32 {
        return printf("%d-%s-%d\n", 42, "xy", 7);
    }
  )");
  EXPECT_EQ(value, 8);  // "42-xy-7\n"
}

TEST(FFITest, varargs_promotes_float_to_double) {
  // f32 must widen to double or printf reads garbage for %f.
  auto value = executeString(R"(
    extern function printf(fmt: raw_ptr<u8>, ...) i32;

    function main() i32 {
        var f: f32 = 1.5;
        return printf("%.2f\n", f);
    }
  )");
  EXPECT_EQ(value, 5);  // "1.50\n"
}

TEST(FFITest, varargs_rejects_too_few_fixed_args) {
  EXPECT_THROW(executeString(R"(
    extern function printf(fmt: raw_ptr<u8>, ...) i32;
    function main() i32 { return printf(); }
  )"),
               std::exception);
}

TEST(FFITest, varargs_not_allowed_on_sun_function) {
  // Sun has no va_arg, so `...` on a definition is meaningless.
  EXPECT_THROW(executeString(R"(
    function f(a: i32, ...) i32 { return a; }
    function main() i32 { return 0; }
  )"),
               std::exception);
}

TEST(FFITest, varargs_requires_a_named_parameter) {
  EXPECT_THROW(executeString(R"(
    extern function f(...) i32;
    function main() i32 { return 0; }
  )"),
               std::exception);
}

// ============================================================================
// extern "C" ABI string and `as "symbol"` renaming
// ============================================================================

TEST(FFITest, extern_c_abi_string) {
  auto value = executeString(R"(
    extern "C" function abs(x: i32) i32;

    function main() i32 {
        return abs(-42);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(FFITest, extern_rejects_unknown_abi) {
  EXPECT_THROW(executeString(R"(
    extern "Rust" function abs(x: i32) i32;
    function main() i32 { return 0; }
  )"),
               std::exception);
}

TEST(FFITest, extern_symbol_rename) {
  // The Sun-side name is my_abs; the symbol linked against is abs.
  auto value = executeString(R"(
    extern function my_abs(x: i32) i32 as "abs";

    function main() i32 {
        return my_abs(-9);
    }
  )");
  EXPECT_EQ(value, 9);
}

TEST(FFITest, extern_symbol_rename_with_abi_and_pointer) {
  auto value = executeString(R"(
    extern "C" function c_strlen(s: raw_ptr<u8>) i64 as "strlen";

    function main() i32 {
        return c_strlen("abcdef");
    }
  )");
  EXPECT_EQ(value, 6);
}

TEST(FFITest, renamed_extern_does_not_bind_its_c_name) {
  // Only the Sun-side name is in scope; the C symbol is not introduced.
  EXPECT_THROW(executeString(R"(
    extern function my_abs(x: i32) i32 as "abs";
    function main() i32 { return abs(-9); }
  )"),
               std::exception);
}

TEST(FFITest, as_is_still_usable_as_an_identifier) {
  // `as` is matched contextually in extern declarations, not reserved.
  auto value = executeString(R"(
    function main() i32 {
        var as = 5;
        return as * 2;
    }
  )");
  EXPECT_EQ(value, 10);
}
