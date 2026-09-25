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

#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "driver/execution_utils.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_builder.h"
#include "parsing/lexer.h"
#include "parsing/parser.h"
#include "semantic_analysis/semantic_analyzer.h"

using sun::moon_bundling::MoonImport;

using sun::ast::ASTNodeType;
using sun::ast::ModuleAST;
using sun::ast::UsingAST;
using sun::driver::Driver;
using sun::driver::executeString;
using sun::driver::initTestEnvironment;
using sun::parsing::Parser;

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

// File imports must resolve field types and constructor expressions
// identically.
TEST(Modules, file_using_std_reaches_module_class_fields) {
  auto value = sun::driver::executeStringWithStdlib(R"(
    using std;

    /* Exercises a file import inside a module. */
    module spike {
      /* Stores a string constructed through the file import. */
      public class Path {
        var path: String;
        init(alloc: const ref HeapAllocator) {
          this.path = String(alloc, "/");
        }
        /* Returns the stored path length. */
        public method length() i64 { return this.path.length(); }
      }
    }

    /* Checks that the module class uses the imported string type. */
    function main() i32 {
      var alloc = make_heap_allocator();
      var path = spike.Path(alloc);
      return _convert<i32>(path.length());
    }
  )");
  EXPECT_EQ(value, 1);
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

TEST(Modules, module_qualified_function_can_be_stored_as_a_pointer) {
  auto value = executeString(R"(
    public module math {
      public function double(x: i32) i32 { return x * 2; }
    }

    function main() i32 {
      var callback: function (i32) i32 = math.double;
      return callback(21);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Modules, expected_pointer_type_selects_qualified_function_overload) {
  auto value = executeString(R"(
    public module math {
      public function convert(x: i32) i32 { return x * 2; }
      public function convert(x: f64) i32 { return 42; }
    }

    function main() i32 {
      var callback: function (f64) i32 = math.convert;
      return callback(14.0);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Modules, imported_function_can_be_stored_as_a_pointer) {
  auto value = executeString(R"(
    public module math {
      public function double(x: i32) i32 { return x * 2; }
    }
    using math;

    function main() i32 {
      var callback = double;
      return callback(21);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Modules, nested_module_can_contain_a_function_declaration) {
  auto value = executeString(R"(
    public module outer.inner {
      public function answer() i32 { return 42; }
    }
    function main() i32 { return outer.inner.answer(); }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Modules, parse_using_wildcard) {
  auto parser = Parser::createStringParser(R"(
    using std;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::USING);

  auto* usingNode = static_cast<const UsingAST*>(ast->getBody()[0].get());
  EXPECT_TRUE(usingNode->isModuleImport());
  EXPECT_EQ(usingNode->getNamespacePathString(), "std");
}

TEST(Modules, parse_using_specific_symbol) {
  auto parser = Parser::createStringParser(R"(
    using std.Vec;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::USING);

  auto* usingNode = static_cast<const UsingAST*>(ast->getBody()[0].get());
  EXPECT_FALSE(usingNode->isModuleImport());
  EXPECT_EQ(usingNode->getNamespacePathString(), "std");
  EXPECT_EQ(usingNode->getTarget(), "Vec");
}

TEST(Modules, parse_using_nested_module) {
  auto parser = Parser::createStringParser(R"(
    using std.matrix.types;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::USING);

  auto* usingNode = static_cast<const UsingAST*>(ast->getBody()[0].get());
  EXPECT_FALSE(usingNode->isModuleImport());
  EXPECT_EQ(usingNode->getNamespacePathString(), "std.matrix");
  EXPECT_EQ(usingNode->getTarget(), "types");
}

TEST(Modules, module_with_class_method) {
  // Test module with class and method call
  auto value = executeString(R"(
    public module mymod {
      public class ClassA {
        public var value: i32;
        init(v: i32) {
          this.value = v;
        }
        public method foo() i32 {
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
          init(v: i32) {
            this.val = v;
          }
          public method foo() i32 {
            return this.val;
          }
        }

        public class ClassA {
          public var b: ClassB;
          init() {
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
    public module std {
      public function foo() i32 {
        return 1;
      }
    }
    
    public module std {
      public function bar() i32 {
        return 2;
      }
    }

    function main() i32 {
        return std.foo() + std.bar();
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
      init() { this.x = 1; }
    }
    class Foo {
      var y: i32;
      init() { this.y = 2; }
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
      method bar() i32;
    }
    interface IFoo {
      method baz() i32;
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
    public module std.io {
      public function read() i32 { return 1; }
    }
    public module std {
      public module io {
        public function write() i32 { return 2; }
      }
    }
    function main() i32 {
      return std.io.read() + std.io.write();
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
        init(h: i32) {
          this.health = h;
        }
        public method getHealth() i32 {
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
    public module other_libc {
      public extern function abs(x: i32) i32;
    }
    function main() i32 {
      unsafe { return libc.abs(-13) + other_libc.abs(-13); };
    }
  )");
  EXPECT_EQ(value, 26);
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
  auto value = sun::driver::executeStringWithStdlib(R"(
    function main() i32 {
      std.println("ok");
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

/** Keeps test fixtures and helpers local to this source file. */
namespace {

/** Builds a compiled library containing the supplied fixture declarations. */
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
  sun::moon_bundling::MoonBuilder::build(libSrc.string(), moonPath);
  return moonPath;
}

}  // namespace

// The '<'_>' marker on a lambda-typed parameter must survive the trip
// through a .moon: had serialization dropped it, the imported signature
// would read as a clean lambda type and reject the capturing argument.
TEST(Modules, moon_ref_lambda_param_survives_round_trip) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("reflambda", R"(
    public module reflambda {
        public function apply(f: <'_>(i32) => i32, x: i32) i32 {
            return f(x);
        }
    }
  )");

  auto driver = Driver::createForJIT("moon_ref_lambda_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using reflambda;

    function main() i32 {
        var base = 40;
        return apply([ref base](n: i32) => i32 { return base + n; }, 2);
    }
  )");
  EXPECT_EQ(value, 42);
}

// A lifetime name on a bundled signature must survive the trip through a
// .moon: the importer re-validates signatures, so a dropped declaration
// would make the parameter's '<'a>' an undeclared-lifetime error.
TEST(Modules, moon_lifetime_param_survives_round_trip) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("lifetimelambda", R"(
    public module lifetimelambda {
        public function apply<'a>(f: <'a>(i32) => i32, x: i32) i32 {
            return f(x);
        }
    }
  )");

  auto driver = Driver::createForJIT("moon_lifetime_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using lifetimelambda;

    function main() i32 {
        var base = 40;
        return apply([ref base](n: i32) => i32 { return base + n; }, 2);
    }
  )");
  EXPECT_EQ(value, 42);
}

// A class's declared lifetimes survive the trip too: the imported Bus<'a>
// still lets a method bind its slot with 'ref Bus<'this>', and the checker
// still relates the scopes at call sites.
TEST(Modules, moon_class_lifetime_survives_round_trip) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("lifetimebus", R"(
    public module lifetimebus {
        public class Bus<'a> {
            var cb: <'a>(i32) => i32;
            init() { this.cb = (x: i32) => i32 { return x; }; }
            public method subscribe(cb: <'a>(i32) => i32) void { this.cb = cb; return; }
            public method publish(x: i32) i32 { var f = this.cb; return f(x); }
        }
    }
  )");

  auto driver = Driver::createForJIT("moon_class_lifetime_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using lifetimebus;

    class Node {
        var total: i32;
        init() { this.total = 0; }
        public method onMsg(x: i32) i32 { this.total = this.total + x; return this.total; }
        public method attach(bus: ref Bus<'this>) void { bus.subscribe(this.onMsg); return; }
    }
    function main() i32 {
        var n = Node();
        var bus = Bus();
        n.attach(bus);
        bus.publish(40);
        bus.publish(2);
        return n.total;
    }
  )");
  EXPECT_EQ(value, 42);
}

// The reverse holds too: a clean lambda parameter stays clean through a
// .moon and keeps rejecting capturing arguments.
TEST(Modules, moon_clean_lambda_param_still_rejects_captures) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("cleanlambda", R"(
    public module cleanlambda {
        public function apply(f: (i32) => i32, x: i32) i32 {
            return f(x);
        }
    }
  )");

  auto driver = Driver::createForJIT("moon_clean_lambda_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  EXPECT_THROW(driver->executeString(R"(
    using cleanlambda;

    function main() i32 {
        var base = 40;
        return apply([ref base](n: i32) => i32 { return base + n; }, 2);
    }
  )"),
               sun::support::SunError);
}

// A throwing free function in a .moon must be invoked (not called) inside a
// try block, or its exception skips the local catch and terminates.
TEST(Modules, moon_free_function_throw_is_caught_by_importer) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("throwlib", R"(
    public module throwlib {
        public class Boom implements IError {
            init() {}
            public method code() i32 { return 77; }
            public method message() static_ptr<u8> { return "boom"; }
        }
        public function fail(x: i32) i32 throws IError {
            if (x > 0) { throw Boom(); }
            return 1;
        }
        public function nested(x: i32) i32 throws IError {
            return fail(x);
        }
    }
  )");

  auto driver = Driver::createForJIT("moon_throw_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using throwlib;

    function main() i32 {
        var r: i32 = 0;
        try { r = r + fail(1); } catch (e: ref IError) { r = r + e.code(); }
        try { r = r + nested(1); } catch (e: ref IError) { r = r + 1000; }
        try { r = r + fail(0); } catch (e: ref IError) { r = r + 5000; }
        return r;
    }
  )");
  EXPECT_EQ(value, 1078);
}

// A module nested inside another must survive the trip through a .moon: both
// reachable by its full name through its parent, and importable on its own.
// A stale bundle makes a missing submodule look like a name-resolution bug
// ("Unknown member 'inner' in module '$hash$.outer'"), so this pins the
// behaviour the error would otherwise be blamed on.
TEST(Modules, moon_nested_module_is_reachable_by_qualified_name) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("nestedlib", R"(
    public module outer {
        public function top() i32 { return 1; }

        public module inner {
            public function nested() i32 { return 2; }
        }
    }
  )");

  auto driver = Driver::createForJIT("moon_nested_qualified_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using outer;

    function main() i32 {
        return outer.top() * 10 + outer.inner.nested();
    }
  )");
  EXPECT_EQ(value, 12);
}

// The same nested module, imported directly so its functions need no prefix.
TEST(Modules, moon_nested_module_can_be_imported_on_its_own) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("nestedlib2", R"(
    public module outer2 {
        public module inner {
            public function nested() i32 { return 2; }
        }
    }
  )");

  auto driver = Driver::createForJIT("moon_nested_using_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using outer2.inner;

    function main() i32 {
        return nested();
    }
  )");
  EXPECT_EQ(value, 2);
}

// A .moon carrying a module-level class variable and a program with one of
// its own must both run their initializers. Each module's init function is
// internal and registered in llvm.global_ctors; when the two were a single
// external "__sun_static_init", linking the bundle silently replaced the
// program's and left its global zeroed.
TEST(Modules, moon_and_program_global_initializers_both_run) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("globlib", R"(
    public module globlib {
        public class LibCounter {
            var total: i32;
            init(start: i32) { this.total = start; }
            public method bump() i32 { this.total = this.total + 1; return this.total; }
        }
        var lib_counter: LibCounter = LibCounter(100);
        public function bump_lib() i32 { return lib_counter.bump(); }
    }
  )");

  auto driver = Driver::createForJIT("moon_global_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using globlib;

    class AppCounter {
        var total: i32;
        init(start: i32) { this.total = start; }
        public method bump() i32 { this.total = this.total + 1; return this.total; }
    }
    var app_counter: AppCounter = AppCounter(1000);

    function main() i32 {
        return bump_lib() + app_counter.bump();
    }
  )");
  EXPECT_EQ(value, 1102);
}

// A manifest can name a moon by url; it is fetched into the moon cache
// (file:// keeps the test offline) and imported from there.
TEST(Modules, manifest_moon_url_is_fetched_and_imported) {
  initTestEnvironment();
  namespace fs = std::filesystem;
  auto moonPath = writeMoonLib("urllib", R"(
    public module urllib {
        public function seven() i32 { return 7; }
    }
  )");

  fs::path dir = fs::temp_directory_path() / "sun_moon_url_test";
  fs::remove_all(dir);
  fs::create_directories(dir / "cache");
  setenv("SUN_MOON_CACHE", (dir / "cache").c_str(), 1);

  fs::path mainFile = dir / "main.sun";
  {
    std::ofstream out(mainFile);
    out << "manifest { libraries: [{ url: \"file://" +
               fs::absolute(moonPath).string() +
               "\" }] }\n"
               "using urllib;\n"
               "function main() i32 { return seven(); }\n";
  }

  auto driver = Driver::createForJIT("moon_url_main");
  auto value = driver->executeFile(mainFile.string());
  unsetenv("SUN_MOON_CACHE");
  EXPECT_EQ(value, 7);

  // The bundle landed in the cache directory
  EXPECT_FALSE(fs::is_empty(dir / "cache"));
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
            init(a: i64, b: i64) { this.a = a; this.b = b; }
        }
        public class Sink {
            var total: i64;
            init() { this.total = 0; }
            public method take(p: Pair) void { this.total = this.total + p.a + p.b; }
            public method total_of() i64 { return this.total; }
        }
        public function sum(p: Pair) i64 { return p.a + p.b; }
    }
  )");

  auto driver = Driver::createForJIT("moon_byval_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
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

// === Nested modules and module-level variables across a .moon ===

// A class in a nested module whose field is a generic specialization. The
// module stubs used to be emitted in hash order, so the nested module could
// be processed before the parent that declares the generic — which made every
// import of the bundle fail, not just uses of the class.
TEST(Modules, moon_nested_module_class_with_generic_field) {
  auto moonPath = writeMoonLib("nestedgeneric", R"(
    public module outer {
      public class Box<T> {
        var v: T;
        init(v: T) { this.v = v; }
        public method get() ref T {
          return unsafe { _to_ref<T>(_address_of<T>(this.v)); };
        }
      }

      public module inner {
        public class Holder {
          var b: Box<i32>;
          init(x: i32) { this.b = Box<i32>(x); }
          public method value() i32 { return this.b.get(); }
        }
      }
    }
  )");

  auto driver = Driver::createForJIT("nested_generic_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using outer.inner;

    function main() i32 {
        var h = Holder(41);
        return h.value() + 1;
    }
  )");
  EXPECT_EQ(value, 42);
}

// Module-level variables are part of a module's interface, so the bundle has
// to carry them. Importers reference the bundle's storage rather than
// defining a second, uninitialized copy.
TEST(Modules, moon_exports_module_variables) {
  auto moonPath = writeMoonLib("globals", R"(
    public module conf {
      public var LIMIT: i32 = 42;
      public var SCALE: f64 = 1.5;

      public var INFERRED = 100;

      public module deep {
        public var DEPTH: i32 = 9;
      }
    }
  )");

  auto driver = Driver::createForJIT("globals_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using conf;
    using conf.deep;

    function main() i32 {
        if (SCALE < 1.4) { return -1; }
        // A declaration that inferred its type crosses too: extraction runs
        // before inference, so the bundle keeps the initializer for its type.
        if (INFERRED != 100) { return -2; }
        return LIMIT + DEPTH;
    }
  )");
  EXPECT_EQ(value, 51);
}

// The importer writes the bundle's storage, so a write from the importer and
// one from inside the bundle are seen by both.
TEST(Modules, moon_module_variables_are_assignable) {
  auto moonPath = writeMoonLib("mutglobals", R"(
    public module conf {
      public var LIMIT: i32 = 42;
      public function bump() void { LIMIT = LIMIT + 1; }
    }
  )");

  auto driver = Driver::createForJIT("mut_globals_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using conf;

    function main() i32 {
        LIMIT = 10;
        bump();          // the bundle's own code sees the new value
        conf.LIMIT += 1;
        return LIMIT;
    }
  )");
  EXPECT_EQ(value, 12);
}

// A private module variable is carried (code in the bundle reads it) but must
// stay unreachable from an importer.
TEST(Modules, moon_private_module_variable_is_hidden) {
  auto moonPath = writeMoonLib("privglobal", R"(
    public module hidden {
      var SECRET: i32 = 7;
      public function reveal() i32 { return SECRET; }
    }
  )");

  auto driver = Driver::createForJIT("priv_global_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  // The bundle's own code still reads it.
  EXPECT_EQ(driver->executeString(R"(
    using hidden;
    function main() i32 { return reveal(); }
  )"),
            7);

  auto driver2 = Driver::createForJIT("priv_global_main2");
  driver2->setMoonImports({MoonImport(moonPath.string())});
  EXPECT_THROW(driver2->executeString(R"(
    using hidden;
    function main() i32 { return SECRET; }
  )"),
               std::exception);
}

// `const` survives a .moon round trip: a constant global stays constant for
// the importer, a `const ref` parameter keeps its kind, and a `const method`
// may still be called on a constant receiver.
TEST(Modules, moon_keeps_const_declarations) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("constlib", R"(
    public module constlib {
        public const LIMIT: i32 = 40;
        public class Counter {
            var n: i32;
            init(n: i32) { this.n = n; }
            public const method get() i32 { return this.n; }
            public method bump() void { this.n = this.n + 1; }
        }
        public function peek(c: const ref Counter) i32 { return c.get(); }
    }
  )");

  auto driver = Driver::createForJIT("moon_const_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using constlib;

    function main() i32 {
        const c = Counter(2);
        return LIMIT + peek(c) + c.get() - 2;
    }
  )");
  EXPECT_EQ(value, 42);

  auto rejects = [&](const std::string& body, const char* message) {
    auto d = Driver::createForJIT("moon_const_reject");
    d->setMoonImports({MoonImport(moonPath.string())});
    EXPECT_SUN_ERROR_WITH_MESSAGE(
        d->executeString("using constlib;\nfunction main() i32 {\n" + body +
                         "\nreturn 0;\n}\n"),
        message);
  };
  rejects("LIMIT = 1;", "Cannot assign to constant 'LIMIT'");
  // Qualified: the message names the declaring module, not its library hash
  rejects("constlib.LIMIT = 1;", "Cannot assign to constant 'constlib.LIMIT'");
  rejects("constlib.LIMIT += 1;", "Cannot assign to constant 'constlib.LIMIT'");
  rejects("const c = Counter(1); c.bump();",
          "Cannot call non-const method 'bump' on constant 'c'");
  rejects("var c = Counter(1); var r: const ref Counter = c; r.bump();",
          "Cannot call non-const method 'bump' on const reference 'r'");
}

// === Module-level variables (issue #124) ===
//
// A module-level `var` is shared mutable state: it is written from inside its
// module, through a `using` import, and by its qualified name. All three reach
// the same global, which is emitted once under its declaration's symbol.

TEST(Modules, module_variable_assigned_within_module) {
  auto value = executeString(R"(
    module dds {
      public var counter: i64 = 0;
      public function bump() void { counter = counter + 3; }
    }
    using dds;
    function main() i32 {
      bump();
      bump();
      return counter;
    }
  )");
  EXPECT_EQ(value, 6);
}

TEST(Modules, module_variable_assigned_through_using_import) {
  auto value = executeString(R"(
    module dds { public var counter: i64 = 0; }
    using dds;
    function main() i32 {
      counter = 9;
      counter += 5;
      return counter;
    }
  )");
  EXPECT_EQ(value, 14);
}

TEST(Modules, module_variable_assigned_by_qualified_name) {
  auto value = executeString(R"(
    module dds { public var counter: i64 = 0; }
    function main() i32 {
      dds.counter = 7;
      dds.counter += 4;
      return dds.counter;
    }
  )");
  EXPECT_EQ(value, 11);
}

TEST(Modules, nested_module_variable_is_assignable) {
  auto value = executeString(R"(
    public module dds {
      public module inner { public var counter: i64 = 0; }
      public function bump() void { inner.counter += 1; }
    }
    function main() i32 {
      dds.inner.counter = 3;
      dds.bump();
      return dds.inner.counter;
    }
  )");
  EXPECT_EQ(value, 4);
}

// A compound global is owned like any other: the overwritten value is dropped
// and the new one moved in, rather than bitwise copied.
TEST(Modules, class_typed_module_variable_is_assignable) {
  auto value = executeString(R"(
    class Counter {
      var n: i32;
      init(n: i32) { this.n = n; }
      public method get() i32 { return this.n; }
    }
    module dds {
      public var c: Counter = Counter(1);
      public function reset(n: i32) void { c = Counter(n); }
    }
    function main() i32 {
      dds.reset(5);
      var a = dds.c.get();
      dds.c = Counter(9);
      return a + dds.c.get();
    }
  )");
  EXPECT_EQ(value, 14);
}

TEST(Modules, module_constant_cannot_be_assigned) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    module dds {
      public const limit: i64 = 5;
      public function bad() void { limit = 7; }
    }
    function main() i32 { dds.bad(); return 0; }
  )"),
                                "Cannot assign to constant 'limit'");

  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    module dds { public const limit: i64 = 5; }
    function main() i32 { dds.limit = 7; return 0; }
  )"),
                                "Cannot assign to constant 'dds.limit'");

  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    module dds { public const limit: i64 = 5; }
    function main() i32 { dds.limit += 7; return 0; }
  )"),
                                "Cannot assign to constant 'dds.limit'");
}

TEST(Modules, qualified_module_variable_assignment_is_type_checked) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    module dds { public var counter: i64 = 0; }
    function main() i32 { dds.counter = true; return 0; }
  )"),
                                "Cannot assign value of type 'bool'");

  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    module dds { public var counter: i64 = 0; }
    function main() i32 { dds.nope = 1; return 0; }
  )"),
                                "Unknown member 'nope' in module 'dds'");

  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    module dds { public function f() i32 { return 1; } }
    function main() i32 { dds.f = 3; return 0; }
  )"),
                                "it is not a variable");
}

