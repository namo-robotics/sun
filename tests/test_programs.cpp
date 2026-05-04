// tests/test_programs.cpp

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

TEST(ProgramTest, vars) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 1;
        return x + 2;
    };
  )");
  EXPECT_EQ(value, 3);
}

TEST(ProgramTest, fib) {
  auto value = executeString(R"(
function fib(n: i32) i32 {
        if (n < 2) {
            return n;
        } else {
            return fib(n-1) + fib(n-2);
        }
    };

    function main() i32 {
        return fib(10);
    };
  )");
  EXPECT_EQ(value, 55);
}

TEST(ProgramTest, mutual_recursion) {
  auto value = executeString(R"(
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

TEST(ProgramTest, square_lambda) {
  auto value = executeString(R"(
    var square = lambda (x: i32) i32 {
        return x*x;
    };

    function main() i32 {
        return square(2);
    };
  )");
  EXPECT_EQ(value, 4);
}

TEST(ProgramTest, square_func) {
  auto value = executeString(R"(
    function square(x: i32) i32 {
        return x*x;
    };

    function main() i32 {
        return square(2);
    };
  )");
  EXPECT_EQ(value, 4);
}

TEST(ProgramTest, square_func_with_closure) {
  auto value = executeString(R"(
    function main() i32 {
        var a: i32 = 1;
        function square(x: i32) i32 {
            return x*x + a;
        };
        return square(2);
    };
  )");
  EXPECT_EQ(value, 5);
}

TEST(ProgramTest, foo_bar) {
  auto value = executeString(R"(
    function foo(x: i32) i32 {
        return x*x;
    };

    function bar(x: i32) i32 {
        return foo(x)*x;
    };

    function main() i32 {
        var y: i32 = bar(3);
        var z: i32 = y + 1;
        return z + 1;
    };
  )");
  EXPECT_EQ(value, 29);
}

TEST(ProgramTest, nested_func) {
  auto value = executeString(R"(
    var f = lambda (x: i32) i32 {
        var g = lambda (y: i32) i32 {
            return y+2;
        };
        return g(x+1);
    };

    function main() i32 {
        return f(0);
    };
  )");
  EXPECT_EQ(value, 3);
}

TEST(ProgramTest, func_not_declared) {
  EXPECT_THROW(executeString(R"(
    var foo = lambda (x) {
        return bar(x);
    };
    var bar = lambda (x) {
        return x+1;
    };

    function main() i32 {
        return foo(0);
    };
  )"),
               SunError);
}

TEST(ProgramTest, closure) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 1;
        var f = lambda () i32 {
            return x+1;
        };
        return f();
    };
  )");
  EXPECT_EQ(value, 2);
}

TEST(ProgramTest, function_var) {
  // Functions cannot be assigned to variables - only lambdas can
  EXPECT_THROW(executeString(R"(
    function main() i32 {
        var x: i32 = 1;
        var f = function foo() i32 {
            return x+1;
        };
        return f();
    };
  )"),
               SunError);
}

TEST(ProgramTest, closure2) {
  auto value = executeString(R"(
    var x: i32 = 1;
    var f = lambda () i32 {
        x = x + 1;
        return x;
    };

    function main() i32 {
        f();
        return x;
    };
  )");
  EXPECT_EQ(value, 2);
}

TEST(ProgramTest, global_var) {
  auto value = executeString(R"(
    var x: i32 = 9;
    var f = lambda () i32 {
        return x+2;
    };

    function main() i32 {
        return f();
    };
  )");
  EXPECT_EQ(value, 11);
}

TEST(ProgramTest, var_assign) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 1;
        x = 2;
        return x;
    };
  )");
  EXPECT_EQ(value, 2);
}

// First-class function tests
TEST(ProgramTest, func_assign) {
  auto value = executeString(R"(
    var add = lambda (x: i32, y: i32) i32 {
        return x + y;
    };

    function main() i32 {
        var result: i32 = add(3, 4);
        return result;
    };
  )");
  EXPECT_EQ(value, 7);
}

