#include <gtest/gtest.h>

#include <set>
#include <sstream>

#include "ast.h"
#include "ast/ast_children.h"
#include "driver/driver.h"
#include "parsing/parser.h"
#include "semantic_analysis/declaration_identity_pass.h"

namespace {

/** Parse syntax without resolving any declaration signatures. */
std::unique_ptr<BlockExprAST> parse(const std::string& source) {
  std::istringstream input(source);
  Parser parser(input);
  return parser.parseString(source);
}

}  // namespace

TEST(Tooling_Frontend_DeclarationIdentity,
     nested_types_exist_before_signatures) {
  auto ast = parse(R"(
    function work(x: i32) void { class Local {} }
    function work(x: bool) void { class Local {} }
  )");
  sun::DeclarationTable table;
  sun::DeclarationIdentityPass pass(table);
  pass.run(*ast);
  const auto& first = static_cast<const FunctionAST&>(*ast->getBody()[0]);
  const auto& second = static_cast<const FunctionAST&>(*ast->getBody()[1]);
  const auto& firstLocal = *first.getBody().getBody()[0];
  const auto& secondLocal = *second.getBody().getBody()[0];
  EXPECT_NE(firstLocal.getDeclarationId(), secondLocal.getDeclarationId());
  EXPECT_EQ(table.get(firstLocal.getDeclarationId()).owner,
            first.getDeclarationId());
  EXPECT_EQ(table.get(secondLocal.getDeclarationId()).owner,
            second.getDeclarationId());
  EXPECT_FALSE(first.getProto().hasResolvedParamTypes());
  EXPECT_EQ(first.getDeclarationId(), first.getProto().getDeclarationId());
  auto size = table.size();
  pass.run(*ast);
  EXPECT_EQ(table.size(), size);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     computed_reset_preserves_only_identity) {
  auto ast = parse("function f(x: i32) i32 { return x; }");
  sun::DeclarationTable table;
  sun::DeclarationIdentityPass(table).run(*ast);
  auto& function = static_cast<FunctionAST&>(*ast->getBody()[0]);
  const auto id = function.getDeclarationId();
  const auto parameter = function.declarationIdentity().parameters[0];
  function.setResolvedType(sun::Types::Int32());
  function.setTargetDeclarationId(id);
  function.getProtoMut().setQualifiedName({{}, "computed"});
  sun::clearComputedAnalysis(*ast);
  EXPECT_EQ(function.getDeclarationId(), id);
  EXPECT_EQ(function.declarationIdentity().parameters[0], parameter);
  EXPECT_FALSE(function.hasResolvedType());
  EXPECT_FALSE(function.getTargetDeclarationId());
  EXPECT_FALSE(function.getProto().hasQualifiedName());
  sun::resetAnalysisSession(*ast);
  EXPECT_FALSE(function.getDeclarationId());
  EXPECT_FALSE(function.getProto().hasAnalysis());
  sun::DeclarationTable next;
  sun::DeclarationIdentityPass(next).run(*ast);
  EXPECT_TRUE(function.getDeclarationId());
  EXPECT_EQ(next.size(), table.size());
}

TEST(Tooling_Frontend_DeclarationIdentity, cloning_does_not_copy_session_ids) {
  auto ast = parse("function f<T>(x: T) T { class Local {} return x; }");
  sun::DeclarationTable table;
  sun::DeclarationIdentityPass pass(table);
  pass.run(*ast);
  const auto& original = static_cast<const FunctionAST&>(*ast->getBody()[0]);
  auto clone = original.clone();
  EXPECT_FALSE(clone->getDeclarationId());
  pass.run(*clone);
  const auto& copied = static_cast<const FunctionAST&>(*clone);
  EXPECT_NE(copied.getDeclarationId(), original.getDeclarationId());
  EXPECT_NE(copied.getBody().getBody()[0]->getDeclarationId(),
            original.getBody().getBody()[0]->getDeclarationId());
  EXPECT_NE(copied.declarationIdentity().parameters[0],
            original.declarationIdentity().parameters[0]);
}