TEST(Modules, private_module_variable_cannot_be_assigned_from_outside) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    module dds { var counter: i64 = 0; }
    function main() i32 { dds.counter = 1; return 0; }
  )"),
                                "is private to module 'dds'");
}

TEST(Modules, moon_sibling_signatures_are_source_order_independent) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("sibling_signature", R"(
    public module api {
      public class Reader {
        init() {}
        public method read(token: const ref ztypes.Token) i32 {
          return token.get();
        }
      }
    }

    public module ztypes {
      public class Token {
        var value: i32;
        init(value: i32) { this.value = value; }
        public const method get() i32 { return this.value; }
      }
    }
  )");

  auto driver = Driver::createForJIT("moon_sibling_signature_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using api;
    using ztypes;

    function main() i32 {
      var token = Token(42);
      var reader = Reader();
      return reader.read(token);
    }
  )");
  EXPECT_EQ(value, 42);
}

// === Names must agree on both sides of a .moon boundary ===

// A method taking an interface-typed parameter. The bundle and its importer
// must derive the same symbol for it, or the importer asks the linker for a
// symbol the bundle never defined (issue #216).
TEST(Modules, moon_interface_param_links) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("ifaceparam", R"(
    public module handlers {
        public interface IHandler { method handle(x: i32) i32; }

        /** Invokes a handler supplied by the importer. */
        public class Runner {
            /** Creates a runner. */
            init() {}
            /** Dispatches through a borrowed handler across the library boundary. */
            public method run(h: ref IHandler, x: i32) i32 { return h.handle(x); }
        }
    }
  )");

  auto driver = Driver::createForJIT("moon_iface_param_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using handlers;

    class Echo implements IHandler {
        var pad: i32;
        init() { this.pad = 0; }
        public method handle(x: i32) i32 { return x + 1; }
    }

    function main() i32 {
        var echo = Echo();
        var h: ref IHandler = echo;
        var r = Runner();
        return r.run(h, 41);
    }
  )");
  EXPECT_EQ(value, 42);
}

