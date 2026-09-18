#include <gtest/gtest.h>

#include <set>
#include <sstream>

#include "ast.h"
#include "ast/ast_children.h"
#include "codegen/codegen_visitor.h"
#include "driver/driver.h"
#include "parsing/parser.h"
#include "semantic_analysis/callable_signature.h"
#include "semantic_analysis/declaration_identity_pass.h"
#include "semantic_analysis/item_refs.h"
#include "serialization/ast_deserializer.h"
#include "serialization/ast_serializer.h"

using sun::semantic_analysis::CallableSignature;
using sun::semantic_analysis::DeclarationId;
using sun::semantic_analysis::DeclarationIdentityPass;
using sun::semantic_analysis::DeclarationKind;
using sun::semantic_analysis::DeclarationTable;
using sun::semantic_analysis::PortableDeclarationKey;
using sun::semantic_analysis::SpecializationKey;
using sun::semantic_analysis::TypeRegistry;
using sun::semantic_analysis::Types;

using sun::ast::ASTNodeType;
using sun::ast::ClassDefinitionAST;
using sun::ast::ExprAST;
using sun::ast::forEachChild;
using sun::ast::FunctionAST;
using sun::ast::LambdaAST;
using sun::ast::ModuleAST;
using sun::driver::Driver;
using sun::semantic_analysis::SemanticAnalyzer;
using sun::semantic_analysis::SemanticContext;

namespace {

/** Parse syntax without resolving any declaration signatures. */
std::unique_ptr<sun::ast::BlockExprAST> parse(const std::string& source) {
  std::istringstream input(source);
  sun::parsing::Parser parser(input);
  return parser.parseString(source);
}

}  // namespace

TEST(Tooling_Frontend_DeclarationIdentity,
     nested_types_exist_before_signatures) {
  auto ast = parse(R"(
    function work(x: i32) void { class Local {} }
    function work(x: bool) void { class Local {} }
  )");
  DeclarationTable table;
  DeclarationIdentityPass pass(table);
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
  DeclarationTable table;
  DeclarationIdentityPass(table).run(*ast);
  auto& function = static_cast<FunctionAST&>(*ast->getBody()[0]);
  const auto id = function.getDeclarationId();
  const auto parameter = function.declarationIdentity().parameters[0];
  function.setResolvedType(Types::Int32());
  function.setTargetDeclarationId(id);
  function.getProtoMut().setQualifiedName({{}, "computed"});
  sun::semantic_analysis::clearComputedAnalysis(*ast);
  EXPECT_EQ(function.getDeclarationId(), id);
  EXPECT_EQ(function.declarationIdentity().parameters[0], parameter);
  EXPECT_FALSE(function.hasResolvedType());
  EXPECT_FALSE(function.getTargetDeclarationId());
  EXPECT_FALSE(function.getProto().hasQualifiedName());
  sun::semantic_analysis::resetAnalysisSession(*ast);
  EXPECT_FALSE(function.getDeclarationId());
  EXPECT_FALSE(function.getProto().hasAnalysis());
  DeclarationTable next;
  DeclarationIdentityPass(next).run(*ast);
  EXPECT_TRUE(function.getDeclarationId());
  EXPECT_EQ(next.size(), table.size());
}

