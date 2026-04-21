// tests/test_modules.cpp
// Tests for the module/import system

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "codegen.h"
#include "codegen_visitor.h"
#include "execution_utils.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"

// === Compilation tests (AOT) ===

TEST(ModuleTest, import_single_function) {
  EXPECT_NO_THROW(compileFile("tests/programs/import_math.sun"));
}

TEST(ModuleTest, import_multiple_functions) {
  EXPECT_NO_THROW(compileFile("tests/programs/import_multi_func.sun"));
}

TEST(ModuleTest, import_two_modules) {
  EXPECT_NO_THROW(compileFile("tests/programs/import_two_modules.sun"));
}

TEST(ModuleTest, import_transitive) {
  EXPECT_NO_THROW(compileFile("tests/programs/import_transitive.sun"));
}

// === Parser tests ===

TEST(ModuleTest, parse_import_statement) {
  auto parser = Parser::createStringParser(R"(
    import "math_module.sun";
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::IMPORT);

  auto* importNode = static_cast<const ImportAST*>(ast->getBody()[0].get());
  EXPECT_EQ(importNode->getPath(), "math_module.sun");
}

TEST(ModuleTest, parse_multiple_imports) {
  auto parser = Parser::createStringParser(R"(
    import "a.sun";
    import "b.sun";
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 2);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::IMPORT);
  EXPECT_EQ(ast->getBody()[1]->getType(), ASTNodeType::IMPORT);
}

// === Module declaration tests ===

TEST(ModuleTest, parse_module_declaration) {
  auto parser = Parser::createStringParser(R"(
    module math {
      function square(x: i32) i32 {
        return x * x;
      }
    }
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::NAMESPACE);

  auto* moduleNode = static_cast<const NamespaceAST*>(ast->getBody()[0].get());
  EXPECT_EQ(moduleNode->getName(), "math");
}

