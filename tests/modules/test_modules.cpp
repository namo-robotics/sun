// tests/modules/test_modules.cpp
// Tests for the module system

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "codegen.h"
#include "codegen_visitor.h"
#include "execution_utils.h"
#include "lexer.h"
#include "moon_builder.h"
#include "parser.h"
#include "semantic_analyzer.h"

// === Module declaration tests ===

TEST(Modules, parse_module_declaration) {
  auto parser = Parser::createStringParser(R"(
    public module math {
      public function square(x: i32) i32 {
        return x * x;
      }
    }
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::MODULE);

  auto* moduleNode = static_cast<const ModuleAST*>(ast->getBody()[0].get());
  EXPECT_EQ(moduleNode->getName(), "math");
}

TEST(Modules, module_with_using_all) {
  // Test: module math { function square(x: i32) i32 { return x * x; } }
  //       using math;
  //       function main() i32 { return square(5); }
  auto value = executeString(R"(
    public module math {
      public function square(x: i32) i32 {
        return x * x;
      }
    }
    using math;
    function main() i32 {
      return square(5);
    }
  )");
  EXPECT_EQ(value, 25);
}

TEST(Modules, module_with_using_specific) {
  // Test: using math.square; to import only one symbol
  auto value = executeString(R"(
    public module math {
      public function square(x: i32) i32 {
        return x * x;
      }
      public function cube(x: i32) i32 {
        return x * x * x;
      }
    }
    using math.square;
    function main() i32 {
      return square(3);
    }
  )");
  EXPECT_EQ(value, 9);
}

TEST(Modules, parse_using_wildcard) {
  auto parser = Parser::createStringParser(R"(
    using sun;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::USING);

  auto* usingNode = static_cast<const UsingAST*>(ast->getBody()[0].get());
  EXPECT_TRUE(usingNode->isModuleImport());
  EXPECT_EQ(usingNode->getNamespacePathString(), "sun");
}

TEST(Modules, parse_using_specific_symbol) {
  auto parser = Parser::createStringParser(R"(
    using sun.Vec;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::USING);

  auto* usingNode = static_cast<const UsingAST*>(ast->getBody()[0].get());
  EXPECT_FALSE(usingNode->isModuleImport());
  EXPECT_EQ(usingNode->getNamespacePathString(), "sun");
  EXPECT_EQ(usingNode->getTarget(), "Vec");
}

TEST(Modules, parse_using_nested_module) {
  auto parser = Parser::createStringParser(R"(
    using sun.matrix.types;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::USING);

  auto* usingNode = static_cast<const UsingAST*>(ast->getBody()[0].get());
  EXPECT_FALSE(usingNode->isModuleImport());
  EXPECT_EQ(usingNode->getNamespacePathString(), "sun.matrix");
  EXPECT_EQ(usingNode->getTarget(), "types");
}