TEST(Tooling_Frontend_DeclarationIdentity, cloning_does_not_copy_session_ids) {
  auto ast = parse("function f<T>(x: T) T { class Local {} return x; }");
  DeclarationTable table;
  DeclarationIdentityPass pass(table);
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
  DeclarationTable table;
  DeclarationIdentityPass(table).run(*ast);
  const auto& module = static_cast<const ModuleAST&>(*ast->getBody()[0]);
  const auto& cls =
      static_cast<const ClassDefinitionAST&>(*module.getBody().getBody()[0]);
  const auto& field = cls.getFields()[0];
  EXPECT_EQ(table.get(field.declaration.id).owner, cls.getDeclarationId());
  EXPECT_EQ(table.get(field.declaration.id).module, module.getDeclarationId());
  const auto& method = *cls.getMethods()[0].function;
  EXPECT_EQ(table.get(method.getDeclarationId()).owner, cls.getDeclarationId());
  auto fieldId = field.declaration.id;
  sun::semantic_analysis::clearComputedAnalysis(*ast);
  EXPECT_EQ(field.declaration.id, fieldId);
  sun::semantic_analysis::resetAnalysisSession(*ast);
  EXPECT_FALSE(field.declaration.id);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     reused_tree_requires_a_session_reset) {
  auto ast = parse("function f() void {}");
  DeclarationTable first;
  DeclarationTable second;
  DeclarationIdentityPass(first).run(*ast);
  second.add(DeclarationKind::Function, "unrelated");
  EXPECT_ANY_THROW(DeclarationIdentityPass(second).run(*ast));
  sun::semantic_analysis::resetAnalysisSession(*ast);
  EXPECT_NO_THROW(DeclarationIdentityPass(second).run(*ast));
}

TEST(Tooling_Frontend_DeclarationIdentity, reopened_modules_share_identity) {
  auto ast = parse(
      "module m { function a() void {} } module m { function b() void {} }");
  DeclarationTable table;
  DeclarationIdentityPass(table).run(*ast);
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
      const auto& reference =
          static_cast<const sun::ast::VariableReferenceAST&>(node);
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
  EXPECT_ANY_THROW(DeclarationIdentityPass(second.typeRegistry->declarations)
                       .run(*first.ast));
}

TEST(Tooling_Frontend_DeclarationIdentity, nominal_types_precede_names) {
  auto ast = parse(R"(
    function work(x: i32) void { class Local {} }
    function work(x: bool) void { class Local {} }
  )");
  TypeRegistry types;
  DeclarationIdentityPass(types.declarations).run(*ast);
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
  TypeRegistry first;
  TypeRegistry second;
  auto a = first.declarations.add(DeclarationKind::Interface, "Local");
  auto b = second.declarations.add(DeclarationKind::Interface, "Local");
  ASSERT_EQ(a, b);
  EXPECT_FALSE(first.getInterface(a)->equals(*second.getInterface(b)));
  auto e = first.declarations.add(DeclarationKind::Enum, "Value");
  auto f = first.declarations.add(DeclarationKind::Enum, "Value");
  EXPECT_FALSE(first.getEnum(e)->equals(*first.getEnum(f)));
  EXPECT_ANY_THROW(first.getClass(a));
  EXPECT_ANY_THROW(first.getEnum(DeclarationId{}));
}