// A generic specialized over one of the bundle's own types, passed by value:
// the importer pre-declares the bundle's functions, so both sides must agree
// on the specialization's struct layout and symbol (issue #217). The importer
// then also reuses the bundle's precompiled specialization instead of
// instantiating its own.
TEST(Modules, moon_generic_over_own_type_by_value_links) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("boxes", R"(
    public module boxes {
        public interface IHandler { method handle(x: i32) i32; }

        /** Supplies a concrete handler owned by the generic box. */
        public class Handler implements IHandler {
            /** Creates the handler. */
            init() {}
            /** Increments the input. */
            public method handle(x: i32) i32 { return x + 1; }
        }
        public class Box<T> {
            var value: T;
            init(value: T) { this.value = value; }
            public method get() ref T {
                return unsafe { _to_ref<T>(_address_of<T>(this.value)); };
            }
        }

        public class Server {
            var box: Box<Handler>;
            init(box: Box<Handler>) { this.box = box; }
            public method run(x: i32) i32 { return this.box.get().handle(x); }
        }
        public function make_server(box: Box<Handler>) Server {
            return Server(box);
        }

        public class Config {
            public var n: i32;
            init(n: i32) { this.n = n; }
        }
        public function config_value(box: Box<Config>) i32 {
            return box.get().n;
        }
    }
  )");

  auto driver = Driver::createForJIT("moon_generic_by_value_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using boxes;

    function main() i32 {
        var b = Box<Handler>(Handler());
        var s = make_server(b);
        var c = Box<Config>(Config(10));
        return s.run(31) + config_value(c);
    }
  )");
  EXPECT_EQ(value, 42);
}

