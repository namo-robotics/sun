// tests/functions/generic/test_variadics.cpp - Tests for value packs on free
// generic functions: `args...`, `args...: _params_of<T>`, and the expansion
// `f(args...)`.
//
// A pack is monomorphized per argument tuple, so most of what these pin is
// that two call sites at different arities become two functions rather than
// colliding on whichever was compiled first. Packs on class methods are
// covered by tests/memory_safety/test_allocator.cpp.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "driver/driver.h"
#include "driver/execution_utils.h"
#include "moon_bundling/moon_builder.h"

namespace {

// A free factory over a pack, the shape `HeapAllocator.create<T>` has as a
// method. Prepended to most of the sources below.
constexpr const char* kMake = R"(
    function make<T>(args...: _params_of<T>) raw_ptr<T> {
        var size: i64 = _sizeof<T>();
        var memory: raw_ptr<i8> = unsafe { _malloc(size); };
        _init<T>(memory, args...);
        return memory;
    }
)";

// A class with two constructors, so an argument tuple has something to pick.
constexpr const char* kPoint = R"(
    class Point {
        var x: i32;
        var y: i32;
        function init(x: i32, y: i32) { this.x = x; this.y = y; }
        function init(v: i32) { this.x = v; this.y = v; }
        function sum() i32 { return this.x + this.y; }
    }
)";

std::string source(const std::string& body) {
  return std::string(kPoint) + kMake + body;
}

}  // namespace

// -------------------------------------------------------------------
// The basic shape
// -------------------------------------------------------------------