TEST(Tooling_Frontend_DeclarationIdentity,
     generated_function_preparation_belongs_to_pipeline) {
  auto ast = parse("function work(x: i32) i32 { return x; }");
  auto types = std::make_shared<TypeRegistry>();
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
  TypeRegistry types;
  auto templateId = types.declarations.add(DeclarationKind::Class, "Box");
  auto otherTemplate = types.declarations.add(DeclarationKind::Class, "Box");
  auto first =
      types.getClass(types.declarations.add(DeclarationKind::Class, "Local"));
  auto second =
      types.getClass(types.declarations.add(DeclarationKind::Class, "Local"));
  SpecializationKey key{templateId, {}, {first}, std::nullopt};
  auto instance = types.specialize(key);
  EXPECT_EQ(types.specialize(key), instance);
  EXPECT_NE(types.specialize({templateId, {}, {second}, std::nullopt}),
            instance);
  EXPECT_NE(types.specialize({otherTemplate, {}, {first}, std::nullopt}),
            instance);
  ASSERT_TRUE(types.declarations.get(instance).specialization);
  EXPECT_EQ(types.declarations.get(instance).specialization->source,
            templateId);
  first->addField("recursive", Types::Reference(first));
  EXPECT_EQ(types.specialize(key), instance);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     specialization_keys_compare_structure_and_variadic_packs) {
  TypeRegistry types;
  auto source = types.declarations.add(DeclarationKind::Function, "work");
  auto owner = types.declarations.add(DeclarationKind::Class, "Owner");
  SpecializationKey key{
      source, {}, {Types::Reference(Types::Int32())}, std::nullopt};
  auto instance = types.specialize(key);
  SpecializationKey equal{
      source, {}, {Types::Reference(Types::Int32())}, std::nullopt};
  EXPECT_EQ(types.specialize(equal), instance);
  EXPECT_EQ(sun::semantic_analysis::SpecializationKeyHash{}(key),
            sun::semantic_analysis::SpecializationKeyHash{}(equal));
  equal.arguments = {Types::Reference(Types::Int32(), false)};
  EXPECT_NE(types.specialize(equal), instance);
  equal = key;
  equal.enclosing = owner;
  auto owned = types.specialize(equal);
  EXPECT_NE(owned, instance);
  EXPECT_EQ(types.declarations.get(owned).owner, owner);
  equal = key;
  equal.variadic = std::vector<sun::semantic_analysis::TypePtr>{};
  auto emptyPack = types.specialize(equal);
  EXPECT_NE(emptyPack, instance);
  equal.variadic = std::vector<sun::semantic_analysis::TypePtr>{Types::Int32()};
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
  auto next = std::static_pointer_cast<sun::semantic_analysis::RawPointerType>(
      type->getFields()[0].type);
  EXPECT_EQ(next->getPointeeType(), type);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     interface_conformance_uses_session_ids) {
  TypeRegistry types;
  auto first = types.getInterface(
      types.declarations.add(DeclarationKind::Interface, "Readable"));
  auto second = types.getInterface(
      types.declarations.add(DeclarationKind::Interface, "Readable"));
  auto implementation =
      types.getClass(types.declarations.add(DeclarationKind::Class, "Value"));
  implementation->addImplementedInterface(*first);
  EXPECT_TRUE(implementation->implementsInterface(*first));
  EXPECT_FALSE(implementation->implementsInterface(*second));
  first->setQualifiedName({{"renamed"}, "Readable"});
  EXPECT_TRUE(implementation->convertibleToInterface(*first));
  implementation->markStaticOnlyInterface(*first);
  EXPECT_FALSE(implementation->convertibleToInterface(*first));
  EXPECT_TRUE(implementation->implementsInterface(*first));
  TypeRegistry other;
  auto foreign = other.getInterface(
      other.declarations.add(DeclarationKind::Interface, "Readable"));
  ASSERT_EQ(first->getDeclarationId(), foreign->getDeclarationId());
  EXPECT_FALSE(implementation->implementsInterface(*foreign));
  EXPECT_ANY_THROW(implementation->addImplementedInterface(*foreign));
}