// The compiler spells a bundle's own symbols with the bundle's hash while it
// compiles them; nothing renames symbols afterwards. So every function the
// bundle defines must carry the hash its metadata records — a symbol without
// it is one the importer could never find.
TEST(Modules, moon_symbols_use_versioned_portable_identities) {
  initTestEnvironment();
  auto moonPath = writeMoonLib("hashed", R"(
    public module hashed {
        public interface IShape { method area() i32; }
        public class Square implements IShape {
            var side: i32;
            init(side: i32) { this.side = side; }
            public method area() i32 { return this.side * this.side; }
        }
        public class Pair<T> {
            var a: T;
            var b: T;
            init(a: T, b: T) { this.a = a; this.b = b; }
        }
        public var count: i32 = 0;
        public function make_pair(side: i32) Pair<Square> {
            var f = (x: i32) => i32 { return x + 1; };
            return Pair<Square>(Square(side), Square(f(side)));
        }
    }
  )");

  auto reader = sun::moon_bundling::MoonReader::open(moonPath);
  ASSERT_NE(reader, nullptr);
  auto modules = reader->listModules();
  ASSERT_FALSE(modules.empty());
  const auto* metadata = reader->getMetadata(modules[0]);
  ASSERT_NE(metadata, nullptr);
  const std::string prefix = "_SUN1_";

  llvm::LLVMContext context;
  auto bundled = reader->loadModule(modules[0], context);
  ASSERT_NE(bundled, nullptr);
  for (const auto& func : bundled->functions()) {
    if (func.isDeclaration() || func.isIntrinsic()) continue;
    // Module-private functions (lambdas) cannot be named from outside, so
    // their names need no prefix
    if (func.hasLocalLinkage()) continue;
    std::string name = func.getName().str();
    if (func.hasFnAttribute("sun.cabi")) continue;
    EXPECT_EQ(name.rfind(prefix, 0), 0u) << "unprefixed symbol: " << name;
  }
  for (const auto& global : bundled->globals()) {
    if (!global.hasInitializer()) continue;
    std::string name = global.getName().str();
    if (global.hasLocalLinkage() || global.getName().starts_with("llvm."))
      continue;
    if (global.getMetadata("sun.cabi")) continue;
    EXPECT_EQ(name.rfind(prefix, 0), 0u) << "unprefixed global: " << name;
  }
}