TEST(ProgramTest, func_reassign) {
  auto value = executeString(R"(
    var f = lambda () i32 {
        return 1;
    };

    function main() i32 {
        f = lambda () i32 {
            return 2;
        };
        return f();
    };
  )");
  EXPECT_EQ(value, 2);
}

TEST(ProgramTest, func_reassign2) {
  auto value = executeString(R"(
    var f = lambda () i32 {
        return 1;
    };
    function main() i32 {
        var a = f;
        return a();
    };
  )");
  EXPECT_EQ(value, 1);
}

TEST(ProgramTest, func_reassign3) {
  auto value = executeString(R"(
    var f = lambda () i32 {
        return 1;
    };
    var g = lambda () i32 {
        return 2;
    };
    function main() i32 {
        var a = f;
        a = g;
        return a();
    };
  )");
  EXPECT_EQ(value, 2);
}

TEST(ProgramTest, func_pass) {
  auto value = executeString(R"(
    var apply = lambda (f: (i32) i32, x: i32) i32 {
        return f(x);
    };

    var double_it = lambda (n: i32) i32 {
        return n * 2;
    };

    function main() i32 {
        return apply(double_it, 7);
    };
  )");
  EXPECT_EQ(value, 14);
}

TEST(ProgramTest, pass_named_function) {
  auto value = executeString(R"(
    var apply = lambda (f: _(i32) i32, x: i32) i32 {
        return f(x);
    };

function double(n: i32) i32 {
        return n * 2;
    };

    function main() i32 {
        return apply(double, 7);
    };
  )");
  EXPECT_EQ(value, 14);
}

TEST(ProgramTest, func_return) {
  auto value = executeString(R"(
    var f = lambda () () i32 {
        var g = lambda () i32 {
            return 2;
        };
        return g;
    };

    function main() i32 {
        var y = f();
        return y();
    };
  )");
  EXPECT_EQ(value, 2);
}

TEST(ProgramTest, print_i32) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    function main() i32 {
        print_i32(42);
        return 100;
    };
  )");
  EXPECT_EQ(value, 100);
}

TEST(ProgramTest, println_i32) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    function main() i32 {
        println_i32(123);
        println_i32(-456);
        return 200;
    };
  )");
  EXPECT_EQ(value, 200);
}

TEST(ProgramTest, print_multiple) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    function main() i32 {
        var x: i32 = 10;
        print_i32(x);
        print_newline();
        x = x + 5;
        println_i32(x);
        return x;
    };
  )");
  EXPECT_EQ(value, 15);
}

TEST(ProgramTest, hello_world) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    function main() i32 {
        println("hello world");
        return 0;
    };
  )");
  EXPECT_EQ(value, 0);
}

TEST(ProgramTest, string_variable) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    function main() i32 {
        var greeting: static_ptr<u8> = "Hello, Sun!";
        println(greeting);
        return 0;
    };
  )");
  EXPECT_EQ(value, 0);
}

TEST(ProgramTest, return_bool_true) {
  auto value = executeString(R"(
    function main() bool {
        return 1 < 2;
    };
  )");
  EXPECT_EQ(value, true);
}

TEST(ProgramTest, return_bool_false) {
  auto value = executeString(R"(
    function main() bool {
        return 2 < 1;
    };
  )");
  EXPECT_EQ(value, false);
}

TEST(ProgramTest, return_i64) {
  auto value = executeString(R"(
    function main() i64 {
        return 42;
    };
  )");
  // Verify the return type is i64 (even though value fits in i32)
  EXPECT_TRUE(std::holds_alternative<int64_t>(value));
  EXPECT_EQ(std::get<int64_t>(value), 42);
}

TEST(ProgramTest, return_f32) {
  auto value = executeString(R"(
    function main() f32 {
        return 3.14;
    };
  )");
  EXPECT_TRUE(std::holds_alternative<float>(value));
  EXPECT_NEAR(std::get<float>(value), 3.14f, 0.01f);
}

TEST(ProgramTest, return_f64) {
  auto value = executeString(R"(
    function main() f64 {
        return 2.718281828;
    };
  )");
  EXPECT_TRUE(std::holds_alternative<double>(value));
  EXPECT_NEAR(std::get<double>(value), 2.718281828, 0.0001);
}