TEST(Tooling_Frontend_DeclarationIdentity,
     abstract_arguments_use_binder_identity) {
  TypeRegistry types;
  sun::ast::TypeParameter parameter("T");
  auto a = types.declarations.add(DeclarationKind::TypeParameter, "T");
  auto b = types.declarations.add(DeclarationKind::TypeParameter, "T");
  auto first = parameter.toSunType(types.declarations, a);
  auto repeated = parameter.toSunType(types.declarations, a);
  auto second = parameter.toSunType(types.declarations, b);
  EXPECT_TRUE(first->equals(*repeated));
  EXPECT_FALSE(first->equals(*second));
  auto source = types.declarations.add(DeclarationKind::Class, "Box");
  auto instance = types.specialize({source, {}, {first}, std::nullopt});
  EXPECT_EQ(instance, types.specialize({source, {}, {repeated}, std::nullopt}));
  EXPECT_NE(instance, types.specialize({source, {}, {second}, std::nullopt}));
  auto projection =
      static_cast<const sun::semantic_analysis::TypeParameterType&>(*first)
          .project(sun::semantic_analysis::TypeProjection::ReturnType);
  auto repeatedProjection =
      static_cast<const sun::semantic_analysis::TypeParameterType&>(*repeated)
          .project(sun::semantic_analysis::TypeProjection::ReturnType);
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
  DeclarationTable table;
  DeclarationIdentityPass pass(table);
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
  auto types = std::make_shared<TypeRegistry>();
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
  auto unknown =
      types->getClass(types->declarations.add(DeclarationKind::Class, "Box"));
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
  auto types = std::make_shared<TypeRegistry>();
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
  const auto& plain = static_cast<const sun::ast::EnumDefinitionAST&>(
      *result.ast->getBody()[0]);
  auto plainType = result.typeRegistry->getEnum(plain.getDeclarationId());
  for (size_t i = 0; i < plain.getVariants().size(); ++i)
    EXPECT_EQ(plainType->getVariants()[i].declarationId,
              plain.getVariants()[i].declaration.id);
  const auto& generic = static_cast<const sun::ast::EnumDefinitionAST&>(
      *result.ast->getBody()[1]);
  ASSERT_EQ(generic.getSpecializations().size(), 2u);
  std::set<DeclarationId> variants;
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
  DeclarationTable table;
  DeclarationIdentityPass pass(table);
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

TEST(Tooling_Frontend_DeclarationIdentity, captures_follow_analysis_lifetime) {
  auto driver = Driver::createForJIT();
  const std::string source = R"(
    function main() i32 {
      var value: i32 = 7;
      var read = [const ref value]() => i32 { return value; };
      return read();
    }
  )";
  auto first = driver->analyzeString(source);
  ASSERT_FALSE(first.error);
  const LambdaAST* lambda = nullptr;
  std::function<void(const ExprAST&)> visit = [&](const ExprAST& node) {
    if (node.getType() == ASTNodeType::LAMBDA)
      lambda = &static_cast<const LambdaAST&>(node);
    forEachChild(node, visit);
  };
  visit(*first.ast);
  ASSERT_NE(lambda, nullptr);
  const auto& proto = lambda->getProto();
  ASSERT_EQ(proto.getCaptures().size(), 1u);
  const auto capture = proto.getCaptures()[0];
  const auto id = lambda->getDeclarationId();
  EXPECT_EQ(first.typeRegistry->declarations.get(capture.declarationId).name,
            "value");
  EXPECT_TRUE(capture.type->equals(*Types::Int32()));

  auto clone = lambda->clone();
  const auto& cloned = static_cast<const LambdaAST&>(*clone).getProto();
  EXPECT_FALSE(cloned.hasClosure());
  EXPECT_FALSE(cloned.hasAnalysis());
  EXPECT_EQ(cloned.getRefCaptureNames(), proto.getRefCaptureNames());
  EXPECT_EQ(cloned.getConstRefCaptureNames(), proto.getConstRefCaptureNames());

  auto second = driver->analyzeString(source);
  ASSERT_FALSE(second.error);
  driver.reset();
  EXPECT_EQ(proto.getCaptures()[0].declarationId, capture.declarationId);
  EXPECT_EQ(first.typeRegistry->declarations.get(capture.declarationId).name,
            "value");
  EXPECT_FALSE(proto.declarationIdentity().session.expired());
  EXPECT_NE(first.typeRegistry, second.typeRegistry);

  sun::semantic_analysis::clearComputedAnalysis(*first.ast);
  EXPECT_EQ(lambda->getDeclarationId(), id);
  EXPECT_FALSE(proto.hasClosure());
  EXPECT_FALSE(proto.hasRefCaptures());
  EXPECT_EQ(proto.getConstRefCaptureNames(), std::vector<std::string>{"value"});

  visit(*second.ast);
  ASSERT_EQ(lambda->getProto().getCaptures().size(), 1u);
  sun::semantic_analysis::resetAnalysisSession(*second.ast);
  EXPECT_FALSE(lambda->getDeclarationId());
  EXPECT_FALSE(lambda->getProto().hasClosure());
  EXPECT_FALSE(lambda->getProto().hasAnalysis());
  EXPECT_EQ(lambda->getProto().getConstRefCaptureNames(),
            std::vector<std::string>{"value"});
}