TEST(Tooling_Frontend_DeclarationIdentity, members_retain_module_ownership) {
  auto ast = parse(R"(
    module m {
      class Box<T> { var value: T; method get() T { return this.value; } }
      enum Choice { A, B }
    }
  )");
  sun::DeclarationTable table;
  sun::DeclarationIdentityPass(table).run(*ast);
  const auto& module = static_cast<const ModuleAST&>(*ast->getBody()[0]);
  const auto& cls =
      static_cast<const ClassDefinitionAST&>(*module.getBody().getBody()[0]);
  const auto& field = cls.getFields()[0];
  EXPECT_EQ(table.get(field.declaration.id).owner, cls.getDeclarationId());
  EXPECT_EQ(table.get(field.declaration.id).module, module.getDeclarationId());
  const auto& method = *cls.getMethods()[0].function;
  EXPECT_EQ(table.get(method.getDeclarationId()).owner, cls.getDeclarationId());
  auto fieldId = field.declaration.id;
  sun::clearComputedAnalysis(*ast);
  EXPECT_EQ(field.declaration.id, fieldId);
  sun::resetAnalysisSession(*ast);
  EXPECT_FALSE(field.declaration.id);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     reused_tree_requires_a_session_reset) {
  auto ast = parse("function f() void {}");
  sun::DeclarationTable first;
  sun::DeclarationTable second;
  sun::DeclarationIdentityPass(first).run(*ast);
  second.add(sun::DeclarationKind::Function, "unrelated");
  EXPECT_ANY_THROW(sun::DeclarationIdentityPass(second).run(*ast));
  sun::resetAnalysisSession(*ast);
  EXPECT_NO_THROW(sun::DeclarationIdentityPass(second).run(*ast));
}

TEST(Tooling_Frontend_DeclarationIdentity, reopened_modules_share_identity) {
  auto ast = parse(
      "module m { function a() void {} } module m { function b() void {} }");
  sun::DeclarationTable table;
  sun::DeclarationIdentityPass(table).run(*ast);
  EXPECT_EQ(ast->getBody()[0]->getDeclarationId(),
            ast->getBody()[1]->getDeclarationId());
}

TEST(Tooling_Frontend_DeclarationIdentity,
     resolved_calls_and_parameters_carry_ids) {
  auto driver = Driver::createForJIT();
  auto result = driver->analyzeString(R"(
    function twice(x: i32) i32 { return x + x; }
    function main() i32 { var value: i32 = 7; return twice(value); }
  )");
  ASSERT_FALSE(result.error.has_value());
  const auto& function =
      static_cast<const FunctionAST&>(*result.ast->getBody()[0]);
  const auto& main = static_cast<const FunctionAST&>(*result.ast->getBody()[1]);
  const auto variableId = main.getBody().getBody()[0]->getDeclarationId();
  size_t checked = 0;
  std::function<void(const ExprAST&)> visit = [&](const ExprAST& node) {
    if (node.getType() == ASTNodeType::CALL) {
      EXPECT_EQ(node.getTargetDeclarationId(), function.getDeclarationId());
      ++checked;
    }
    if (node.getType() == ASTNodeType::VARIABLE_REFERENCE) {
      const auto& reference = static_cast<const VariableReferenceAST&>(node);
      if (reference.getName() == "x") {
        EXPECT_EQ(reference.getTargetDeclarationId(),
                  function.declarationIdentity().parameters[0]);
        ++checked;
      }
      if (reference.getName() == "value") {
        EXPECT_EQ(reference.getTargetDeclarationId(), variableId);
        ++checked;
      }
    }
    forEachChild(node, visit);
  };
  visit(*result.ast);
  EXPECT_EQ(checked, 4u);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     analyzed_snapshots_own_their_session) {
  auto driver = Driver::createForJIT();
  auto first = driver->analyzeString("function f() i32 { return 1; }");
  auto second = driver->analyzeString("function f() i32 { return 2; }");
  ASSERT_FALSE(first.error);
  ASSERT_FALSE(second.error);
  ASSERT_TRUE(first.typeRegistry);
  ASSERT_TRUE(second.typeRegistry);
  EXPECT_NE(first.typeRegistry, second.typeRegistry);
  driver.reset();
  const auto& function = *first.ast->getBody()[0];
  EXPECT_EQ(
      first.typeRegistry->declarations.get(function.getDeclarationId()).name,
      "f");
  EXPECT_FALSE(function.declarationIdentity().session.expired());
  EXPECT_ANY_THROW(
      sun::DeclarationIdentityPass(second.typeRegistry->declarations)
          .run(*first.ast));
}