TEST(Modules, malformed_dotted_module_names) {
  for (const auto* source : {"module a. {}", "module a..b {}",
                             "public module .a {}", "module a.b;"}) {
    SCOPED_TRACE(source);
    auto parser = Parser::createStringParser(source);
    EXPECT_THROW(parser.parseProgram(), std::exception);
  }
}

// === Global constants built from other constants (issue #311) ===

TEST(Modules, global_const_initialized_from_another_const) {
  EXPECT_EQ(executeString(R"(
    const A: i64 = 4;
    const B: i64 = A;
    const C: i64 = A * 2 + 1;
    const NARROW: i32 = 3;
    const WIDE: i64 = NARROW;
    function main() i32 {
      if (B != 4i64) { return 1; }
      if (C != 9i64) { return 2; }
      if (WIDE != 3i64) { return 3; }
      return 0;
    }
  )"),
            0);
}

TEST(Modules, module_const_initialized_from_another_const) {
  EXPECT_EQ(executeString(R"(
    module limits {
      public const BASE: i64 = 10;
      public const DOUBLE: i64 = BASE * 2;
    }
    const TRIPLE: i64 = limits.BASE * 3;
    function main() i32 {
      if (limits.DOUBLE != 20i64) { return 1; }
      if (TRIPLE != 30i64) { return 2; }
      return 0;
    }
  )"),
            0);
}

// A `const` is never reassigned, but its first value need not be known at
// compile time: one that reads a `var` is given its value at startup.
TEST(Modules,
     global_const_initialized_from_mutable_global_initializes_at_startup) {
  EXPECT_EQ(executeString(R"(
    var counter: i64 = 4;
    const SNAPSHOT: i64 = counter;
    function main() i32 { return _convert<i32>(SNAPSHOT); }
  )"),
            4);
  EXPECT_EQ(executeString(R"(
    module state { public var counter: i64 = 4; }
    const SNAPSHOT: i64 = state.counter;
    function main() i32 { return _convert<i32>(SNAPSHOT); }
  )"),
            4);
}

// A value known at compile time is written into the program image, so it
// costs nothing at startup: only the variables that need run-time work appear
// in the startup function.
TEST(Modules, global_known_at_compile_time_needs_no_startup_code) {
  sun::driver::initTestEnvironment();
  {
    auto driver = Driver::createForAOT("image_globals_only");
    driver->compileString(R"(
      const A: i64 = 4;
      const B: i64 = A * 2 + 1;
      var counter: i32 = 7;
      const PRIMES: array<i32, 3> = [2, 3, 5];
      const NAME = "sun";
      function main() i32 { return _convert<i32>(B) + counter + PRIMES[0]; }
    )");
    EXPECT_EQ(driver->getModule().getFunction("__sun_static_init"), nullptr);
    EXPECT_EQ(driver->getModule().getGlobalVariable("llvm.global_ctors"),
              nullptr);
  }
  {
    auto driver = Driver::createForAOT("image_and_startup_globals");
    driver->compileString(R"(
      const A: i64 = 4;
      var counter: i64 = A + 1;
      const SNAPSHOT: i64 = counter;
      function main() i32 { return _convert<i32>(SNAPSHOT); }
    )");
    llvm::Function* startup =
        driver->getModule().getFunction("__sun_static_init");
    ASSERT_NE(startup, nullptr);
    // SNAPSHOT is the only variable the startup function writes
    int stores = 0;
    for (const auto& block : *startup)
      for (const auto& instruction : block)
        if (llvm::isa<llvm::StoreInst>(instruction)) ++stores;
    EXPECT_EQ(stores, 1);
  }
}