TEST(Tooling_Frontend_DeclarationIdentity,
     vtables_distinguish_ids_and_sessions) {
  auto types = std::make_shared<TypeRegistry>();
  auto classId = types->declarations.add(DeclarationKind::Class, "Box");
  auto firstId = types->declarations.add(DeclarationKind::Interface, "View");
  auto secondId = types->declarations.add(DeclarationKind::Interface, "View");
  auto cls = types->getClass(classId, {{}, "Box"});
  auto first = types->getInterface(firstId, {{}, "View"});
  auto second = types->getInterface(secondId, {{}, "View"});
  uint64_t ordinal = 1;
  for (auto id : {classId, firstId, secondId})
    types->declarations.bindPortable(
        id, PortableDeclarationKey::original(std::string(64, 'a'), ordinal++));
  sun::codegen::CodegenContext context("vtable_identity", nullptr);
  sun::codegen::CodegenVisitor gen(context, types);
  auto& classes = gen.classGenerator();
  auto* firstTable = classes.getOrCreateInterfaceVtable(cls.get(), first.get());
  auto* secondTable =
      classes.getOrCreateInterfaceVtable(cls.get(), second.get());
  EXPECT_NE(firstTable, secondTable);
  EXPECT_EQ(firstTable,
            classes.getOrCreateInterfaceVtable(cls.get(), first.get()));

  TypeRegistry foreign;
  auto foreignClassId = foreign.declarations.add(DeclarationKind::Class, "Box");
  auto foreignInterfaceId =
      foreign.declarations.add(DeclarationKind::Interface, "View");
  auto foreignClass = foreign.getClass(foreignClassId, {{}, "Box"});
  auto foreignInterface =
      foreign.getInterface(foreignInterfaceId, {{}, "View"});
  ASSERT_EQ(foreignClassId, classId);
  ASSERT_EQ(foreignInterfaceId, firstId);
  EXPECT_ANY_THROW(
      classes.getOrCreateInterfaceVtable(foreignClass.get(), first.get()));
  EXPECT_ANY_THROW(
      classes.getOrCreateInterfaceVtable(cls.get(), foreignInterface.get()));
}

TEST(Tooling_Frontend_DeclarationIdentity,
     resolved_enum_and_interface_templates_survive_closed_scopes) {
  auto ast = parse(R"(
    function define() void {
      enum Choice<T> { Some(T), None }
      interface View<T> { method get() T; }
    }
  )");
  auto types = std::make_shared<TypeRegistry>();
  SemanticAnalyzer analyzer(types);
  analyzer.pipeline().run(*ast);
  const auto& function = static_cast<const FunctionAST&>(*ast->getBody()[0]);
  const auto& enumeration = *function.getBody().getBody()[0];
  const auto& interface = *function.getBody().getBody()[1];
  auto* enumInfo =
      analyzer.context().lookupGenericEnum(enumeration.getDeclarationId());
  auto* interfaceInfo =
      analyzer.context().lookupGenericInterface(interface.getDeclarationId());
  ASSERT_NE(enumInfo, nullptr);
  ASSERT_NE(interfaceInfo, nullptr);
  auto binder = types->declarations.add(DeclarationKind::TypeParameter, "U");
  auto argument =
      Types::TypeParameter("U", {}, binder, types->declarations.session());
  auto abstractEnum =
      analyzer.generics().instantiateGenericEnum(*enumInfo, {argument});
  auto abstractInterface = analyzer.generics().instantiateGenericInterface(
      *interfaceInfo, {argument});
  abstractEnum->setGenericQualifiedName({{}, "unrelated"});
  abstractInterface->setGenericQualifiedName({{}, "unrelated"});
  analyzer.context().enterTypeParamScope({"U"}, {Types::Int32()});
  auto concreteEnum = analyzer.types().substituteTypeParameters(abstractEnum);
  auto concreteInterface =
      analyzer.types().substituteTypeParameters(abstractInterface);
  EXPECT_EQ(concreteEnum, analyzer.generics().instantiateGenericEnum(
                              *enumInfo, {Types::Int32()}));
  EXPECT_EQ(concreteInterface, analyzer.generics().instantiateGenericInterface(
                                   *interfaceInfo, {Types::Int32()}));
  analyzer.context().exitScope();

  auto borrowed = analyzer.generics().instantiateGenericEnum(
      *enumInfo, {Types::Reference(Types::Int32())});
  borrowed->setGenericQualifiedName({{}, "unrelated"});
  EXPECT_EQ(analyzer.types().createConstView(borrowed),
            analyzer.generics().instantiateGenericEnum(
                *enumInfo, {Types::Reference(Types::Int32(), false)}));
  auto variant = parse("Choice.None;");
  {
    sun::semantic_analysis::SemanticContext::ScopeSwitchGuard scope(
        analyzer.context(), SemanticContext::definitionScopeOf(*enumInfo));
    EXPECT_TRUE(analyzer.enums().tryAnalyzeGenericEnumUnitVariant(
        static_cast<sun::ast::MemberAccessAST&>(*variant->getBody()[0]),
        borrowed));
    EXPECT_EQ(variant->getBody()[0]->getResolvedType(), borrowed);
  }
  TypeRegistry foreign;
  EXPECT_ANY_THROW(borrowed->sourceDeclaration(foreign.declarations));
  EXPECT_ANY_THROW(abstractInterface->sourceDeclaration(foreign.declarations));
}