TEST(Tooling_Frontend_DeclarationIdentity, nominal_types_precede_names) {
  auto ast = parse(R"(
    function work(x: i32) void { class Local {} }
    function work(x: bool) void { class Local {} }
  )");
  sun::TypeRegistry types;
  sun::DeclarationIdentityPass(types.declarations).run(*ast);
  auto& first = static_cast<FunctionAST&>(*ast->getBody()[0]);
  auto& second = static_cast<FunctionAST&>(*ast->getBody()[1]);
  auto a = first.getBody().getBody()[0]->getDeclarationId();
  auto b = second.getBody().getBody()[0]->getDeclarationId();
  auto firstType = types.getClass(a);
  auto secondType = types.getClass(b);
  EXPECT_NE(firstType, secondType);
  EXPECT_FALSE(firstType->equals(*secondType));
  EXPECT_EQ(types.getClass(a, {{"work_i32"}, "Local"}), firstType);
  EXPECT_EQ(types.getClass(a), firstType);
  EXPECT_TRUE(firstType->equals(*firstType));
}

TEST(Tooling_Frontend_DeclarationIdentity, nominal_types_distinguish_sessions) {
  sun::TypeRegistry first;
  sun::TypeRegistry second;
  auto a = first.declarations.add(sun::DeclarationKind::Interface, "Local");
  auto b = second.declarations.add(sun::DeclarationKind::Interface, "Local");
  ASSERT_EQ(a, b);
  EXPECT_FALSE(first.getInterface(a)->equals(*second.getInterface(b)));
  auto e = first.declarations.add(sun::DeclarationKind::Enum, "Value");
  auto f = first.declarations.add(sun::DeclarationKind::Enum, "Value");
  EXPECT_FALSE(first.getEnum(e)->equals(*first.getEnum(f)));
  EXPECT_ANY_THROW(first.getClass(a));
  EXPECT_ANY_THROW(first.getEnum(sun::DeclarationId{}));
}