// Every kind of value initialized at startup holds the right value by the
// time `main` runs, including arrays and numbers that need widening.
TEST(Modules, globals_initialized_at_startup_hold_their_values) {
  EXPECT_EQ(executeString(R"(
    var seed: i32 = 3;
    const WIDE: i64 = seed;
    const HALF: f64 = _convert<f64>(seed) / 2.0;
    const SQUARES: array<i32, 3> = [seed, seed * seed, seed * seed * seed];
    const IS_ODD: bool = seed % 2 == 1;
    function main() i32 {
      if (not IS_ODD) { return -1; }
      if (HALF != 1.5) { return -2; }
      return _convert<i32>(WIDE) + SQUARES[0] + SQUARES[1] + SQUARES[2];
    }
  )"),
            3 + 3 + 9 + 27);
}

/** The message a program fails to compile with, or "" when it compiles. */
static std::string globalCompileError(const std::string& source) {
  try {
    executeString(source);
  } catch (const std::exception& error) {
    return error.what();
  }
  return "";
}

// Globals, like functions and classes, may be used before the line that
// declares them: by a function, by a module, and by another global.
TEST(Modules, global_can_be_used_before_its_declaration) {
  EXPECT_EQ(executeString(R"(
    function main() i32 { return _convert<i32>(total()) + origin.x + add_one(1); }
    function total() i64 { counter = counter + DOUBLE; return counter + m.get(); }
    module m {
      public function get() i64 { return LIMIT; }
      const LIMIT: i64 = BASE + 1;
    }
    const DOUBLE: i64 = BASE * 2;
    const BASE: i64 = 10;
    var counter: i64 = 1;
    class Point { var x: i32; init(x: i32) { this.x = x; } }
    var origin = Point(7);
    var add_one = (x: i32) => i32 { return x + 1; };
  )"),
            1 + 20 + 11 + 7 + 2);
}

// A constant computed from a later constant is still a compile-time value.
TEST(Modules, global_read_before_its_declaration_is_still_compile_time) {
  sun::driver::initTestEnvironment();
  auto driver = Driver::createForAOT("later_global_image");
  driver->compileString(R"(
    const DOUBLE: i64 = BASE * 2;
    const BASE: i64 = 10;
    function main() i32 { return _convert<i32>(DOUBLE); }
  )");
  EXPECT_EQ(driver->getModule().getFunction("__sun_static_init"), nullptr);
}

// A constant computed by calling a plain function is a compile-time value
// like any other: it can size an array, and the program needs no startup code.
TEST(Modules, global_computed_by_a_pure_function_is_compile_time) {
  const std::string program = R"(
    function fact(n: i64) i64 {
      if (n <= 1) { return 1; }
      return n * fact(n - 1);
    }
    function half(x: i64) i64 { return x / 2; }
    const SIZE: i64 = half(8);
    const BIG: i64 = fact(5) + LATE;
    const LATE: i64 = twice(1);
    var grid: array<i32, SIZE> = [1, 2, 3, 4];
    function twice(x: i64) i64 { return x * 2; }
    function main() i32 { return _convert<i32>(BIG) + grid[3]; }
  )";
  EXPECT_EQ(executeString(program), 120 + 2 + 4);

  sun::driver::initTestEnvironment();
  auto driver = Driver::createForAOT("pure_function_globals");
  driver->compileString(program);
  EXPECT_EQ(driver->getModule().getFunction("__sun_static_init"), nullptr);
}

TEST(Modules, global_that_depends_on_itself_is_rejected) {
  std::string message = globalCompileError(R"(
    const A: i64 = B + 1;
    const B: i64 = C;
    const C: i64 = A;
    function main() i32 { return 0; }
  )");
  EXPECT_NE(message.find("'A' depends on itself: A -> B -> C -> A"),
            std::string::npos)
      << message;
}

// Startup initializers run in source order, so reading one that has not run
// yet would see zero. A compile-time value has no such order.
TEST(Modules, startup_global_reading_a_later_startup_global_is_rejected) {
  std::string message = globalCompileError(R"(
    var seed: i64 = 3;
    const FIRST: i64 = SECOND + 1;
    const SECOND: i64 = seed * 2;
    function main() i32 { return 0; }
  )");
  EXPECT_NE(message.find("'FIRST' is initialized before 'SECOND'"),
            std::string::npos)
      << message;

  EXPECT_EQ(executeString(R"(
    var seed: i64 = 3;
    const FIRST: i64 = seed + SECOND;
    const SECOND: i64 = 4;
    function main() i32 { return _convert<i32>(FIRST); }
  )"),
            7);
}

// Emitting a function, class, lambda or module leaves the code generator
// inside the last function it wrote. A global declared afterwards is still at
// file scope and must be treated that way.
TEST(Modules, global_const_reads_const_across_other_declarations) {
  EXPECT_EQ(executeString(R"(
    const A: i64 = 4;
    function helper() i32 { return 0; }
    const AFTER_FUNCTION: i64 = A;

    class Point {
      var x: i64;
      init(x: i64) { this.x = x; }
    }
    const AFTER_CLASS: i64 = A + 1;

    var add_one = (x: i64) => i64 { return x + 1; };
    const AFTER_LAMBDA: i64 = A + 2;

    module inner {
      public function helper() i32 { return 0; }
      public const IN_MODULE: i64 = 10;
      public const AFTER_MODULE_FUNCTION: i64 = IN_MODULE + 1;
    }
    const AFTER_MODULE: i64 = inner.AFTER_MODULE_FUNCTION + A;

    function main() i32 {
      if (AFTER_FUNCTION != 4i64) { return 1; }
      if (AFTER_CLASS != 5i64) { return 2; }
      if (AFTER_LAMBDA != 6i64) { return 3; }
      if (AFTER_MODULE != 15i64) { return 4; }
      return 0;
    }
  )"),
            0);
}

// A string global needs no open function, wherever it is declared.
TEST(Modules, global_string_before_and_after_a_function) {
  EXPECT_EQ(executeString(R"(
    const FIRST = "sun";
    function helper() i32 { return 0; }
    const SECOND = "moon";
    function main() i32 {
      if (FIRST.length() != 3i64) { return 1; }
      if (SECOND.length() != 4i64) { return 2; }
      return 0;
    }
  )"),
            0);
}