TEST(Tooling_Frontend_DeclarationIdentity,
     enum_patterns_do_not_match_shadowed_template_names) {
  auto driver = Driver::createForJIT();
  auto result = driver->analyzeString(R"(
    enum Choice<T> { Some(T), None }
    function main() i32 {
      var item = Choice.Some(7);
      if (true) {
        enum Choice<T> { Some(T), None }
        return match item {
          Choice.Some(value) => value,
          Choice.None => 0
        };
      }
      return 0;
    }
  )");
  ASSERT_TRUE(result.error);
  EXPECT_NE(std::string(result.error->what()).find("Pattern does not match"),
            std::string::npos);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     portable_source_ordinals_ignore_session_allocations) {
  const std::string source = R"(
    public module lib {
      public function work<T>(value: T) i32 {
        class Local { var n: i32; }
        return 1;
      }
    }
  )";
  auto first = parse(source), second = parse(source);
  DeclarationTable a, b;
  b.add(DeclarationKind::Variable, "unrelated");
  DeclarationIdentityPass(a).run(*first);
  DeclarationIdentityPass(b).run(*second);
  PortableDeclarationKey::assignOriginals(*first, a, std::string(64, 'a'));
  PortableDeclarationKey::assignOriginals(*second, b, std::string(64, 'a'));
  for (uint64_t i = 1; i <= a.size(); ++i) {
    const auto key =
        PortableDeclarationKey::fromDeclaration(DeclarationId(i), a);
    const auto imported = b.findPortable(key);
    ASSERT_TRUE(imported);
    EXPECT_NE(imported, DeclarationId(i));
    EXPECT_EQ(a.get(DeclarationId(i)).name, b.get(imported).name);
  }
}

TEST(Tooling_Frontend_DeclarationIdentity,
     imported_declarations_survive_session_reset_but_not_cloning) {
  auto source = parse("function work<T>(value: T) i32 { return 1; }");
  DeclarationTable original;
  DeclarationIdentityPass(original).run(*source);
  PortableDeclarationKey::assignOriginals(*source, original,
                                          std::string(64, 'a'));
  sun::serialization::ASTSerializer serializer({.declarations = &original});
  sun::serialization::ASTDeserializer deserializer(
      {.import_declarations = true});
  auto imported = deserializer.deserialize(serializer.serialize(*source));
  std::vector<sun::semantic_analysis::ImportedDeclarationRecord> records;
  auto key = [&](DeclarationId id) {
    return id ? PortableDeclarationKey::fromDeclaration(id, original).encoding()
              : std::string{};
  };
  for (uint64_t i = 1; i <= original.size(); ++i) {
    const auto& record = original.get(DeclarationId(i));
    records.push_back({key(DeclarationId(i)),
                       static_cast<uint32_t>(record.kind), record.name,
                       key(record.owner), key(record.module)});
  }
  DeclarationTable first;
  first.importRecords(records);
  DeclarationIdentityPass(first).run(*imported);
  auto& function =
      *static_cast<sun::ast::BlockExprAST&>(*imported).getBody()[0];
  const auto portable = PortableDeclarationKey::fromDeclaration(
      function.getDeclarationId(), first);
  const auto oldId = function.getDeclarationId();
  sun::semantic_analysis::resetAnalysisSession(*imported);
  ASSERT_FALSE(function.getDeclarationId());
  ASSERT_TRUE(function.declarationIdentity().imported);
  DeclarationTable second;
  second.add(DeclarationKind::Variable, "unrelated");
  second.importRecords(records);
  DeclarationIdentityPass(second).run(*imported);
  EXPECT_NE(function.getDeclarationId(), oldId);
  EXPECT_EQ(PortableDeclarationKey::fromDeclaration(function.getDeclarationId(),
                                                    second),
            portable);
  auto clone = function.clone();
  EXPECT_FALSE(clone->getDeclarationId());
  EXPECT_FALSE(clone->declarationIdentity().imported);
}