TEST(ModuleTest, module_with_using_all) {
  // Test: module math { function square(x: i32) i32 { return x * x; } }
  //       using math;
  //       function main() i32 { return square(5); }
  auto value = executeString(R"(
    module math {
      function square(x: i32) i32 {
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

TEST(ModuleTest, module_with_using_specific) {
  // Test: using math.square; to import only one symbol
  auto value = executeString(R"(
    module math {
      function square(x: i32) i32 {
        return x * x;
      }
      function cube(x: i32) i32 {
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

TEST(ModuleTest, parse_using_wildcard) {
  auto parser = Parser::createStringParser(R"(
    using sun;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::USING);

  auto* usingNode = static_cast<const UsingAST*>(ast->getBody()[0].get());
  EXPECT_TRUE(usingNode->isWildcardImport());
  EXPECT_EQ(usingNode->getNamespacePathString(), "sun");
}

TEST(ModuleTest, parse_using_specific_symbol) {
  auto parser = Parser::createStringParser(R"(
    using sun.Vec;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::USING);

  auto* usingNode = static_cast<const UsingAST*>(ast->getBody()[0].get());
  EXPECT_FALSE(usingNode->isWildcardImport());
  EXPECT_EQ(usingNode->getNamespacePathString(), "sun");
  EXPECT_EQ(usingNode->getTarget(), "Vec");
}

TEST(ModuleTest, parse_using_prefix_wildcard) {
  auto parser = Parser::createStringParser(R"(
    using sun.Mat*;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::USING);

  auto* usingNode = static_cast<const UsingAST*>(ast->getBody()[0].get());
  EXPECT_FALSE(usingNode->isWildcardImport());
  EXPECT_TRUE(usingNode->isPrefixWildcardImport());
  EXPECT_EQ(usingNode->getPrefix(), "Mat");
}

TEST(ModuleTest, parse_using_nested_module) {
  auto parser = Parser::createStringParser(R"(
    using sun.matrix.types;
  )");
  auto ast = parser.parseProgram();
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(ast->getBody().size(), 1);
  EXPECT_EQ(ast->getBody()[0]->getType(), ASTNodeType::USING);

  auto* usingNode = static_cast<const UsingAST*>(ast->getBody()[0].get());
  EXPECT_FALSE(usingNode->isWildcardImport());
  EXPECT_EQ(usingNode->getNamespacePathString(), "sun.matrix");
  EXPECT_EQ(usingNode->getTarget(), "types");
}

TEST(ModuleTest, module_with_class_method) {
  // Test module with class and method call
  auto value = executeString(R"(
    module mymod {
      class ClassA {
        var value: i32;
        function init(v: i32) {
          this.value = v;
        }
        function foo() i32 {
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

TEST(ModuleTest, nested_modules_nested_classes_method_chain) {
  // Test: mod_x.mod_y.ClassA.ClassB.foo()
  // Nested modules with nested classes and method call chain
  auto value = executeString(R"(
    module mod_x {
      module mod_y {
    
        class ClassB {
          var val: i32;
          function init(v: i32) {
            this.val = v;
          }
          function foo() i32 {
            return this.val;
          }
        }

        class ClassA {
          var b: ClassB;
          function init() {
            this.b = ClassB(42);
          }
        }

        var a = ClassA();
      }
    }

    function main() i32 {
      return mod_x.mod_y.a.b.foo();
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(ModuleTest, same_module_in_multiple_files_merges) {
  // Two files declare the same module with different functions - they merge
  auto value = executeString(R"(
    import "tests/programs/same_mod_a.sun";
    import "tests/programs/same_mod_b.sun";

    function main() i32 {
      return mymod.foo() + mymod.bar();
    }
  )");
  EXPECT_EQ(value, 3);  // foo() returns 1, bar() returns 2
}

TEST(ModuleTest, nested_module_qualified_function_call) {
  // Call functions in nested modules using qualified names without using
  auto value = executeString(R"(
    import "tests/programs/reversed_mods_AB.sun";
    import "tests/programs/reversed_mods_BA.sun";

    function main() i32 {
      return A.B.foo() + B.A.foo();
    }
  )");
  EXPECT_EQ(value, 3);  // A.B.foo() returns 2, B.A.foo() returns 1
}

TEST(ModuleTest, submod_fn_calls_mod_fn) {
  // Call functions in nested modules using qualified names without using
  auto value = executeString(R"(
    import "tests/programs/submod_fn_calls_mod_fn.sun";

    function main() i32 {
      return A.foo() + A.B.bar();
    }
  )");
  EXPECT_EQ(value, 2);  // A.B.foo() returns 2, B.A.foo() returns 1
}

TEST(ModuleTest, submod_duplicates_fn) {
  // When two modules define the same symbol and both are wildcard imported,
  // using the unqualified name is ambiguous and errors
  EXPECT_THROW(executeString(R"(
    module A {
      function foo() i32 {
        return 1;
      }
    }
    module B {
      function foo() i32 {
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

TEST(ModuleTest, using_nested_module_as_wildcard) {
  // "using A.B;" where B is a nested module should import all from A.B
  auto value = executeString(R"(
    module A {
      function foo() i32 {
        return 1;
      }
      module B {
        function bar() i32 {
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

TEST(ModuleTest, nested_module_ambiguity) {
  // When parent and nested module both define foo(), using both causes
  // ambiguity
  EXPECT_THROW(executeString(R"(
    module A {
      function foo() i32 {
        return 1;
      }
      module B {
        function foo() i32 {
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

TEST(ModuleTest, extend_existing_module) {
  // Adding new functions to an existing module (declared earlier) works
  auto value = executeString(R"(
    module sun {
      function foo() i32 {
        return 1;
      }
    }
    
    module sun {
      function bar() i32 {
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

TEST(ModuleTest, redeclare_function_same_signature_errors) {
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

TEST(ModuleTest, redeclare_class_errors) {
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

TEST(ModuleTest, redeclare_interface_errors) {
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

TEST(ModuleTest, shadow_global_variable_errors) {
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

TEST(ModuleTest, local_shadowing_allowed) {
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

TEST(ModuleTest, using_is_lexically_scoped) {
  // A 'using' inside a function should not leak to a sibling function
  auto value = executeString(R"(
    module math {
      function square(x: i32) i32 { return x * x; }
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

TEST(ModuleTest, using_causes_ambiguity) {
  // A 'using' inside a function should not leak to a sibling function
  EXPECT_THROW(executeString(R"(
    module math {
      function foo() i32 { return 1; }
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