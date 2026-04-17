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
