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
#include "moon_bundling/moon_builder.h"
#include "parsing/lexer.h"
#include "parsing/parser.h"
#include "semantic_analysis/semantic_analyzer.h"

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
  auto value = executeStringWithStdlib(R"(
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
  EXPECT_THROW(driver->executeString(R"(
    using cleanlambda;

    function main() i32 {
        var base = 40;
        return apply([ref base](n: i32) => i32 { return base + n; }, 2);
    }
  )"),
               SunError);
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
  // The bundle's own code still reads it.
  EXPECT_EQ(driver->executeString(R"(
    using hidden;
    function main() i32 { return reveal(); }
  )"),
            7);

  auto driver2 = Driver::createForJIT("priv_global_main2");
  driver2->setMoonImports({sun::MoonImport(moonPath.string())});
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
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
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
    d->setMoonImports({sun::MoonImport(moonPath.string())});
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
// the same global, which is emitted under the module-mangled name.

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