// A bundle exports through its modules, so a global outside any module has no
// importer that could reach it. Building such a bundle is rejected; importing
// one used to crash the compiler.
TEST(Modules, moon_rejects_global_outside_any_module) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      writeMoonLib("bare_global", R"(
        var BARE_LIMIT: i64 = 7;

        public module bare {
          public function answer() i32 { return 42; }
        }
      )"),
      "global 'BARE_LIMIT' is declared outside any module");
  // A C extern global declares a name like any other, so the rule covers it.
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      writeMoonLib("bare_extern_global", R"(
        extern "C" var c_environ: raw_ptr<raw_ptr<u8>> as "environ";

        public module bare_extern {
          public function answer() i32 { return 42; }
        }
      )"),
      "global 'c_environ' is declared outside any module");
}

// A library's types keep their array sizes across a bundle, including a size
// named by a constant the importer cannot see.
TEST(Modules, moon_keeps_named_array_sizes) {
  auto moonPath = writeMoonLib("named_sizes", R"(
    public module named_sizes {
      public const N: i64 = 3;
      const HIDDEN: i64 = 2;
      public class Grid {
        public var cells: array<i32, N>;
        var pad: array<i32, HIDDEN>;
        init() { this.cells = [1, 2, 3]; this.pad = [0, 0]; }
      }
      public function ends(g: ref Grid) i32 { return g.cells[0] + g.cells[2]; }
    }
  )");

  auto driver = Driver::createForJIT("named_sizes_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using named_sizes;
    function main() i32 {
      var g = named_sizes.Grid();
      var copy: array<i32, 3> = [g.cells[0], g.cells[1], g.cells[2]];
      return named_sizes.ends(g) + copy[1];
    }
  )");
  EXPECT_EQ(value, 6);
}

/**
 * A library with constants of each kind, built once: the library cache keeps
 * a bundle open by path, so a test must not rebuild one it has loaded.
 */
static std::filesystem::path constValuesLib() {
  static const std::filesystem::path path = writeMoonLib("const_values", R"(
    public module const_values {
      public const LIMIT: i64 = 7;
      public const N: i64 = 3;
      const HIDDEN: i64 = 2;
      public const PRIMES: array<i32, 3> = [2, 3, 5];
      public var seed: i64 = 4;
      public const SNAP: i64 = seed;
      public class Ring<T> {
        var slots: array<T, HIDDEN>;
        init(a: T, b: T) { this.slots = [a, b]; }
        public method last() T { return this.slots[1]; }
      }
    }
  )");
  return path;
}

// A bundle publishes the value of each `const` it computed at compile time,
// so an importer can compute with it: in its own constants, as an array size,
// and inside a generic class the library ships. The storage stays the
// library's.
TEST(Modules, moon_publishes_compile_time_constant_values) {
  auto driver = Driver::createForJIT("const_values_main");
  driver->setMoonImports({MoonImport(constValuesLib().string())});
  auto value = driver->executeString(R"(
    using const_values;
    const B: i64 = const_values.LIMIT + 1;
    var grid: array<i32, const_values.N> = [1, 2, 3];
    function main() i32 {
      var r = const_values.Ring<i32>(5, 6);
      return _convert<i32>(B) + grid[2] + r.last() + const_values.PRIMES[2];
    }
  )");
  EXPECT_EQ(value, 8 + 3 + 6 + 5);
}

// A constant computed from a library's constant is itself a compile-time
// value: neither the library nor the program needs any startup code.
TEST(Modules, moon_constant_value_keeps_the_importer_compile_time) {
  auto moonPath = writeMoonLib("const_only", R"(
    public module const_only { public const LIMIT: i64 = 7; }
  )");
  auto driver = Driver::createForAOT("const_only_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  driver->compileString(R"(
    using const_only;
    const B: i64 = const_only.LIMIT + 1;
    function main() i32 { return _convert<i32>(B); }
  )");
  EXPECT_EQ(driver->getModule().getFunction("__sun_static_init"), nullptr);
}

TEST(Modules, moon_constant_without_a_compile_time_value_cannot_be_a_size) {
  auto driver = Driver::createForJIT("const_values_reject");
  driver->setMoonImports({MoonImport(constValuesLib().string())});
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      driver->executeString(R"(
    using const_values;
    var bad: array<i32, const_values.SNAP> = [1];
    function main() i32 { return 0; }
  )"),
      "'const_values.SNAP' is initialized at startup because it is defined in "
      "a precompiled library that gives it no compile-time value");

  auto hidden = Driver::createForJIT("const_values_hidden");
  hidden->setMoonImports({MoonImport(constValuesLib().string())});
  EXPECT_SUN_ERROR_WITH_MESSAGE(hidden->executeString(R"(
    using const_values;
    var bad: array<i32, const_values.HIDDEN> = [1, 2];
    function main() i32 { return 0; }
  )"),
                                "'HIDDEN' is private to module 'const_values'");
}

// === Startup order across bundles ===
//
// Globals that need code to initialize them are set up by a startup function,
// one per bundle plus one for the program. A library's must run before the
// startup function of anything that imports it, and exactly once however many
// import paths lead to it.