TEST(Tooling_Frontend_DeclarationIdentity,
     generated_function_preparation_belongs_to_pipeline) {
  auto ast = parse("function work(x: i32) i32 { return x; }");
  auto types = std::make_shared<sun::TypeRegistry>();
  SemanticAnalyzer analyzer(types);
  auto& function = static_cast<FunctionAST&>(*ast->getBody()[0]);
  analyzer.pipeline().prepareGenerated(function, std::vector<std::string>{},
                                       {});
  auto id = function.getDeclarationId();
  ASSERT_TRUE(id);
  auto count = types->declarations.size();
  auto info = analyzer.getFunctionInfo(function);
  EXPECT_EQ(info.declarationId, id);
  EXPECT_EQ(types->declarations.size(), count);
  analyzer.pipeline().prepareGenerated(function, std::vector<std::string>{},
                                       {});
  EXPECT_EQ(function.getDeclarationId(), id);
  EXPECT_EQ(types->declarations.size(), count);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     inferred_and_explicit_generic_calls_share_a_target) {
  auto driver = Driver::createForJIT();
  auto result = driver->analyzeString(R"(
    function identity<T>(value: T) T { return value; }
    function main() i32 { return identity(1) + identity<i32>(2); }
  )");
  ASSERT_FALSE(result.error);
  const auto& generic =
      static_cast<const FunctionAST&>(*result.ast->getBody()[0]);
  ASSERT_EQ(generic.getSpecializations().size(), 1u);
  const auto target =
      generic.getSpecializations().begin()->second->getDeclarationId();
  ASSERT_TRUE(target);
  size_t checked = 0;
  std::function<void(const ExprAST&)> visit = [&](const ExprAST& node) {
    if (node.getType() == ASTNodeType::CALL ||
        node.getType() == ASTNodeType::GENERIC_CALL) {
      EXPECT_EQ(node.getTargetDeclarationId(), target);
      ++checked;
    }
    forEachChild(node, visit);
  };
  visit(*result.ast->getBody()[1]);
  EXPECT_EQ(checked, 2u);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     generic_interface_templates_use_ids) {
  auto driver = Driver::createForJIT();
  auto result = driver->analyzeString(R"(
    module first { interface Item<T> { method get() T; } }
    module second { interface Item<T> { method get() T; } }
  )");
  ASSERT_FALSE(result.error);
  const auto& first = static_cast<const ModuleAST&>(*result.ast->getBody()[0]);
  const auto& second = static_cast<const ModuleAST&>(*result.ast->getBody()[1]);
  auto firstId = first.getBody().getBody()[0]->getDeclarationId();
  auto secondId = second.getBody().getBody()[0]->getDeclarationId();
  auto firstType = result.typeRegistry->getInterface(firstId);
  auto secondType = result.typeRegistry->getInterface(secondId);
  EXPECT_NE(firstType, secondType);
  EXPECT_FALSE(firstType->equals(*secondType));
  EXPECT_EQ(firstType->getDeclarationId(), firstId);
  EXPECT_EQ(secondType->getDeclarationId(), secondId);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     specialization_keys_distinguish_nominal_arguments_and_templates) {
  sun::TypeRegistry types;
  auto templateId = types.declarations.add(sun::DeclarationKind::Class, "Box");
  auto otherTemplate =
      types.declarations.add(sun::DeclarationKind::Class, "Box");
  auto first = types.getClass(
      types.declarations.add(sun::DeclarationKind::Class, "Local"));
  auto second = types.getClass(
      types.declarations.add(sun::DeclarationKind::Class, "Local"));
  sun::SpecializationKey key{templateId, {}, {first}, std::nullopt};
  auto instance = types.specialize(key);
  EXPECT_EQ(types.specialize(key), instance);
  EXPECT_NE(types.specialize({templateId, {}, {second}, std::nullopt}),
            instance);
  EXPECT_NE(types.specialize({otherTemplate, {}, {first}, std::nullopt}),
            instance);
  ASSERT_TRUE(types.declarations.get(instance).specialization);
  EXPECT_EQ(types.declarations.get(instance).specialization->source,
            templateId);
  first->addField("recursive", sun::Types::Reference(first));
  EXPECT_EQ(types.specialize(key), instance);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     specialization_keys_compare_structure_and_variadic_packs) {
  sun::TypeRegistry types;
  auto source = types.declarations.add(sun::DeclarationKind::Function, "work");
  auto owner = types.declarations.add(sun::DeclarationKind::Class, "Owner");
  sun::SpecializationKey key{
      source, {}, {sun::Types::Reference(sun::Types::Int32())}, std::nullopt};
  auto instance = types.specialize(key);
  sun::SpecializationKey equal{
      source, {}, {sun::Types::Reference(sun::Types::Int32())}, std::nullopt};
  EXPECT_EQ(types.specialize(equal), instance);
  EXPECT_EQ(sun::SpecializationKeyHash{}(key),
            sun::SpecializationKeyHash{}(equal));
  equal.arguments = {sun::Types::Reference(sun::Types::Int32(), false)};
  EXPECT_NE(types.specialize(equal), instance);
  equal = key;
  equal.enclosing = owner;
  auto owned = types.specialize(equal);
  EXPECT_NE(owned, instance);
  EXPECT_EQ(types.declarations.get(owned).owner, owner);
  equal = key;
  equal.variadic = std::vector<sun::TypePtr>{};
  auto emptyPack = types.specialize(equal);
  EXPECT_NE(emptyPack, instance);
  equal.variadic = std::vector<sun::TypePtr>{sun::Types::Int32()};
  EXPECT_NE(types.specialize(equal), emptyPack);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     specialized_members_belong_to_the_concrete_class) {
  auto driver = Driver::createForJIT();
  auto result = driver->analyzeString(R"(
    class Box<T> { var value: T; init(value: T) { this.value = value; } }
    function main() i32 { var box = Box<i32>(7); return box.value; }
  )");
  ASSERT_FALSE(result.error);
  const auto& generic =
      static_cast<const ClassDefinitionAST&>(*result.ast->getBody()[0]);
  ASSERT_EQ(generic.getSpecializations().size(), 1u);
  const auto& [id, instance] = *generic.getSpecializations().begin();
  EXPECT_EQ(instance->getDeclarationId(), id);
  EXPECT_EQ(result.typeRegistry->getClass(id)->getDeclarationId(), id);
  EXPECT_EQ(result.typeRegistry->declarations.get(id).specialization->source,
            generic.getDeclarationId());
  for (const auto& method : instance->getMethods())
    EXPECT_EQ(result.typeRegistry->declarations
                  .get(method.function->getDeclarationId())
                  .owner,
              id);
  for (const auto& field : instance->getFields())
    EXPECT_EQ(result.typeRegistry->declarations.get(field.declaration.id).owner,
              id);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     recursive_generic_types_reuse_the_allocated_instance) {
  auto driver = Driver::createForJIT();
  auto result = driver->analyzeString(R"(
    class Node<T> { var next: raw_ptr<Node<T>>; }
    function inspect(node: ref Node<i32>) void {}
  )");
  ASSERT_FALSE(result.error);
  const auto& generic =
      static_cast<const ClassDefinitionAST&>(*result.ast->getBody()[0]);
  ASSERT_EQ(generic.getSpecializations().size(), 1u);
  const auto id = generic.getSpecializations().begin()->first;
  auto type = result.typeRegistry->getClass(id);
  ASSERT_EQ(type->getFields().size(), 1u);
  auto next =
      std::static_pointer_cast<sun::RawPointerType>(type->getFields()[0].type);
  EXPECT_EQ(next->getPointeeType(), type);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     interface_conformance_uses_session_ids) {
  sun::TypeRegistry types;
  auto first = types.getInterface(
      types.declarations.add(sun::DeclarationKind::Interface, "Readable"));
  auto second = types.getInterface(
      types.declarations.add(sun::DeclarationKind::Interface, "Readable"));
  auto implementation = types.getClass(
      types.declarations.add(sun::DeclarationKind::Class, "Value"));
  implementation->addImplementedInterface(*first);
  EXPECT_TRUE(implementation->implementsInterface(*first));
  EXPECT_FALSE(implementation->implementsInterface(*second));
  first->setQualifiedName({{"renamed"}, "Readable"});
  EXPECT_TRUE(implementation->convertibleToInterface(*first));
  implementation->markStaticOnlyInterface(*first);
  EXPECT_FALSE(implementation->convertibleToInterface(*first));
  EXPECT_TRUE(implementation->implementsInterface(*first));
  sun::TypeRegistry other;
  auto foreign = other.getInterface(
      other.declarations.add(sun::DeclarationKind::Interface, "Readable"));
  ASSERT_EQ(first->getDeclarationId(), foreign->getDeclarationId());
  EXPECT_FALSE(implementation->implementsInterface(*foreign));
  EXPECT_ANY_THROW(implementation->addImplementedInterface(*foreign));
}

