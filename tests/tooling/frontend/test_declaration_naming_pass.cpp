#include <gtest/gtest.h>

#include <sstream>

#include "ast.h"
#include "ast/ast_children.h"
#include "driver/execution_utils.h"
#include "parsing/parser.h"
#include "semantic_analysis/declaration_naming_pass.h"
#include "semantic_analysis/field_initializer_preparation_pass.h"
#include "semantic_analysis/semantic_pipeline.h"

namespace {

/** Parse declarations without performing semantic analysis. */
std::unique_ptr<BlockExprAST> parseDeclarations(const std::string& source) {
  std::istringstream input(source);
  Parser parser(input);
  return parser.parseString(source);
}

}  // namespace

TEST(Tooling_Frontend_DeclarationNames, names_do_not_require_resolved_types) {
  auto program = parseDeclarations(R"(
    module library {
      class Box<T> {
        var value: T;
        init(value: T) { this.value = value; }
        method get() T { return this.value; }
      }
      interface Shape { method size() i32; }
      enum Color { Red, Blue }
      var storage: Unknown = 0;
      function consume(value: Unknown) Unknown { return value; }
    }
  )");
  sun::DeclarationNamingPass{}.run(*program);
  const auto& module = static_cast<ModuleAST&>(*program->getBody()[0]);
  const auto& declarations = module.getBody().getBody();
  const auto& box = static_cast<ClassDefinitionAST&>(*declarations[0]);
  EXPECT_EQ(module.getQualifiedName().display(), "library");
  EXPECT_EQ(box.getQualifiedName().display(), "library.Box");
  EXPECT_EQ(
      box.getMethods()[0].function->getProto().getQualifiedName().display(),
      "library.Box.init");
  const auto& method = box.getMethods()[1].function->getProto();
  EXPECT_EQ(method.getQualifiedName().display(), "library.Box.get");
  EXPECT_EQ(method.getQualifiedName().owner(),
            std::vector<std::string>({"library"}));
  const auto& shape = static_cast<InterfaceDefinitionAST&>(*declarations[1]);
  EXPECT_EQ(shape.getQualifiedName().display(), "library.Shape");
  EXPECT_EQ(
      shape.getMethods()[0].function->getProto().getQualifiedName().display(),
      "library.Shape.size");
  EXPECT_EQ(static_cast<EnumDefinitionAST&>(*declarations[2])
                .getQualifiedName()
                .display(),
            "library.Color");
  EXPECT_EQ(static_cast<VariableCreationAST&>(*declarations[3])
                .getQualifiedName()
                .display(),
            "library.storage");
  const auto& function = static_cast<FunctionAST&>(*declarations[4]);
  EXPECT_EQ(function.getProto().getQualifiedName().display(),
            "library.consume");
  EXPECT_TRUE(function.getProto().getQualifiedName().paramSuffix.empty());
  EXPECT_FALSE(function.getProto().hasResolvedParamTypes());
  EXPECT_FALSE(function.getProto().hasResolvedReturnType());
  EXPECT_FALSE(box.getResolvedType());
}

TEST(Tooling_Frontend_DeclarationNames, nested_and_reopened_modules) {
  auto program = parseDeclarations(R"(
    module outer { module inner { function first<T>(x: T) T { return x; } } }
    module outer { module inner { function second<T>(x: T) T { return x; } } }
  )");
  sun::DeclarationNamingPass{}.run(*program);
  for (const auto& declaration : program->getBody()) {
    const auto& outer = static_cast<ModuleAST&>(*declaration);
    const auto& inner = static_cast<ModuleAST&>(*outer.getBody().getBody()[0]);
    const auto& function =
        static_cast<FunctionAST&>(*inner.getBody().getBody()[0]);
    EXPECT_EQ(inner.getQualifiedName().display(), "outer.inner");
    EXPECT_EQ(inner.getQualifiedName().owner(),
              std::vector<std::string>({"outer"}));
    EXPECT_EQ(function.getProto().getQualifiedName().scopePath,
              std::vector<std::string>({"outer", "inner"}));
    EXPECT_EQ(function.getProto().getQualifiedName().owner(),
              std::vector<std::string>({"outer", "inner"}));
  }
}

TEST(Tooling_Frontend_DeclarationNames, preserves_bundle_identity_under_alias) {
  auto program = parseDeclarations(R"(
    module alias { function identity<T>(x: T) T { return x; } }
  )");
  auto& module = static_cast<ModuleAST&>(*program->getBody()[0]);
  module.setQualifiedName({{"$bundle$"}, "original", {"$bundle$"}});
  auto moon = MoonScopeAST::forOwnBundle("$bundle$", std::move(program));
  sun::DeclarationNamingPass{}.run(*moon);
  const auto& function =
      static_cast<FunctionAST&>(*module.getBody().getBody()[0]);
  EXPECT_EQ(module.getQualifiedName().display(), "original");
  EXPECT_EQ(function.getProto().getQualifiedName().scopePath,
            std::vector<std::string>({"$bundle$", "original"}));
  EXPECT_EQ(function.getProto().getQualifiedName().owner(),
            std::vector<std::string>({"$bundle$", "original"}));
}