TEST(Functions_Generic_Variadics, free_function_forwards_pack_to_init) {
  auto value = executeString(source(R"(
    function main() i32 {
        var p = make<Point>(3, 4);
        return unsafe { p.sum(); };
    }
  )"));
  EXPECT_EQ(value, 7);
}

// One call site used at two arities must produce two specializations, each
// selecting the init overload its own arguments match. The free-function twin
// of MemorySafety_Allocator.create_selects_init_overload_by_args.
TEST(Functions_Generic_Variadics, arity_selects_init_overload) {
  auto value = executeString(source(R"(
    function main() i32 {
        var a = make<Point>(3, 4);   // init(i32, i32) -> 7
        var b = make<Point>(9);      // init(i32)      -> 18
        return unsafe { a.sum() + b.sum(); };
    }
  )"));
  EXPECT_EQ(value, 25);
}

TEST(Functions_Generic_Variadics, empty_pack) {
  auto value = executeString(R"(
    class Answer { var v: i32; function init() { this.v = 42; } }
    function make<T>(args...: _params_of<T>) raw_ptr<T> {
        var size: i64 = _sizeof<T>();
        var memory: raw_ptr<i8> = unsafe { _malloc(size); };
        _init<T>(memory, args...);
        return memory;
    }
    function main() i32 {
        var a = make<Answer>();
        return unsafe { a.v; };
    }
  )");
  EXPECT_EQ(value, 42);
}

// -------------------------------------------------------------------
// Fixed parameters before the pack
// -------------------------------------------------------------------

// Only what is left after the declared parameters fills the pack. Getting the
// split wrong pushes `factor` into the pack and leaves Point without its
// arguments.
TEST(Functions_Generic_Variadics, fixed_params_precede_the_pack) {
  auto value = executeString(source(R"(
    function scaled<T>(factor: i32, args...: _params_of<T>) i32 {
        var p = make<T>(args...);
        return unsafe { factor * p.sum(); };
    }
    function main() i32 { return scaled<Point>(2, 3, 4); }
  )"));
  EXPECT_EQ(value, 14);
}

TEST(Functions_Generic_Variadics, too_few_arguments_for_fixed_params) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(source(R"(
    function scaled<T>(factor: i32, args...: _params_of<T>) i32 {
        var p = make<T>(args...);
        return unsafe { factor * p.sum(); };
    }
    function main() i32 { return scaled<Point>(); }
  )")),
                                "expects at least 1 argument");
}

// -------------------------------------------------------------------
// Forwarding
// -------------------------------------------------------------------

TEST(Functions_Generic_Variadics, pack_forwarded_between_generic_functions) {
  auto value = executeString(source(R"(
    function relay<T>(args...: _params_of<T>) raw_ptr<T> {
        return make<T>(args...);
    }
    function main() i32 {
        var a = relay<Point>(3, 4);
        var b = relay<Point>(9);
        return unsafe { a.sum() + b.sum(); };
    }
  )"));
  EXPECT_EQ(value, 25);
}

// -------------------------------------------------------------------
// A pack makes a function a template on its own
// -------------------------------------------------------------------

// No `<T>` of its own: the pack alone is what makes this a template, and it
// is still emitted once per argument tuple.
TEST(Functions_Generic_Variadics, pack_without_type_parameters) {
  auto value = executeString(source(R"(
    function build(args...: _params_of<Point>) raw_ptr<Point> {
        return make<Point>(args...);
    }
    function main() i32 {
        var a = build(3, 4);
        var b = build(9);
        return unsafe { a.sum() + b.sum(); };
    }
  )"));
  EXPECT_EQ(value, 25);
}

// The same, but borrowing T from an enclosing generic: `build` can only be
// defined once `outer` is specialized.
TEST(Functions_Generic_Variadics, pack_borrows_enclosing_type_parameter) {
  auto value = executeString(source(R"(
    function outer<T>() i32 {
        function build(args...: _params_of<T>) i32 {
            var p = make<T>(args...);
            return unsafe { p.sum(); };
        }
        return build(3, 4) + build(9);
    }
    function main() i32 { return outer<Point>(); }
  )"));
  EXPECT_EQ(value, 25);
}

// -------------------------------------------------------------------
// Inferred type arguments
// -------------------------------------------------------------------

// No `<...>` written: the type argument comes from the fixed arguments, and
// everything past them fills the pack. This is the shape stdlib `spawn` takes.
TEST(Functions_Generic_Variadics, type_argument_inferred_from_fixed_arg) {
  auto value = executeString(R"(
    function apply<F: _Lambda>(f: F, args...: _params_of<F>) i32 {
        return f(args...);
    }
    function main() i32 {
        var add = lambda (a: i32, b: i32) i32 { return a + b; };
        var neg = lambda (a: i32) i32 { return 0 - a; };
        return apply(add, 3, 4) + apply(neg, 5);
    }
  )");
  EXPECT_EQ(value, 2);
}

// -------------------------------------------------------------------
// _params_of<T> checking
// -------------------------------------------------------------------

TEST(Functions_Generic_Variadics, no_matching_constructor_is_an_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      executeString(source(R"(
    function main() i32 { var p = make<Point>(1, 2, 3); return 0; }
  )")),
      "No matching constructor for 'Point' with arguments (i32, i32, i32)");
}

TEST(Functions_Generic_Variadics, lambda_params_arity_mismatch_is_an_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function apply<F: _Lambda>(f: F, args...: _params_of<F>) i32 {
        return f(args...);
    }
    function main() i32 {
        var add = lambda (a: i32, b: i32) i32 { return a + b; };
        return apply(add, 3);
    }
  )"),
                                "which takes (i32, i32); got (i32)");
}

// An unannotated pack takes whatever the call passes; so does an annotation
// naming something that is neither a class nor a lambda. Both are recorded
// and left unchecked.
TEST(Functions_Generic_Variadics, unannotated_pack_is_unchecked) {
  auto value = executeString(R"(
    function total(args...) i32 { return 0; }
    function main() i32 { return total(1, true, 'c') + 7; }
  )");
  EXPECT_EQ(value, 7);
}

TEST(Functions_Generic_Variadics, params_of_a_primitive_is_unchecked) {
  auto value = executeString(R"(
    function total<T>(args...: _params_of<T>) i32 { return 0; }
    function main() i32 { return total<i32>(1, 2, 3) + 7; }
  )");
  EXPECT_EQ(value, 7);
}

// -------------------------------------------------------------------
// Bundles
// -------------------------------------------------------------------

// A pack template carried in a .moon must still specialize on the importer's
// side, including at an arity the bundle itself never used.
TEST(Functions_Generic_Variadics, pack_template_survives_a_moon_round_trip) {
  initTestEnvironment();
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "sun_variadic_moon_test";
  fs::create_directories(dir);
  fs::path libSrc = dir / "packlib.sun";
  {
    std::ofstream out(libSrc);
    out << R"(
      public module packlib {
          public class Point {
              var x: i32;
              var y: i32;
              public function init(x: i32, y: i32) { this.x = x; this.y = y; }
              public function init(v: i32) { this.x = v; this.y = v; }
              public function sum() i32 { return this.x + this.y; }
          }
          public function make<T>(args...: _params_of<T>) raw_ptr<T> {
              var size: i64 = _sizeof<T>();
              var memory: raw_ptr<i8> = unsafe { _malloc(size); };
              _init<T>(memory, args...);
              return memory;
          }
      }
    )";
  }
  fs::path moonPath = dir / "packlib.moon";
  sun::MoonBuilder::build(libSrc.string(), moonPath);

  auto driver = Driver::createForJIT("variadic_moon_main");
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using packlib;
    function main() i32 {
        var a = make<Point>(3, 4);
        var b = make<Point>(9);
        return unsafe { a.sum() + b.sum(); };
    }
  )");
  EXPECT_EQ(value, 25);
}

// -------------------------------------------------------------------
// _return_type_of<F>
// -------------------------------------------------------------------
// A type computed from another type rather than named outright. Inside the
// template F is still standing for itself, so the return type stays symbolic
// until a call says what F is. This is what lets stdlib `spawn` say it hands
// back a handle to whatever its lambda returns.

TEST(Functions_Generic_Variadics, return_type_of_follows_the_lambda) {
  auto value = executeString(R"(
    function run<F: _Lambda>(f: F, args...: _params_of<F>) _return_type_of<F> {
        return f(args...);
    }
    function main() i32 {
        var add = lambda (a: i32, b: i32) i32 { return a + b; };
        var one = lambda (a: i32) i32 { return a; };
        return run(add, 40, 2) - run(one, 1) + 1;
    }
  )");
  EXPECT_EQ(value, 42);
}

// Two lambdas returning different types make two specializations, each with
// its own return type — the point of computing it rather than declaring it.
TEST(Functions_Generic_Variadics, return_type_of_differs_per_specialization) {
  auto value = executeString(R"(
    function run<F: _Lambda>(f: F, args...: _params_of<F>) _return_type_of<F> {
        return f(args...);
    }
    function main() i32 {
        var small = lambda (a: i32) i32 { return a; };
        var wide = lambda (a: i64) i64 { return a * 2; };
        var w: i64 = run(wide, 20);
        return run(small, 2) + _convert<i32>(w);
    }
  )");
  EXPECT_EQ(value, 42);
}

// As a type argument to a generic class, which is the shape spawn's
// `Thread<_return_type_of<F>>` takes.
TEST(Functions_Generic_Variadics, return_type_of_as_a_type_argument) {
  auto value = executeString(R"(
    class Box<T> {
        public var v: T;
        public function init(v: T) { this.v = v; }
    }
    function boxed<F: _Lambda>(f: F, args...: _params_of<F>)
        Box<_return_type_of<F>> {
        return Box<_return_type_of<F>>(f(args...));
    }
    function main() i32 {
        var add = lambda (a: i32, b: i32) i32 { return a + b; };
        var b = boxed(add, 40, 2);
        return b.v;
    }
  )");
  EXPECT_EQ(value, 42);
}

// A void lambda gives a void return type, so the forwarding call is a
// `return <void expression>` — legal, and the only value is the effect.
TEST(Functions_Generic_Variadics, return_type_of_a_void_lambda_is_void) {
  auto value = executeString(R"(
    var counter: i32 = 0;
    function run<F: _Lambda>(f: F, args...: _params_of<F>) _return_type_of<F> {
        return f(args...);
    }
    function main() i32 {
        var bump = lambda (n: i32) void { counter = counter + n; };
        run(bump, 40);
        run(bump, 2);
        return counter;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Functions_Generic_Variadics, return_type_of_a_non_lambda_is_an_error) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    function run<T>(args...: _params_of<T>) _return_type_of<T> {
        return 0;
    }
    function main() i32 { return run<i32>(1); }
  )"),
                                "requires a lambda or function type");
}
