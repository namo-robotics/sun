// tests/tooling/backend/test_compilation.cpp
// Tests for AOT compilation features

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ast/ast_children.h"
#include "codegen/functions/function_registry.h"
#include "driver/driver.h"

// Helper function to test compilation (without JIT)
void compileString(const std::string& source) {
  Driver::createForAOT()->compileString(source);
}

// === Valid compilation tests ===

TEST(Tooling_Backend_Compilation, main_returns_i32) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
        return 0;
    };
  )"));
}

TEST(Tooling_Backend_Compilation, main_returns_i32_with_computation) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
        var x: i32 = 10;
        var y: i32 = 20;
        return x + y;
    };
  )"));
}

TEST(Tooling_Backend_Compilation, main_returns_i32_with_functions) {
  EXPECT_NO_THROW(compileString(R"(
function add(a: i32, b: i32) i32 {
        return a + b;
    };

    function main() i32 {
        return add(1, 2);
    };
  )"));
}

// === Invalid compilation tests (main must return i32) ===

TEST(Tooling_Backend_Compilation, main_returns_f64_fails) {
  EXPECT_THROW(compileString(R"(
        function main() f64 {
            return 3.14;
        };
      )"),
               SunError);
}

TEST(Tooling_Backend_Compilation, main_returns_f32_fails) {
  EXPECT_THROW(compileString(R"(
        function main() f32 {
            return 3.14;
        };
      )"),
               SunError);
}

TEST(Tooling_Backend_Compilation, main_returns_i64_fails) {
  EXPECT_THROW(compileString(R"(
        function main() i64 {
            return 42;
        };
      )"),
               SunError);
}

TEST(Tooling_Backend_Compilation, main_returns_bool_fails) {
  EXPECT_THROW(compileString(R"(
        function main() bool {
            1 < 2;
        };
      )"),
               SunError);
}

TEST(Tooling_Backend_Compilation, main_returns_string_fails) {
  EXPECT_THROW(compileString(R"(
        function main() static_ptr<u8> {
            "hello";
        };
      )"),
               SunError);
}

// Test that error message is informative
TEST(Tooling_Backend_Compilation, error_message_contains_type_info) {
  try {
    compileString(R"(
      function main() f64 {
          return 3.14;
      };
    )");
    FAIL() << "Expected SunError";
  } catch (const SunError& e) {
    std::string msg = e.what();
    EXPECT_TRUE(msg.find("i32") != std::string::npos)
        << "Error should mention i32";
    EXPECT_TRUE(msg.find("f64") != std::string::npos)
        << "Error should mention f64";
  }
}

TEST(Tooling_Backend_Compilation, function_ids_survive_symbol_renaming) {
  CodegenContext context("function_ids", nullptr);
  auto types = std::make_shared<sun::TypeRegistry>();
  CodegenState state(context, types);
  FunctionRegistry functions(state);
  auto first = types->declarations.add(sun::DeclarationKind::Function, "f");
  auto second = types->declarations.add(sun::DeclarationKind::Function, "f");
  auto* signature = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context.getContext()), false);
  auto* original =
      llvm::Function::Create(signature, llvm::Function::ExternalLinkage, "f",
                             context.mainModule.get());
  functions.registerFunction(first, original);
  original->setName("renamed");
  auto* replacement =
      llvm::Function::Create(signature, llvm::Function::ExternalLinkage, "f",
                             context.mainModule.get());
  functions.registerFunction(second, replacement);
  EXPECT_EQ(functions.lookupFunctionById(first), original);
  EXPECT_EQ(functions.lookupFunctionById(second), replacement);
  EXPECT_ANY_THROW(functions.registerFunction(first, replacement));
  original->eraseFromParent();
  EXPECT_ANY_THROW(functions.lookupFunctionById(first));
  functions.registerFunction(first, replacement);
  EXPECT_EQ(functions.lookupFunctionById(first), replacement);
  EXPECT_ANY_THROW(functions.lookupFunctionById(sun::DeclarationId{}));
}

TEST(Tooling_Backend_Compilation, generic_method_target_ignores_call_symbol) {
  auto driver = Driver::createForJIT();
  size_t checked = 0;
  driver->setMetadataCallback([&](const BlockExprAST& ast, SemanticAnalyzer&) {
    std::function<void(const ExprAST&)> visit = [&](const ExprAST& node) {
      if (node.getType() == ASTNodeType::MEMBER_ACCESS) {
        const auto& member = static_cast<const MemberAccessAST&>(node);
        if (member.getMemberName() == "identity") {
          EXPECT_TRUE(member.getTargetDeclarationId());
          member.setQualifiedName({{}, "unrelated_symbol"});
          ++checked;
        }
      }
      forEachChild(node, visit);
    };
    visit(ast);
  });
  auto value = driver->executeString(R"(
    class Box {
      public method identity<T>(value: T) T { return value; }
    }
    function main() i32 {
      var box = Box();
      return box.identity<i32>(19) + box.identity(23);
    }
  )");
  EXPECT_EQ(value, 42);
  EXPECT_EQ(checked, 2u);
}

