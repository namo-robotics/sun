// tests/test_ffi.cpp — C FFI (`extern function`) declarations
//
// These call real libc symbols. Under JIT they resolve through the
// DynamicLibrarySearchGenerator for the current process; AOT builds get them
// from the C runtime that `cc` links in.

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "abi/c_abi.h"
#include "ast_deserializer.h"
#include "ast_serializer.h"
#include "compiler.h"
#include "execution_utils.h"
#include "extern_c.h"
#include "parser.h"

// ============================================================================
// Calling C functions
// ============================================================================

TEST(FFITest, call_libc_abs) {
  auto value = executeString(R"(
    extern function abs(x: i32) i32;

    function main() i32 {
        unsafe { return abs(-42); };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(FFITest, extern_declared_after_use) {
  // The block pre-pass must declare externs up front, so an extern may be
  // used before the declaration appears in the file.
  auto value = executeString(R"(
    function main() i32 {
        unsafe { return abs(-7); };
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
        unsafe { return abs(-3) + abs(-4); };
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(FFITest, extern_with_pointer_args_and_void_return) {
  auto value = executeString(R"(
    extern function malloc(size: i64) raw_ptr<u8>;
    extern function free(p: raw_ptr<u8>) void;

    function main() i32 {
        unsafe {
            var p = malloc(64);
            free(p);
        };
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
        unsafe { return abs(-11); };
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
        if (d > 99.0) { unsafe { return abs(-9); }; }
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

TEST(FFITest, extern_rejects_types_with_no_c_spelling) {
  // Arrays are fat pointers, interfaces are vtable pairs, lambdas are
  // closures — none has a C equivalent, so they must error rather than
  // silently miscompile.
  EXPECT_THROW(executeString(R"(
    interface I { function m() i32; }
    extern function f(p: I) i32;
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
        unsafe { return strlen("hello, ffi"); };
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(FFITest, static_ptr_variable_to_raw_ptr_param) {
  auto value = executeString(R"(
    extern function strlen(s: raw_ptr<u8>) i64;

    function main() i32 {
        var s: static_ptr<u8> = "via variable";
        unsafe { return strlen(s); };
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
        unsafe { return printf("abcde\n"); };
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
        unsafe { return printf("%d-%s-%d\n", 42, "xy", 7); };
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
        unsafe { return printf("%.2f\n", f); };
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
        unsafe { return abs(-42); };
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
        unsafe { return my_abs(-9); };
    }
  )");
  EXPECT_EQ(value, 9);
}

TEST(FFITest, extern_symbol_rename_with_abi_and_pointer) {
  auto value = executeString(R"(
    extern "C" function c_strlen(s: raw_ptr<u8>) i64 as "strlen";

    function main() i32 {
        unsafe { return c_strlen("abcdef"); };
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

// ============================================================================
// Linking native libraries (-l / -L)
// ============================================================================
// These use sun_ffi_testlib, whose symbols are absent from the test binary.
// Testing against libc would pass even if library loading did nothing.

namespace {

// Directory holding the built sun_ffi_testlib shared object, baked in by
// CMake. Empty when the define is absent (e.g. an ad-hoc build).
std::string ffiTestLibDir() {
#ifdef SUN_FFI_TESTLIB_DIR
  return SUN_FFI_TESTLIB_DIR;
#else
  return {};
#endif
}

}  // namespace

TEST(FFILinkTest, shell_quote_neutralises_metacharacters) {
  // -l/-L values reach the linker through std::system, so they must not be
  // able to inject shell syntax.
  EXPECT_EQ(sun::shellQuote("plain"), "'plain'");
  EXPECT_EQ(sun::shellQuote("a b"), "'a b'");
  EXPECT_EQ(sun::shellQuote("x; rm -rf /"), "'x; rm -rf /'");
  EXPECT_EQ(sun::shellQuote("$(id)"), "'$(id)'");
  // A quote must close, escape, and reopen so it cannot terminate the string.
  EXPECT_EQ(sun::shellQuote("it's"), "'it'\\''s'");
}

TEST(FFILinkTest, missing_library_is_reported_not_thrown) {
  sun::LinkOptions opts;
  opts.libraries = {"definitely_not_a_real_library_xyz"};
  auto failed = sun::loadDynamicLibraries(opts);
  ASSERT_EQ(failed.size(), 1u);
  EXPECT_EQ(failed[0], "definitely_not_a_real_library_xyz");
}

TEST(FFILinkTest, symbols_are_unavailable_before_the_library_is_loaded) {
  // Guards the test below: if these symbols were already reachable, loading
  // the library would prove nothing. Declared before the loading test since
  // dlopen is process-wide and irreversible (under ctest each test is its
  // own process anyway).
  EXPECT_EQ(llvm::sys::DynamicLibrary::SearchForAddressOfSymbol(
                "sun_ffi_triple"),
            nullptr);
}

TEST(FFILinkTest, loads_library_from_search_path_and_calls_into_it) {
  if (ffiTestLibDir().empty()) GTEST_SKIP() << "testlib dir unknown";

  sun::LinkOptions opts;
  opts.libraries = {"sun_ffi_testlib"};
  opts.searchPaths = {ffiTestLibDir()};
  auto failed = sun::loadDynamicLibraries(opts);
  ASSERT_TRUE(failed.empty()) << "could not load sun_ffi_testlib";

  auto value = executeString(R"(
    extern "C" function sun_ffi_triple(x: i32) i32;
    extern "C" function sun_ffi_sum(a: i64, b: i64) i64;

    function main() i32 {
        unsafe { return sun_ffi_triple(4) + sun_ffi_sum(10, 20); };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(FFILinkTest, loads_library_given_as_an_explicit_path) {
  if (ffiTestLibDir().empty()) GTEST_SKIP() << "testlib dir unknown";

  sun::LinkOptions opts;
  opts.libraries = {ffiTestLibDir() + "/libsun_ffi_testlib.so"};
  auto failed = sun::loadDynamicLibraries(opts);
  EXPECT_TRUE(failed.empty());
}

// ============================================================================
// `unsafe` gating
// ============================================================================
// Calling C leaves everything the type system and borrow checker guarantee,
// so it is gated exactly like the equivalent intrinsics (_malloc, _free, ...).

TEST(FFISafetyTest, extern_call_outside_unsafe_is_rejected) {
  EXPECT_THROW(executeString(R"(
    extern "C" function abs(x: i32) i32;
    function main() i32 { return abs(-5); }
  )"),
               std::exception);
}

TEST(FFISafetyTest, module_qualified_extern_call_outside_unsafe_is_rejected) {
  EXPECT_THROW(executeString(R"(
    module libc { extern "C" function abs(x: i32) i32; }
    function main() i32 { return libc.abs(-5); }
  )"),
               std::exception);
}

TEST(FFISafetyTest, extern_call_inside_unsafe_is_allowed) {
  auto value = executeString(R"(
    extern "C" function abs(x: i32) i32;
    function main() i32 {
        unsafe { return abs(-5); };
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(FFISafetyTest, safe_sun_wrapper_around_an_unsafe_call) {
  // The intended pattern: the unsafe boundary is contained in one wrapper,
  // and callers of the wrapper need no unsafe of their own.
  auto value = executeString(R"(
    extern "C" function abs(x: i32) i32;

    function safe_abs(x: i32) i32 {
        var r = 0;
        unsafe { r = abs(x); };
        return r;
    }

    function main() i32 { return safe_abs(-5) + safe_abs(3); }
  )");
  EXPECT_EQ(value, 8);
}

TEST(FFISafetyTest, declaring_an_extern_needs_no_unsafe) {
  // Only the call is unsafe; the declaration is inert.
  auto value = executeString(R"(
    extern "C" function abs(x: i32) i32;
    function main() i32 { return 1; }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// Struct pointers: `ref T` is C's `T*`
// ============================================================================
// Sun class layout already matches C (declaration order, natural padding) and
// `ref T` already lowers to a bare pointer, so this needs no ABI coercion.

TEST(FFIStructTest, ref_param_accepted_in_extern_signature) {
  auto value = executeString(R"(
    class TS { var sec: i64; var nsec: i64; }
    extern "C" function some_c_fn(t: ref TS) void;
    function main() i32 { return 4; }
  )");
  EXPECT_EQ(value, 4);
}

TEST(FFIStructTest, c_writes_through_a_ref_to_a_sun_object) {
  if (ffiTestLibDir().empty()) GTEST_SKIP() << "testlib dir unknown";
  sun::LinkOptions opts;
  opts.libraries = {"sun_ffi_testlib"};
  opts.searchPaths = {ffiTestLibDir()};
  ASSERT_TRUE(sun::loadDynamicLibraries(opts).empty());

  auto value = executeString(R"(
    class TS { var sec: i64; var nsec: i64; }
    extern "C" function sun_ffi_fill(t: ref TS) void;

    function main() i32 {
        var t: TS = { sec: 0, nsec: 0 };
        unsafe { sun_ffi_fill(t); };
        return t.sec + t.nsec;
    }
  )");
  EXPECT_EQ(value, 16);  // C writes sec=7, nsec=9
}

TEST(FFIStructTest, ref_return_is_still_rejected) {
  // Sun's `ref` return has auto-deref semantics with no C equivalent;
  // raw_ptr<T> is the way to return a pointer.
  EXPECT_THROW(executeString(R"(
    class P { var x: i32; }
    extern "C" function f(x: i32) ref P;
    function main() i32 { return 0; }
  )"),
               std::exception);
}

// ============================================================================
// Externs through .moon libraries
// ============================================================================
// A bodyless declaration used to deserialize as an ordinary Sun function with
// an empty body, which meant it picked up Sun name mangling and lost the C
// symbol. See ModuleTest for the plain module-scope cases.

TEST(FFIMoonTest, extern_survives_serialization_roundtrip) {
  using namespace sun::serialization;

  auto parser = Parser::createStringParser(R"(
    extern "C" function c_strlen(s: raw_ptr<u8>) i64 as "strlen";
    extern "C" function c_printf(fmt: raw_ptr<u8>, ...) i32 as "printf";
    declare function later(n: i32) bool;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 3u);

  ASTSerializer serializer;
  ASTDeserializer deserializer;
  auto roundTrip = [&](const ExprAST& e) {
    return deserializer.deserialize(serializer.serialize(e));
  };

  auto* strlenFn =
      static_cast<FunctionAST*>(roundTrip(*ast->getBody()[0]).release());
  ASSERT_NE(strlenFn, nullptr);
  EXPECT_TRUE(strlenFn->isExtern());
  EXPECT_TRUE(strlenFn->isCExtern());
  EXPECT_TRUE(strlenFn->getProto().hasLinkName());
  EXPECT_EQ(strlenFn->getProto().getLinkName(), "strlen");

  auto* printfFn =
      static_cast<FunctionAST*>(roundTrip(*ast->getBody()[1]).release());
  EXPECT_TRUE(printfFn->isCExtern());
  EXPECT_TRUE(printfFn->getProto().isCVariadic());

  // `declare` is bodyless too but is NOT a C extern; conflating them would
  // strip Sun mangling from a forward declaration.
  auto* declareFn =
      static_cast<FunctionAST*>(roundTrip(*ast->getBody()[2]).release());
  EXPECT_TRUE(declareFn->isExtern());
  EXPECT_FALSE(declareFn->isCExtern());
  EXPECT_FALSE(declareFn->getProto().hasLinkName());

  delete strlenFn;
  delete printfFn;
  delete declareFn;
}

// ============================================================================
// Structs by value across the C boundary
// ============================================================================
// Each case exercises a different System V classification outcome. The C side
// is compiled by cc, so a mismatch between our classification and the real
// ABI shows up as a wrong value rather than a compile error.

namespace {

// Loads the fixture library once; the tests below all need its symbols.
bool loadFfiTestLib() {
  if (ffiTestLibDir().empty()) return false;
  sun::LinkOptions opts;
  opts.libraries = {"sun_ffi_testlib"};
  opts.searchPaths = {ffiTestLibDir()};
  return sun::loadDynamicLibraries(opts).empty();
}

}  // namespace

TEST(FFIStructValueTest, passes_a_register_class_struct) {
  // struct { int, int } travels in one integer register.
  if (!loadFfiTestLib()) GTEST_SKIP() << "fixture library unavailable";
  auto value = executeString(R"(
    class Pair {
        var a: i32;
        var b: i32;
        function init(a: i32, b: i32) { this.a = a; this.b = b; }
    }
    extern "C" function sun_ffi_take_pair(p: Pair) i32;

    function main() i32 {
        var p = Pair(3, 7);
        unsafe { return sun_ffi_take_pair(p); };
    }
  )");
  EXPECT_EQ(value, 307);  // a*100 + b
}

TEST(FFIStructValueTest, passes_a_mixed_integer_and_sse_struct) {
  // struct { int, double } splits across an integer and an SSE register.
  if (!loadFfiTestLib()) GTEST_SKIP() << "fixture library unavailable";
  auto value = executeString(R"(
    class Mixed {
        var a: i32;
        var b: f64;
        function init(a: i32, b: f64) { this.a = a; this.b = b; }
    }
    extern "C" function sun_ffi_take_mixed(m: Mixed) i32;

    function main() i32 {
        var m = Mixed(5, 37.0);
        unsafe { return sun_ffi_take_mixed(m); };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(FFIStructValueTest, passes_a_memory_class_struct_byval) {
  // 20 bytes exceeds two eightbytes, so it goes through memory.
  if (!loadFfiTestLib()) GTEST_SKIP() << "fixture library unavailable";
  auto value = executeString(R"(
    class Big {
        var a: i32; var b: i32; var c: i32; var d: i32; var e: i32;
        function init(a: i32, b: i32, c: i32, d: i32, e: i32) {
            this.a = a; this.b = b; this.c = c; this.d = d; this.e = e;
        }
    }
    extern "C" function sun_ffi_take_big(b: Big) i32;

    function main() i32 {
        var b = Big(1, 2, 3, 4, 5);
        unsafe { return sun_ffi_take_big(b); };
    }
  )");
  EXPECT_EQ(value, 15);
}

TEST(FFIStructValueTest, returns_a_register_class_struct) {
  if (!loadFfiTestLib()) GTEST_SKIP() << "fixture library unavailable";
  auto value = executeString(R"(
    class Pair {
        var a: i32;
        var b: i32;
        function init(a: i32, b: i32) { this.a = a; this.b = b; }
    }
    extern "C" function sun_ffi_make_pair(a: i32, b: i32) Pair;

    function main() i32 {
        // Initialization, not assignment: assigning a returned struct to an
        // existing variable is broken in Sun generally (it stores the
        // pointer), unrelated to the C boundary.
        unsafe {
            var p = sun_ffi_make_pair(6, 7);
            return p.a * p.b;
        };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(FFIStructValueTest, returns_a_memory_class_struct_via_sret) {
  if (!loadFfiTestLib()) GTEST_SKIP() << "fixture library unavailable";
  auto value = executeString(R"(
    class Big {
        var a: i32; var b: i32; var c: i32; var d: i32; var e: i32;
        function init(a: i32, b: i32, c: i32, d: i32, e: i32) {
            this.a = a; this.b = b; this.c = c; this.d = d; this.e = e;
        }
    }
    extern "C" function sun_ffi_make_big(base: i32) Big;

    function main() i32 {
        unsafe {
            var b = sun_ffi_make_big(10);
            return b.a + b.b + b.c + b.d + b.e;
        };
    }
  )");
  EXPECT_EQ(value, 60);  // 10+11+12+13+14
}

TEST(FFIStructValueTest, struct_followed_by_a_scalar_keeps_argument_order) {
  // A coerced struct expands into extra LLVM arguments, so the scalar after
  // it must still land in the right slot.
  if (!loadFfiTestLib()) GTEST_SKIP() << "fixture library unavailable";
  auto value = executeString(R"(
    class Pair {
        var a: i32;
        var b: i32;
        function init(a: i32, b: i32) { this.a = a; this.b = b; }
    }
    extern "C" function sun_ffi_pair_then_int(p: Pair, extra: i32) i32;

    function main() i32 {
        var p = Pair(1, 2);
        unsafe { return sun_ffi_pair_then_int(p, 39); };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(FFIStructValueTest, module_qualified_extern_call_marshals_structs) {
  // The module-qualified call path (mymod.f) used to skip ABI marshalling
  // entirely and hand the callee a raw {i32, i32} argument.
  if (!loadFfiTestLib()) GTEST_SKIP() << "fixture library unavailable";
  auto value = executeString(R"(
    class Pair {
        var a: i32;
        var b: i32;
        function init(a: i32, b: i32) { this.a = a; this.b = b; }
    }
    module clib {
      extern "C" function sun_ffi_take_pair(p: Pair) i32;
    }

    function main() i32 {
        var p = Pair(3, 7);
        unsafe { return clib.sun_ffi_take_pair(p); };
    }
  )");
  EXPECT_EQ(value, 307);  // a*100 + b
}

TEST(FFIStructValueTest, predeclared_function_still_registers_marshalling) {
  // declare() used to return early for a Function that already existed (e.g.
  // created by .moon bitcode linking) without registering its lowering, so
  // needsMarshalling() silently answered no.
  auto parser = Parser::createStringParser(R"(
    extern "C" function pre_pair(p: i32) i32;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  auto* fn = static_cast<FunctionAST*>(ast->getBody()[0].get());
  const PrototypeAST& proto = fn->getProto();

  CodegenContext cgctx("predecl_test", nullptr);
  llvm::LLVMContext& lctx = cgctx.getContext();
  llvm::Module* module = cgctx.mainModule.get();
  sun::cabi::ExternCEmitter emitter(cgctx, module);

  llvm::Type* i32Ty = llvm::Type::getInt32Ty(lctx);
  auto* pairTy = llvm::StructType::get(lctx, {i32Ty, i32Ty});
  llvm::Type* params[] = {pairTy};

  // Pre-create the function with the lowered type, as bitcode linking would.
  auto lowering =
      sun::abi::lowerCSignature(llvm::Triple(module->getTargetTriple()),
                                i32Ty, params, module->getDataLayout());
  auto* loweredTy =
      sun::abi::buildLoweredFunctionType(lowering, lctx, /*isVarArg=*/false);
  llvm::Function::Create(loweredTy, llvm::Function::ExternalLinkage,
                         "pre_pair", module);

  llvm::Function* declared = emitter.declare(proto, i32Ty, params);
  ASSERT_NE(declared, nullptr);
  EXPECT_TRUE(emitter.needsMarshalling(declared));
}