TEST(Tooling_Frontend_DeclarationIdentity,
     visibility_follows_module_ids_instead_of_qualified_names) {
  TypeRegistry types;
  auto& table = types.declarations;
  auto library = table.module("library");
  auto child = table.module("nested", library);
  auto unrelated = table.module("unrelated");
  auto source = table.add(DeclarationKind::Class, "Box", library, library);
  auto type = types.getClass(source, {{"unrelated", "SomeClass"}, "Renamed"});
  auto& field = type->addField("hidden", Types::Int32());
  field.visibility = sun::semantic_analysis::Visibility::Private;
  const auto item = sun::semantic_analysis::fieldRef(*type, field);
  EXPECT_TRUE(sun::semantic_analysis::isAccessible(library, item, table));
  EXPECT_TRUE(sun::semantic_analysis::isAccessible(child, item, table));
  EXPECT_FALSE(sun::semantic_analysis::isAccessible(unrelated, item, table));
  EXPECT_FALSE(sun::semantic_analysis::isAccessible({}, item, table));
  EXPECT_NE(sun::semantic_analysis::denialMessage(item, table)
                .find("module 'library'"),
            std::string::npos);

  auto instance =
      types.specialize({source, {}, {Types::Int32()}, std::nullopt});
  auto specialized = types.getClass(instance, {{"alias"}, "Box_i32"});
  auto& specializedField = specialized->addField("hidden", Types::Int32());
  specializedField.visibility = sun::semantic_analysis::Visibility::Private;
  const auto specializedItem =
      sun::semantic_analysis::fieldRef(*specialized, specializedField);
  EXPECT_EQ(table.get(instance).module, library);
  EXPECT_TRUE(
      sun::semantic_analysis::isAccessible(child, specializedItem, table));
  EXPECT_FALSE(
      sun::semantic_analysis::isAccessible(unrelated, specializedItem, table));
}

TEST(Tooling_Frontend_DeclarationIdentity,
     overload_keys_use_exact_type_identity) {
  TypeRegistry types;
  auto firstId = types.declarations.add(DeclarationKind::Class, "Same");
  auto secondId = types.declarations.add(DeclarationKind::Class, "Same");
  auto first = types.getClass(firstId, {{}, "Same"});
  auto second = types.getClass(secondId, {{}, "Same"});
  std::unordered_map<CallableSignature, int,
                     sun::semantic_analysis::CallableSignatureHash>
      keys;
  keys.emplace(CallableSignature{"accept", {first}}, 1);
  keys.emplace(CallableSignature{"accept", {second}}, 2);
  first->setQualifiedName({{"renamed"}, "Display"});
  EXPECT_EQ(keys.size(), 2u);
  EXPECT_EQ(keys.at(CallableSignature{"accept", {first}}), 1);
  EXPECT_EQ(keys.at(CallableSignature{"accept", {second}}), 2);
  EXPECT_FALSE(
      (CallableSignature{"accept", {Types::RawPointer(Types::Int32())}} ==
       CallableSignature{"accept", {Types::RawPointer(Types::Void())}}));
}