TEST(Tooling_Backend_Compilation, variadic_method_target_ignores_call_symbol) {
  auto driver = Driver::createForJIT();
  size_t checked = 0;
  driver->setMetadataCallback([&](const BlockExprAST& ast, SemanticAnalyzer&) {
    std::function<void(const ExprAST&)> visit = [&](const ExprAST& node) {
      if (node.getType() == ASTNodeType::MEMBER_ACCESS) {
        const auto& member = static_cast<const MemberAccessAST&>(node);
        if (member.getMemberName() == "create") {
          EXPECT_TRUE(member.getTargetDeclarationId());
          member.setQualifiedName({{}, "unrelated_symbol"});
          ++checked;
        }
      }
      forEachChild(node, visit);
    };
    visit(ast);
  });
  auto value = driver->executeString(R"(
    function main() i32 {
      var factory = Factory();
      return factory.create<Point>(42, 3, 4);
    }
    class Point {
      init(x: i32, y: i32) {}
    }
    class Factory {
      public method create<T>(tag: i32, args...: _params_of<T>) i32 {
        return tag;
      }
    }
  )");
  EXPECT_EQ(value, 42);
  EXPECT_EQ(checked, 1u);
}

TEST(Tooling_Backend_Compilation, generic_method_missing_target_is_an_error) {
  auto driver = Driver::createForJIT();
  driver->setMetadataCallback([](const BlockExprAST& ast, SemanticAnalyzer&) {
    std::function<void(const ExprAST&)> visit = [&](const ExprAST& node) {
      if (node.getType() == ASTNodeType::MEMBER_ACCESS) {
        const auto& member = static_cast<const MemberAccessAST&>(node);
        if (member.getMemberName() == "identity") {
          member.setTargetDeclarationId({});
        }
      }
      forEachChild(node, visit);
    };
    visit(ast);
  });
  EXPECT_THROW(driver->executeString(R"(
    class Box {
      public method identity<T>(value: T) T { return value; }
    }
    function main() i32 {
      var box = Box();
      return box.identity<i32>(42);
    }
  )"),
               SunError);
}

TEST(Tooling_Backend_Compilation, selected_methods_ignore_reference_spelling) {
  auto driver = Driver::createForJIT();
  size_t checked = 0;
  driver->setMetadataCallback([&](const BlockExprAST& ast, SemanticAnalyzer&) {
    std::function<void(const ExprAST&)> visit = [&](const ExprAST& node) {
      if (node.getType() == ASTNodeType::MEMBER_ACCESS) {
        const auto& member = static_cast<const MemberAccessAST&>(node);
        if (member.getMemberName() == "value") {
          EXPECT_TRUE(member.getTargetDeclarationId());
          const_cast<std::string&>(member.getMemberName()) =
              "renamed_reference";
          ++checked;
        }
      }
      forEachChild(node, visit);
    };
    visit(ast);
  });
  auto result = driver->executeString(R"(
    interface Value { public method value(x: i32) i32; }
    class Box implements Value {
      public method value(x: bool) i32 { return 100; }
      public method value(x: i32) i32 { return x; }
    }
    function dynamicValue(x: ref Value) i32 { return x.value(14); }
    function main() i32 {
      var box = Box();
      var bound: <'_>(i32) => i32 = box.value;
      return box.value(13) + bound(15) + dynamicValue(box);
    }
  )");
  EXPECT_EQ(result, 42);
  EXPECT_EQ(checked, 3u);
}

TEST(Tooling_Backend_Compilation, default_wrappers_have_distinct_targets) {
  auto driver = Driver::createForJIT();
  std::set<sun::DeclarationId> targets;
  driver->setMetadataCallback([&](const BlockExprAST& ast, SemanticAnalyzer&) {
    std::function<void(const ExprAST&)> visit = [&](const ExprAST& node) {
      if (node.getType() == ASTNodeType::MEMBER_ACCESS) {
        const auto& member = static_cast<const MemberAccessAST&>(node);
        if (member.getMemberName() == "value") {
          EXPECT_TRUE(member.getTargetDeclarationId());
          targets.insert(member.getTargetDeclarationId());
        }
      }
      forEachChild(node, visit);
    };
    visit(ast);
  });
  auto result = driver->executeString(R"(
    interface Value { public method value() i32 { return 21; } }
    class First implements Value {}
    class Second implements Value {}
    function main() i32 {
      var first = First();
      var second = Second();
      return first.value() + second.value();
    }
  )");
  EXPECT_EQ(result, 42);
  EXPECT_EQ(targets.size(), 2u);
}