TEST(ProgramTest, return_string) {
  auto value = executeString(R"(
    function main() static_ptr<u8> {
        return "hello";
    };
  )");
  EXPECT_TRUE(std::holds_alternative<std::string>(value));
  EXPECT_EQ(std::get<std::string>(value), "hello");
}

// Tests for optional command-line arguments in main

TEST(ProgramTest, main_with_argc_argv_returns_argc) {
  // Test that main can accept argc and argv parameters
  // argv[0] = "test_prog", argv[1] = "arg1", argv[2] = "arg2"
  const char* args[] = {"test_prog", "arg1", "arg2", nullptr};
  auto value = executeString(
      R"(
    function main(argc: i32, argv: raw_ptr<raw_ptr<i8>>) i32 {
        return argc;
    };
  )",
      3, const_cast<char**>(args));
  EXPECT_EQ(value, 3);
}

TEST(ProgramTest, main_with_argc_argv_no_args) {
  // Test main with argc/argv when no arguments are passed
  const char* args[] = {"test_prog", nullptr};
  auto value = executeString(
      R"(
    function main(argc: i32, argv: raw_ptr<raw_ptr<i8>>) i32 {
        return argc;
    };
  )",
      1, const_cast<char**>(args));
  EXPECT_EQ(value, 1);
}

TEST(ProgramTest, raw_ptr_type_parsing) {
  // Test that raw_ptr type can be parsed in function parameters
  auto value = executeString(R"(
    function foo(p: raw_ptr<i32>) i32 {
        return 42;
    };

    function main() i32 {
        return 42;
    };
  )");
  EXPECT_EQ(value, 42);
}

TEST(ProgramTest, main_void_return) {
  // Test that function main() can have no return type (defaults to void)
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    function main() {
        println("Hello from void main!");
    };
  )");
  // function main() with no return type returns void
  EXPECT_TRUE(std::holds_alternative<sun::VoidValue>(value));
}

TEST(ProgramTest, main_explicit_void_return) {
  // Test that function main() void works explicitly
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;
    function main() void {
        println("Explicit void return");
    };
  )");
  EXPECT_TRUE(std::holds_alternative<sun::VoidValue>(value));
}

// Reference tests
TEST(ProgramTest, ref_basic) {
  // Test basic reference - reading through a reference
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 42;
        ref r = x;
        return r;
    };
  )");
  EXPECT_EQ(value, 42);
}

TEST(ProgramTest, ref_modify_original) {
  // Test that modifying original variable is visible through reference
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        ref r = x;
        x = 20;
        return r;
    };
  )");
  EXPECT_EQ(value, 20);
}

TEST(ProgramTest, ref_modify_through_ref) {
  // Test that modifying through reference changes the original
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 10;
        ref r = x;
        r = 30;
        return x;
    };
  )");
  EXPECT_EQ(value, 30);
}

TEST(ProgramTest, ref_arithmetic) {
  // Test using reference in arithmetic
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 5;
        ref r = x;
        return r * 2 + 3;
    };
  )");
  EXPECT_EQ(value, 13);
}

TEST(ProgramTest, ref_to_global) {
  // Test reference to global variable
  auto value = executeString(R"(
    var g: i32 = 100;

    function main() i32 {
        ref r = g;
        r = 200;
        return g;
    };
  )");
  EXPECT_EQ(value, 200);
}

TEST(ProgramTest, minimal_transitive_import) {
  // Test minimal transitive import
  // main imports a which imports b
  auto value = executeString(R"(
      
    import "tests/programs/minimal_import_a.sun";

    function main() i32 {
        return a();
    };
  )");
  EXPECT_EQ(value, 1);
}

TEST(ProgramTest, minimal_transitive_import_error) {
  // Test minimal transitive import
  // main imports a which imports b
  EXPECT_THROW(executeString(R"(
      
    import "tests/programs/minimal_import_a.sun";

    function main() i32 {
        return b();
    };
  )"),
               SunError);
}