TEST(Modules, moon_globals_are_initialized_before_the_importing_program) {
  auto moonPath = writeMoonLib("startup_leaf", R"(
    public module startup_leaf {
      public class Counter {
        public var value: i64;
        init(start: i64) { this.value = start; }
        public method get() i64 { return this.value; }
      }
      public var counter: Counter = Counter(100);
      public function readCounter() i64 { return counter.get(); }
    }
  )");

  auto driver = Driver::createForJIT("startup_leaf_main");
  driver->setMoonImports({MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using startup_leaf;

    class Snapshot {
      var seen: i64;
      init(seen: i64) { this.seen = seen; }
      method get() i64 { return this.seen; }
    }
    // Reads the library's global while the program is still starting up.
    var snapshot: Snapshot = Snapshot(readCounter());

    function main() i32 {
      if (snapshot.get() != 100i64) { return 1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Modules, moon_globals_are_initialized_in_import_order_across_a_chain) {
  writeMoonLib("chain_leaf", R"(
    public module chain_leaf {
      public class Box {
        public var value: i64;
        init(value: i64) { this.value = value; }
        public method get() i64 { return this.value; }
      }
      public var leaf_box: Box = Box(100);
      public function readLeaf() i64 { return leaf_box.get(); }
    }
  )");
  auto midPath = writeMoonLib("chain_mid", R"(
    public module chain_mid {
      public class MidBox {
        public var value: i64;
        init(value: i64) { this.value = value; }
        public method get() i64 { return this.value; }
      }
      // Initialized from the leaf library's global.
      public var mid_box: MidBox = MidBox(chain_leaf.readLeaf() + 10);
      public function readMid() i64 { return mid_box.get(); }
    }

    manifest {
      libraries: [{ path: "chain_leaf.moon" }]
    }
  )");

  auto driver = Driver::createForJIT("chain_main");
  driver->setMoonImports({MoonImport(midPath.string())});
  auto value = driver->executeString(R"(
    using chain_mid;

    class Snapshot {
      var seen: i64;
      init(seen: i64) { this.seen = seen; }
      method get() i64 { return this.seen; }
    }
    var snapshot: Snapshot = Snapshot(readMid() + 1);

    function main() i32 {
      if (snapshot.get() != 111i64) { return 1; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// A bundle embeds the code of the libraries it imports, so importing a
// library both directly and through another one brings in two copies of its
// startup function. Its globals exist once and must be constructed once.
TEST(Modules, moon_globals_reached_by_two_import_paths_are_initialized_once) {
  auto leafPath = writeMoonLib("once_leaf", R"(
    public module once_leaf {
      public var constructions: i64 = 0;
      public class Tracker {
        public var id: i64;
        init(id: i64) {
          this.id = id;
          constructions = constructions + 1;
        }
      }
      public var tracker: Tracker = Tracker(1);
      public function countConstructions() i64 { return constructions; }
    }
  )");
  auto midPath = writeMoonLib("once_mid", R"(
    public module once_mid {
      public function countSeenByMid() i64 {
        return once_leaf.countConstructions();
      }
    }

    manifest {
      libraries: [{ path: "once_leaf.moon" }]
    }
  )");

  auto driver = Driver::createForJIT("once_main");
  driver->setMoonImports(
      {MoonImport(leafPath.string()), MoonImport(midPath.string())});
  auto value = driver->executeString(R"(
    using once_leaf;
    using once_mid;

    function main() i32 {
      if (countConstructions() != 1i64) { return 1; }
      if (countSeenByMid() != 1i64) { return 2; }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

/** Rejects local shadowing regardless of declaration order or an earlier read.
 */
TEST(Modules, global_shadowing_is_independent_of_order) {
  for (bool module : {false, true}) {
    for (bool globalFirst : {false, true}) {
      for (bool readFirst : {false, true}) {
        SCOPED_TRACE(module);
        SCOPED_TRACE(globalFirst);
        SCOPED_TRACE(readFirst);
        const std::string global = "var total: i32 = 1;\n";
        const std::string local =
            "/** Attempts to shadow the enclosing global. */\n"
            "function f() i32 {\n" +
            std::string(readFirst ? "var before: i32 = total;\n" : "") +
            "var total: i32 = 5;\nreturn total;\n}\n";
        std::string source = globalFirst ? global + local : local + global;
        const int localLine =
            3 + (globalFirst ? 1 : 0) + (readFirst ? 1 : 0) + (module ? 1 : 0);
        if (module)
          source = "/** Contains the conflicting names. */ module m {\n" +
                   source + "}\n";
        source +=
            "/** Supplies the program entry point. */\n"
            "function main() i32 { return 0; }\n";
        try {
          executeString(source);
          FAIL() << "Accepted a local that shadows a global";
        } catch (const sun::support::SunError& error) {
          EXPECT_NE(error.getMessage().find(std::string("Cannot shadow ") +
                                            (module ? "module" : "global") +
                                            " variable 'total'"),
                    std::string::npos);
          ASSERT_TRUE(error.getLocation().has_value());
          EXPECT_EQ(error.getLocation()->line, localLine);
        }
      }
    }
  }
}

/** Keeps library lexical lookup inside its bundle while retaining file imports.
 */
TEST(Modules, library_scope_stops_lookup_at_bundle_boundary) {
  using namespace sun::semantic_analysis;
  GlobalScope program;
  program.declareVariable("consumer", nullptr);
  auto& library = program.declareModule("$library$");
  library.declareVariable("libraryValue", nullptr);
  auto& module = library.declareModule("api");
  BlockScope body;
  body.parent = &module;
  EXPECT_EQ(body.lookupVariable("consumer"), nullptr);
  EXPECT_NE(body.lookupVariable("libraryValue"), nullptr);
  DeclarationTable declarations;
  sun::ast::VariableCreationAST later("later", nullptr);
  const auto laterId = declarations.add(DeclarationKind::Variable, "later");
  declarations.bindAstNode(laterId, &later);
  declarations.registerGlobal(QualifiedName({}, "later"), laterId);
  EXPECT_EQ(program.findUnanalyzedGlobal("later", declarations).node, &later);
  EXPECT_EQ(body.findUnanalyzedGlobal("later", declarations).node, nullptr);
  EXPECT_NO_THROW(body.declareVariable("consumer", nullptr));
  EXPECT_THROW(body.declareVariable("libraryValue", nullptr),
               sun::support::SunError);

  ImportScope importedFile;
  importedFile.scopeName = "$import_file$";
  importedFile.parent = &program;
  EXPECT_NE(importedFile.lookupVariable("consumer"), nullptr);
}

/** Parameters and references obey the same rule as ordinary local variables. */
TEST(Modules, other_bindings_cannot_shadow_later_globals) {
  EXPECT_NE(globalCompileError(R"(
    /** Declares a parameter that conflicts with a later global. */
    function f(total: i32) i32 { return total; }
    var total: i32 = 1;
    /** Supplies the program entry point. */
    function main() i32 { return 0; }
  )")
                .find("Cannot shadow global variable 'total'"),
            std::string::npos);
  EXPECT_NE(globalCompileError(R"(
    /** Declares a reference that conflicts with a later global. */
    function main() i32 {
      var value: i32 = 2;
      ref total = value;
      return total;
    }
    var total: i32 = 1;
  )")
                .find("Cannot shadow global variable 'total'"),
            std::string::npos);
}