TEST(Tooling_Frontend_DeclarationNames,
     repeated_naming_preserves_specializations) {
  auto program =
      parseDeclarations("function identity<T>(x: T) T { return x; }");
  auto& proto = static_cast<FunctionAST&>(*program->getBody()[0]).getProtoMut();
  sun::QualifiedName specialized({"library"}, "identity_i32", {"library"});
  specialized.paramSuffix = "$i32";
  proto.setQualifiedName(specialized);
  sun::DeclarationNamingPass{}.run(*program);
  sun::DeclarationNamingPass{}.run(*program);
  EXPECT_EQ(proto.getQualifiedName(), specialized);
  EXPECT_EQ(proto.getQualifiedName().owner(), specialized.owner());
}

TEST(Tooling_Frontend_DeclarationNames,
     bodies_wait_for_the_enclosing_signature) {
  auto program = parseDeclarations(R"(
    function work(x: i32) i32 {
      enum Choice { First, Second }
      var local: i32 = x;
      return local;
    }
  )");
  sun::DeclarationNamingPass{}.run(*program);
  auto& function = static_cast<FunctionAST&>(*program->getBody()[0]);
  auto& body = const_cast<BlockExprAST&>(function.getBody());
  auto& choice = static_cast<EnumDefinitionAST&>(*body.getBody()[0]);
  auto& local = static_cast<VariableCreationAST&>(*body.getBody()[1]);
  EXPECT_FALSE(choice.hasQualifiedName());
  SemanticContext context(std::make_shared<sun::TypeRegistry>());
  context.enterFunctionScope("work$i32", sun::QualifiedName({}, "work$i32"));
  sun::assignLocalDeclarationName(choice, context.getCurrentScopePath(),
                                  context.currentModulePath());
  EXPECT_EQ(choice.getQualifiedName().scopePath,
            std::vector<std::string>({"work$i32"}));
  EXPECT_FALSE(local.hasQualifiedName());
}

TEST(Tooling_Frontend_DeclarationNames,
     local_types_in_overloads_stay_distinct) {
  EXPECT_EQ(executeString(R"(
    function value(x: i32) i32 {
      enum Choice { First, Second }
      var choice: Choice = Choice.First;
      if (choice == Choice.First) { return 20; }
      return 0;
    }
    function value(x: bool) i32 {
      enum Choice { First, Second }
      var choice: Choice = Choice.Second;
      if (choice == Choice.Second) { return 22; }
      return 0;
    }
    function main() i32 { return value(1) + value(true); }
  )"),
            42);
}

TEST(Tooling_Frontend_DeclarationNames, synthesized_constructors_are_named) {
  auto program = parseDeclarations(R"(
    module library { class Box { var value: i32 = 42; } }
  )");
  sun::FieldInitializerPreparationPass{}.run(*program);
  sun::DeclarationNamingPass{}.run(*program);
  const auto& module = static_cast<ModuleAST&>(*program->getBody()[0]);
  const auto& box =
      static_cast<ClassDefinitionAST&>(*module.getBody().getBody()[0]);
  ASSERT_EQ(box.getMethods().size(), 1u);
  const auto& constructor = *box.getMethods()[0].function;
  EXPECT_TRUE(constructor.isSynthesizedConstructor());
  EXPECT_EQ(constructor.getProto().getQualifiedName().display(),
            "library.Box.init");
  EXPECT_FALSE(constructor.getProto().hasResolvedParamTypes());
}

TEST(Tooling_Frontend_DeclarationNames,
     local_types_in_specializations_stay_distinct) {
  EXPECT_EQ(executeString(R"(
    function value<T>(x: T) i32 {
      enum Choice { First, Second }
      var choice: Choice = Choice.Second;
      if (choice == Choice.Second) { return 21; }
      return 0;
    }
    function main() i32 { return value<i32>(1) + value<bool>(true); }
  )"),
            42);
}

TEST(Tooling_Frontend_DeclarationNames,
     pipeline_prepares_names_and_registers_before_bodies) {
  auto program = parseDeclarations(R"(
    function answer() i32 {
      var counter = Counter();
      return identity(counter.value);
    }
    class Counter { var value: i32 = 42; }
    function identity<T>(value: T) T { return value; }
  )");
  SemanticAnalyzer analyzer(std::make_shared<sun::TypeRegistry>());
  auto& pipeline = analyzer.pipeline();
  pipeline.run(*program);

  const auto& counter =
      static_cast<ClassDefinitionAST&>(*program->getBody()[1]);
  ASSERT_EQ(counter.getMethods().size(), 1u);
  EXPECT_TRUE(counter.getMethods()[0].function->isSynthesizedConstructor());
  EXPECT_EQ(
      counter.getMethods()[0].function->getProto().getQualifiedName().display(),
      "Counter.init");
  auto* identity = static_cast<FunctionAST*>(program->getBody()[2].get());
  const auto* registered = analyzer.context().lookupGenericFunction("identity");
  ASSERT_NE(registered, nullptr);
  EXPECT_EQ(registered->AST, identity);
  EXPECT_EQ(registered->qualifiedName, identity->getProto().getQualifiedName());
  EXPECT_TRUE(identity->getProto().getQualifiedName().paramSuffix.empty());
  EXPECT_TRUE(counter.getResolvedType());
}