TEST(Modules, module_with_class_method) {
  // Test module with class and method call
  auto value = executeString(R"(
    public module mymod {
      public class ClassA {
        public var value: i32;
        public function init(v: i32) {
          this.value = v;
        }
        public function foo() i32 {
          return this.value * 2;
        }
      }
    }
    using mymod;
    function main() i32 {
      var a = ClassA(21);
      return a.foo();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Modules, nested_modules_nested_classes_method_chain) {
  // Test: mod_x.mod_y.ClassA.ClassB.foo()
  // Nested modules with nested classes and method call chain
  auto value = executeString(R"(
    public module mod_x {
      public module mod_y {
    
        public class ClassB {
          public var val: i32;
          public function init(v: i32) {
            this.val = v;
          }
          public function foo() i32 {
            return this.val;
          }
        }

        public class ClassA {
          public var b: ClassB;
          public function init() {
            this.b = ClassB(42);
          }
        }

        public var a = ClassA();
      }
    }

    function main() i32 {
      return mod_x.mod_y.a.b.foo();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Modules, same_module_in_multiple_files_merges) {
  // Two declarations of the same module merge their functions
  auto value = executeString(R"(
    public module mymod {
      public function foo() i32 { return 1; }
    }
    public module mymod {
      public function bar() i32 { return 2; }
    }

    function main() i32 {
      return mymod.foo() + mymod.bar();
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Modules, nested_module_qualified_function_call) {
  // Call functions in nested modules using qualified names without using
  auto value = executeString(R"(
    public module A {
      public module B {
        public function foo() i32 { return 2; }
      }
    }
    public module B {
      public module A {
        public function foo() i32 { return 1; }
      }
    }

    function main() i32 {
      return A.B.foo() + B.A.foo();
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Modules, submod_fn_calls_mod_fn) {
  // Submodule function can call parent module function
  auto value = executeString(R"(
    public module A {
      public function foo() i32 { return 1; }
      public module B {
        public function bar() i32 { return 1; }
      }
    }

    function main() i32 {
      return A.foo() + A.B.bar();
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Modules, submod_duplicates_fn) {
  // When two modules define the same symbol and both are wildcard imported,
  // using the unqualified name is ambiguous and errors
  EXPECT_THROW(executeString(R"(
    public module A {
      public function foo() i32 {
        return 1;
      }
    }
    public module B {
      public function foo() i32 {
        return 2;
      }
    }
    using A;
    using B;
    function main() i32 {
      return foo();
    }
  )"),
               std::exception);
}

TEST(Modules, using_nest_module) {
  // "using A.B;" where B is a nested module should import all from A.B
  auto value = executeString(R"(
    public module A {
      public function foo() i32 {
        return 1;
      }
      public module B {
        public function bar() i32 {
          return 2;
        }
      }
    }
    using A.B;
    function main() i32 {
      return bar();
    }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Modules, nested_module_ambiguity) {
  // When parent and nested module both define foo(), using both causes
  // ambiguity
  EXPECT_THROW(executeString(R"(
    public module A {
      public function foo() i32 {
        return 1;
      }
      public module B {
        public function foo() i32 {
          return 2;
        }
      }
    }
    using A;
    using A.B;
    function main() i32 {
      return foo();
    }
  )"),
               std::exception);
}

TEST(Modules, extend_existing_module) {
  // Adding new functions to an existing module (declared earlier) works
  auto value = executeString(R"(
    public module sun {
      public function foo() i32 {
        return 1;
      }
    }
    
    public module sun {
      public function bar() i32 {
        return 2;
      }
    }

    function main() i32 {
        return sun.foo() + sun.bar();
    }
  )");
  EXPECT_EQ(value, 3);
}

// === Shadowing prevention tests ===

TEST(Modules, redeclare_function_same_signature_errors) {
  // Defining two functions with same name and parameter types is an error
  EXPECT_THROW(executeString(R"(
    function foo(x: i32) i32 {
      return x;
    }
    function foo(x: i32) i32 {
      return x * 2;
    }
    function main() i32 {
      return foo(1);
    }
  )"),
               std::exception);
}

TEST(Modules, redeclare_class_errors) {
  // Defining two classes with same name is an error
  EXPECT_THROW(executeString(R"(
    class Foo {
      var x: i32;
      function init() { this.x = 1; }
    }
    class Foo {
      var y: i32;
      function init() { this.y = 2; }
    }
    function main() i32 {
      return 0;
    }
  )"),
               std::exception);
}

TEST(Modules, redeclare_interface_errors) {
  // Defining two interfaces with same name is an error
  EXPECT_THROW(executeString(R"(
    interface IFoo {
      function bar() i32;
    }
    interface IFoo {
      function baz() i32;
    }
    function main() i32 {
      return 0;
    }
  )"),
               std::exception);
}

TEST(Modules, shadow_global_variable_errors) {
  // Shadowing a global variable from within a function is an error
  EXPECT_THROW(executeString(R"(
    var x: i32 = 10;
    function main() i32 {
      var x: i32 = 20;
      return x;
    }
  )"),
               std::exception);
}

TEST(Modules, local_shadowing_allowed) {
  // Shadowing local variables within nested scopes is allowed
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 10;
      if (true) {
        var x: i32 = 20;
        x = x + 1;
      }
      return x;
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST(Modules, using_is_lexically_scoped) {
  // A 'using' inside a function should not leak to a sibling function
  auto value = executeString(R"(
    public module math {
      public function square(x: i32) i32 { return x * x; }
    }
    function helper() i32 {
      using math;
      return square(3);
    }
    function main() i32 {
      return helper();
    }
  )");
  EXPECT_EQ(value, 9);
}

TEST(Modules, using_causes_ambiguity) {
  // A 'using' inside a function should not leak to a sibling function
  EXPECT_THROW(executeString(R"(
    public module math {
      public function foo() i32 { return 1; }
    }

    function foo() i32 {
      return 2;
    }
    function helper() i32 {
      using math;
      return foo(); // which foo, math.foo or global foo?
    }
    function main() i32 {
      return foo() + helper();
    }
  )"),
               std::exception);
}

TEST(Modules, module_imports_global_function) {
  // Module function can call a global function defined outside the module
  auto value = executeString(R"(
    function foo() i32 {
      return 123;
    }

    public module A {
      public function bar() i32 {
        return foo();
      }
    }

    function main() i32 {
      return A.bar();
    }
  )");
  EXPECT_EQ(value, 123);
}

TEST(Modules, transitive_call_to_imported_function_fails) {
  EXPECT_THROW(executeString(R"(
    public module A {
      public function bar() i32 {
        return foo();
      }
    }

    function main() i32 {
      return A.foo();
    }
  )"),
               std::exception);
}

// === Dotted module name syntax tests ===

TEST(Modules, parse_dotted_module_name) {
  // "module a.b { }" should parse as nested modules
  auto parser = Parser::createStringParser(R"(
    public module outer.inner {
      public function foo() i32 { return 42; }
    }
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::MODULE);

  // Outer module should be "outer"
  auto* outerModule = static_cast<const ModuleAST*>(ast->getBody()[0].get());
  EXPECT_EQ(outerModule->getName(), "outer");

  // Inner module should be "inner"
  const auto& outerBody = outerModule->getBody().getBody();
  ASSERT_EQ(outerBody.size(), 1);
  EXPECT_EQ(outerBody[0]->getType(), ASTNodeType::MODULE);
  auto* innerModule = static_cast<const ModuleAST*>(outerBody[0].get());
  EXPECT_EQ(innerModule->getName(), "inner");
}

TEST(Modules, dotted_module_name_execution) {
  // "module a.b { function foo() }" should be callable as a.b.foo()
  auto value = executeString(R"(
    public module outer.inner {
      public function foo() i32 { return 42; }
    }
    function main() i32 {
      return outer.inner.foo();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Modules, dotted_module_name_three_levels) {
  // "module a.b.c { }" should create three nested modules
  auto value = executeString(R"(
    public module a.b.c {
      public function foo() i32 { return 123; }
    }
    function main() i32 {
      return a.b.c.foo();
    }
  )");
  EXPECT_EQ(value, 123);
}

TEST(Modules, dotted_module_name_with_using) {
  // Using statement should work with dotted module declaration
  auto value = executeString(R"(
    public module math.advanced {
      public function cube(x: i32) i32 { return x * x * x; }
    }
    using math.advanced;
    function main() i32 {
      return cube(3);
    }
  )");
  EXPECT_EQ(value, 27);
}

TEST(Modules, dotted_module_merges_with_explicit_nesting) {
  // Dotted syntax and explicit nesting should merge into same module scope
  auto value = executeString(R"(
    public module sun.io {
      public function read() i32 { return 1; }
    }
    public module sun {
      public module io {
        public function write() i32 { return 2; }
      }
    }
    function main() i32 {
      return sun.io.read() + sun.io.write();
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST(Modules, dotted_module_with_class) {
  // Dotted module containing a class
  auto value = executeString(R"(
    public module game.entities {
      public class Player {
        public var health: i32;
        public function init(h: i32) {
          this.health = h;
        }
        public function getHealth() i32 {
          return this.health;
        }
      }
    }
    function main() i32 {
      var p = game.entities.Player(100);
      return p.getHealth();
    }
  )");
  EXPECT_EQ(value, 100);
}

// ============================================================================
// Module-qualified calls: mod.foo(args...)
// ============================================================================
// The resolved symbol name used to be rebuilt as modulePath + "_" + member,
// which dropped the overload param suffix that codegen actually emits, so
// every module-qualified call failed with "Unknown function: mod_foo".

TEST(Modules, module_qualified_call) {
  auto value = executeString(R"(
    public module m {
      public function sq(x: i32) i32 { return x * x; }
    }
    function main() i32 {
      return m.sq(5);
    }
  )");
  EXPECT_EQ(value, 25);
}

TEST(Modules, module_qualified_call_selects_overload) {
  // Resolution must use the call's argument types, not the first registered
  // overload.
  auto value = executeString(R"(
    public module m {
      public function pick(x: i32) i32 { return 1; }
      public function pick(x: f64) i32 { return 2; }
      public function pick(x: bool) i32 { return 4; }
    }
    function main() i32 {
      return m.pick(1) + m.pick(1.0) + m.pick(true);
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(Modules, nested_module_qualified_call) {
  auto value = executeString(R"(
    public module a {
      public module b {
        public function f(x: i32) i32 { return x + 1; }
      }
    }
    function main() i32 {
      return a.b.f(41);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Modules, module_qualified_call_to_extern) {
  auto value = executeString(R"(
    public module libc {
      public extern function abs(x: i32) i32;
    }
    function main() i32 {
      unsafe { return libc.abs(-13); };
    }
  )");
  EXPECT_EQ(value, 13);
}

TEST(Modules, module_qualified_call_with_void_return) {
  auto value = executeString(R"(
    public module m {
      public function noop(x: i32) void { }
    }
    function main() i32 {
      m.noop(1);
      return 7;
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST(Modules, module_qualified_call_unknown_overload_errors) {
  EXPECT_THROW(executeString(R"(
    public module m {
      public function only_i32(x: i32) i32 { return x; }
    }
    function main() i32 {
      return m.only_i32(true, 2);
    }
  )"),
               std::exception);
}

TEST(Modules, module_qualified_call_into_moon_library) {
  // Library functions carry a content-hash scope segment
  // ("$hash$_sun_println$..."), so the resolved name must come from the
  // function's own qualified name rather than being rebuilt from the path.
  auto value = executeStringWithStdlib(R"(
    function main() i32 {
      sun.println("ok");
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules, module_qualified_call_coerces_arguments) {
  // This call path used to build its argument list with a bare codegen(),
  // skipping every coercion the direct-call path applies — so a string
  // literal reached a raw_ptr<u8> parameter as a fat { ptr, i64 } struct.
  auto value = executeString(R"(
    public module m {
      public function len4(s: raw_ptr<u8>) i32 { return 4; }
    }
    function main() i32 {
      return m.len4("abcd");
    }
  )");
  EXPECT_EQ(value, 4);
}

TEST(Modules, module_qualified_call_widens_numeric_arguments) {
  auto value = executeString(R"(
    public module m {
      public function take(x: i64) i64 { return x; }
    }
    function main() i32 {
      var small: i32 = 7;
      return m.take(small);
    }
  )");
  EXPECT_EQ(value, 7);
}

// === Calling into a precompiled .moon ===

namespace {

std::filesystem::path writeMoonLib(const std::string& name,
                                   const std::string& source) {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "sun_module_moon_tests";
  fs::create_directories(dir);
  fs::path libSrc = dir / (name + ".sun");
  std::ofstream out(libSrc);
  out << source;
  out.close();
  fs::path moonPath = dir / (name + ".moon");
  sun::MoonBuilder::build(libSrc.string(), moonPath);
  return moonPath;
}

}  // namespace

// A throwing free function in a .moon must be invoked (not called) inside a
// try block, or its exception skips the local catch and terminates.
TEST(Modules, moon_free_function_throw_is_caught_by_importer) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("throwlib", R"(
    public module throwlib {
        public class Boom implements IError {
            public function init() {}
            public function code() i32 { return 77; }
            public function message() static_ptr<u8> { return "boom"; }
        }
        public function fail(x: i32) i32, IError {
            if (x > 0) { throw Boom(); }
            return 1;
        }
        public function nested(x: i32) i32, IError {
            return fail(x);
        }
    }
  )");

  auto driver = Driver::createForJIT("moon_throw_main");
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using throwlib;

    function main() i32 {
        var r: i32 = 0;
        try { r = r + fail(1); } catch (e: IError) { r = r + e.code(); }
        try { r = r + nested(1); } catch (e: IError) { r = r + 1000; }
        try { r = r + fail(0); } catch (e: IError) { r = r + 5000; }
        return r;
    }
  )");
  EXPECT_EQ(value, 1078);
}

// Struct types for the same class minted by the library and by the importer
// must agree, or by-value class arguments fail to type-check at the call.
TEST(Modules, moon_method_taking_class_by_value) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("byvallib", R"(
    public module byvallib {
        public class Pair {
            public var a: i64;
            public var b: i64;
            public function init(a: i64, b: i64) { this.a = a; this.b = b; }
        }
        public class Sink {
            var total: i64;
            public function init() { this.total = 0; }
            public function take(p: Pair) void { this.total = this.total + p.a + p.b; }
            public function total_of() i64 { return this.total; }
        }
        public function sum(p: Pair) i64 { return p.a + p.b; }
    }
  )");

  auto driver = Driver::createForJIT("moon_byval_main");
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using byvallib;

    function main() i32 {
        var s = Sink();
        s.take(Pair(1, 2));
        var p = Pair(10, 20);
        s.take(p);
        return _convert<i32>(s.total_of() + sum(Pair(3, 4)));
    }
  )");
  EXPECT_EQ(value, 40);
}