TEST(Tooling_Frontend_DeclarationIdentity,
     abstract_arguments_use_binder_identity) {
  sun::TypeRegistry types;
  TypeParameter parameter("T");
  auto a = types.declarations.add(sun::DeclarationKind::TypeParameter, "T");
  auto b = types.declarations.add(sun::DeclarationKind::TypeParameter, "T");
  auto first = parameter.toSunType(types.declarations, a);
  auto repeated = parameter.toSunType(types.declarations, a);
  auto second = parameter.toSunType(types.declarations, b);
  EXPECT_TRUE(first->equals(*repeated));
  EXPECT_FALSE(first->equals(*second));
  auto source = types.declarations.add(sun::DeclarationKind::Class, "Box");
  auto instance = types.specialize({source, {}, {first}, std::nullopt});
  EXPECT_EQ(instance, types.specialize({source, {}, {repeated}, std::nullopt}));
  EXPECT_NE(instance, types.specialize({source, {}, {second}, std::nullopt}));
  auto projection = static_cast<const sun::TypeParameterType&>(*first).project(
      sun::TypeProjection::ReturnType);
  auto repeatedProjection =
      static_cast<const sun::TypeParameterType&>(*repeated).project(
          sun::TypeProjection::ReturnType);
  EXPECT_TRUE(projection->equals(*repeatedProjection));
  EXPECT_FALSE(projection->equals(*first));
}