TEST(Tooling_Frontend_DeclarationNames,
     collection_does_not_check_function_bodies) {
  auto program = parseDeclarations(R"(
    function answer() i32 { return missing; }
  )");
  sun::FieldInitializerPreparationPass{}.run(*program);
  sun::DeclarationNamingPass{}.run(*program);
  SemanticAnalyzer analyzer(std::make_shared<sun::TypeRegistry>());
  auto& pipeline = analyzer.pipeline();
  pipeline.prepareGenerated(*program);

  auto& collection = pipeline.declarations();
  EXPECT_NO_THROW(collection.run(*program));
  EXPECT_EQ(analyzer.context().getAllFunctions("answer").size(), 1u);

  EXPECT_SUN_ERROR_WITH_MESSAGE(analyzer.bodies().analyzeBlock(*program),
                                "Unknown variable");
}

TEST(Tooling_Frontend_DeclarationNames,
     field_preparation_reaches_function_method_and_lambda_bodies) {
  auto program = parseDeclarations(R"(
    function work() void {
      class Local { var value: Unknown = 1; }
      var callback = () => void {
        class InLambda { var value: Unknown = 2; }
      };
    }
    class Host {
      method work() void {
        class InMethod { var value: Unknown = 3; }
      }
    }
  )");
  sun::FieldInitializerPreparationPass{}.run(*program);
  size_t preparedClasses = 0;
  const auto inspect = [&](auto&& self, const ExprAST& node) -> void {
    if (node.getType() == ASTNodeType::CLASS_DEFINITION) {
      const auto& declaration = static_cast<const ClassDefinitionAST&>(node);
      if (declaration.getName() != "Host") {
        ++preparedClasses;
        ASSERT_EQ(declaration.getMethods().size(), 1u);
        const auto& constructor = *declaration.getMethods()[0].function;
        EXPECT_TRUE(constructor.isSynthesizedConstructor());
        EXPECT_EQ(constructor.getFieldInitializerCount(), 1u);
        EXPECT_EQ(constructor.getBody().getBody().size(), 1u);
        EXPECT_FALSE(constructor.getProto().hasQualifiedName());
      }
    }
    forEachChild(node, [&](const ExprAST& child) { self(self, child); });
  };
  inspect(inspect, *program);
  EXPECT_EQ(preparedClasses, 3u);
}

TEST(Tooling_Frontend_DeclarationNames,
     pipeline_checks_local_classes_without_lowering) {
  auto program = parseDeclarations(R"(
    function value<T>(x: T) i32 {
      class Local { var value: i32 = 21; }
      var local = Local();
      return local.value;
    }
    class Host {
      init() {}
      method value() i32 {
        class Local { var value: i32 = 21; }
        var local = Local();
        return local.value;
      }
    }
    function answer() i32 {
      var callback = () => i32 {
        class Local { var value: i32 = 21; }
        var local = Local();
        return local.value;
      };
      var host = Host();
      return value(1) + value(true) + host.value() + callback();
    }
  )");
  SemanticAnalyzer analyzer(std::make_shared<sun::TypeRegistry>());
  EXPECT_NO_THROW(analyzer.pipeline().run(*program));
}

TEST(Tooling_Frontend_DeclarationNames,
     distinct_ids_do_not_allow_duplicate_names) {
  auto program = parseDeclarations(R"(
    class Item { var value: i32; }
    class Item { var value: i32; }
  )");
  SemanticAnalyzer analyzer(std::make_shared<sun::TypeRegistry>());
  EXPECT_SUN_ERROR_WITH_MESSAGE(analyzer.pipeline().run(*program),
                                "Redefinition of class 'Item'");
}

TEST(Tooling_Frontend_DeclarationNames, local_naming_does_not_walk_method_bodies) {
  auto program = parseDeclarations(R"(
    class Local {
      method work() void { enum Inner { First, Second } }
    }
  )");
  auto& local = static_cast<ClassDefinitionAST&>(*program->getBody()[0]);
  sun::assignLocalDeclarationName(local, {"owner$i32"}, {});
  EXPECT_TRUE(local.hasQualifiedName());
  const auto& method = *local.getMethods()[0].function;
  EXPECT_TRUE(method.getProto().hasQualifiedName());
  const auto& inner =
      static_cast<const EnumDefinitionAST&>(*method.getBody().getBody()[0]);
  EXPECT_FALSE(inner.hasQualifiedName());
}