TEST(Tooling_Frontend_DeclarationIdentity,
     generated_binders_retain_their_source) {
  auto ast = parse(R"(
    function outer<T>(value: T) T {
      class Local { var field: i32; }
      var copy = value;
      return copy;
    }
  )");
  sun::DeclarationTable table;
  sun::DeclarationIdentityPass pass(table);
  pass.run(*ast);
  const auto& source = static_cast<const FunctionAST&>(*ast->getBody()[0]);
  auto clone = source.clone();
  pass.run(*clone, {}, {}, &source);
  const auto& generated = static_cast<const FunctionAST&>(*clone);
  EXPECT_EQ(table.get(generated.getDeclarationId()).origin,
            source.getDeclarationId());
  EXPECT_EQ(table.get(generated.declarationIdentity().parameters[0]).origin,
            source.declarationIdentity().parameters[0]);
  EXPECT_EQ(table.get(generated.declarationIdentity().typeParameters[0]).origin,
            source.declarationIdentity().typeParameters[0]);
  auto& local =
      static_cast<const ClassDefinitionAST&>(*generated.getBody().getBody()[0]);
  auto& originalLocal =
      static_cast<const ClassDefinitionAST&>(*source.getBody().getBody()[0]);
  EXPECT_EQ(table.get(local.getDeclarationId()).owner,
            generated.getDeclarationId());
  EXPECT_EQ(table.get(local.getDeclarationId()).origin,
            originalLocal.getDeclarationId());
  EXPECT_EQ(table.get(local.getFields()[0].declaration.id).origin,
            originalLocal.getFields()[0].declaration.id);
  auto size = table.size();
  pass.run(*clone, {}, {}, &source);
  EXPECT_EQ(table.size(), size);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     resolved_generic_templates_ignore_names_and_request_scope) {
  auto ast = parse(R"(
    module first { public class Box<T> { method echo<U>(value: U) U { return value; } } }
    module second { class Box<T> {} }
    function use(value: ref first.Box<i32>) void {}
  )");
  auto types = std::make_shared<sun::TypeRegistry>();
  SemanticAnalyzer analyzer(types);
  analyzer.pipeline().run(*ast);
  const auto& first = static_cast<const ModuleAST&>(*ast->getBody()[0]);
  const auto& generic =
      static_cast<const ClassDefinitionAST&>(*first.getBody().getBody()[0]);
  ASSERT_EQ(generic.getSpecializations().size(), 1u);
  auto instance = types->getClass(generic.getSpecializations().begin()->first);
  instance->setGenericQualifiedName({{"second"}, "Box"});
  instance->setQualifiedName({{"second"}, "Box"});
  analyzer.context().enterModuleScope("second");
  auto* info = analyzer.generics().lookupGenericClassOf(*instance);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->AST, &generic);
  EXPECT_EQ(analyzer.generics().classDefinitionScope(*instance),
            SemanticContext::definitionScopeOf(*info));
  EXPECT_NE(analyzer.generics().findGenericMethodAST(instance.get(), "echo"),
            nullptr);
  auto unknown = types->getClass(
      types->declarations.add(sun::DeclarationKind::Class, "Box"));
  unknown->setQualifiedName({{"first"}, "Box"});
  EXPECT_EQ(analyzer.generics().lookupGenericClassOf(*unknown), nullptr);
  analyzer.context().exitScope();
}

TEST(Tooling_Frontend_DeclarationIdentity,
     resolved_local_generic_definitions_survive_closed_scopes) {
  auto ast = parse(R"(
    function first() void { class Local { method echo<T>(x: T) T { return x; } } }
    function second() void { class Local { method echo<T>(x: T) T { return x; } } }
  )");
  auto types = std::make_shared<sun::TypeRegistry>();
  SemanticAnalyzer analyzer(types);
  analyzer.pipeline().run(*ast);
  for (const auto& declaration : ast->getBody()) {
    const auto& function = static_cast<const FunctionAST&>(*declaration);
    const auto& local = static_cast<const ClassDefinitionAST&>(
        *function.getBody().getBody()[0]);
    auto type = types->getClass(local.getDeclarationId());
    type->setQualifiedName({{}, "unrelated"});
    auto* info = analyzer.generics().lookupGenericClassOf(*type);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->AST, &local);
    EXPECT_EQ(analyzer.generics().findGenericMethodAST(type.get(), "echo"),
              local.getMethods()[0].function.get());
  }
}

TEST(Tooling_Frontend_DeclarationIdentity,
     enum_variants_retain_concrete_owners_and_source_origins) {
  auto driver = Driver::createForJIT();
  auto result = driver->analyzeString(R"(
    enum Color { Red, Blue }
    enum Choice<T> { Some(T), None }
    function integer(value: Choice<i32>) void {}
    function boolean(value: Choice<bool>) void {}
  )");
  ASSERT_FALSE(result.error);
  const auto& plain =
      static_cast<const EnumDefinitionAST&>(*result.ast->getBody()[0]);
  auto plainType = result.typeRegistry->getEnum(plain.getDeclarationId());
  for (size_t i = 0; i < plain.getVariants().size(); ++i)
    EXPECT_EQ(plainType->getVariants()[i].declarationId,
              plain.getVariants()[i].declaration.id);
  const auto& generic =
      static_cast<const EnumDefinitionAST&>(*result.ast->getBody()[1]);
  ASSERT_EQ(generic.getSpecializations().size(), 2u);
  std::set<sun::DeclarationId> variants;
  for (const auto& [id, type] : generic.getSpecializations()) {
    for (size_t i = 0; i < type->getVariants().size(); ++i) {
      auto variantId = type->getVariants()[i].declarationId;
      EXPECT_TRUE(variants.insert(variantId).second);
      const auto& record = result.typeRegistry->declarations.get(variantId);
      EXPECT_EQ(record.owner, id);
      EXPECT_EQ(record.origin, generic.getVariants()[i].declaration.id);
    }
  }
}

TEST(Tooling_Frontend_DeclarationIdentity,
     cloned_method_lifetimes_retain_their_source_binder) {
  auto ast = parse(R"(
    class Box<T> {
      method apply<'a, U>(callback: <'a>(U) => U, value: U) U {
        return callback(value);
      }
    }
  )");
  sun::DeclarationTable table;
  sun::DeclarationIdentityPass pass(table);
  pass.run(*ast);
  const auto& original =
      static_cast<const ClassDefinitionAST&>(*ast->getBody()[0]);
  auto clone = original.clone();
  pass.run(*clone, {}, {}, &original);
  const auto& instance = static_cast<const ClassDefinitionAST&>(*clone);
  const auto& sourceMethod = *original.getMethods()[0].function;
  const auto& method = *instance.getMethods()[0].function;
  ASSERT_EQ(method.declarationIdentity().lifetimeParameters.size(), 1u);
  auto lifetime = method.declarationIdentity().lifetimeParameters[0];
  EXPECT_EQ(table.get(lifetime).owner, method.getDeclarationId());
  EXPECT_EQ(table.get(lifetime).origin,
            sourceMethod.declarationIdentity().lifetimeParameters[0]);
  EXPECT_EQ(table.get(method.getDeclarationId()).owner,
            instance.getDeclarationId());
}
