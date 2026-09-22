/** Checks import preparation and source ordering in the semantic pipeline. */
#include <gtest/gtest.h>

#include "ast/class_definition_ast.h"
#include "ast/moon_scope_ast.h"
#include "ast/number_expr_ast.h"
#include "semantic_analysis/passes/declaration_identity_pass.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

/** Keeps in-memory bundle builders local to these tests. */
namespace {
using namespace sun::ast;
using namespace sun::semantic_analysis;

/** Creates a portable identity for a declaration in a synthetic bundle. */
PortableDeclarationKey key(char bundle, uint64_t ordinal = 2) {
  return PortableDeclarationKey::original(std::string(64, bundle), ordinal);
}

/** Builds a compiled class stub with a primitive or generic field. */
std::unique_ptr<MoonScopeAST> bundle(
    char artifact, bool generic = false,
    std::optional<PortableDeclarationKey> fieldKey = std::nullopt) {
  const std::string scope = "$" + std::string(64, artifact) + "$";
  std::vector<TypeParameter> parameters;
  if (generic) parameters.emplace_back("T");
  std::vector<ClassFieldDecl> fields;
  TypeAnnotation fieldType(generic ? "T" : "i32");
  if (fieldKey) {
    fieldType.baseName = "Value";
    fieldType.declarationKey = fieldKey;
  }
  fields.push_back({"value", std::move(fieldType)});
  auto type = std::make_unique<ClassDefinitionAST>(
      "Value", std::move(parameters), std::vector<ImplementedInterfaceAST>{},
      std::move(fields), std::vector<ClassMethodDecl>{}, true);
  type->declarationIdentity().imported =
      ImportedDeclarationIdentity{key(artifact).encoding()};
  type->getFields()[0].declaration.imported =
      ImportedDeclarationIdentity{key(artifact, 3).encoding()};
  if (generic)
    type->declarationIdentity().imported->typeParameters.push_back(
        key(artifact, 4).encoding());
  std::vector<std::unique_ptr<ExprAST>> declarations;
  declarations.push_back(std::move(type));
  auto moon = std::make_unique<MoonScopeAST>(
      scope, "", std::nullopt, std::string(1, artifact) + ".moon",
      std::make_unique<BlockExprAST>(std::move(declarations),
                                     BlockKind::Module));
  const auto root = key(artifact, 1).encoding();
  const auto owner = key(artifact).encoding();
  moon->importedDeclarations = {
      {root, static_cast<uint32_t>(DeclarationKind::Module), scope, "", ""},
      {owner, static_cast<uint32_t>(DeclarationKind::Class), "Value", root,
       root},
      {key(artifact, 3).encoding(),
       static_cast<uint32_t>(DeclarationKind::Field), "value", owner, root}};
  if (generic)
    moon->importedDeclarations.push_back(
        {key(artifact, 4).encoding(),
         static_cast<uint32_t>(DeclarationKind::TypeParameter), "T", owner,
         root});
  return moon;
}

/** Builds source syntax whose field default requires a generated constructor.
 */
std::unique_ptr<ClassDefinitionAST> classWithDefault(const std::string& name) {
  std::vector<ClassFieldDecl> fields;
  fields.push_back({"value", TypeAnnotation("i32")});
  fields.back().initializer = std::make_unique<NumberExprAST>(int64_t{1});
  return std::make_unique<ClassDefinitionAST>(
      name, std::vector<TypeParameter>{},
      std::vector<ImplementedInterfaceAST>{}, std::move(fields),
      std::vector<ClassMethodDecl>{});
}

/** Owns a complete AST and a fresh analysis session without a parser. */
class TypeAnalysis_SemanticPipeline : public ::testing::Test {
 protected:
  std::shared_ptr<AnalysisResults> results =
      std::make_shared<AnalysisResults>();
  SemanticAnalyzer analyzer{results};
  SemanticContext& context = analyzer.context();
  BlockExprAST tree;

  /** Adds one already loaded bundle to the complete AST. */
  MoonScopeAST& add(std::unique_ptr<MoonScopeAST> moon) {
    auto& result = *moon;
    std::vector<std::unique_ptr<ExprAST>> nodes;
    nodes.push_back(std::move(moon));
    tree.prependExpressions(std::move(nodes));
    return result;
  }

  /** Checks that preparation rejects a requirement with its original context.
   */
  void expectDependencyError(const std::string& detail = "") {
    try {
      analyzer.pipeline().run(tree);
      FAIL() << "Expected an exact dependency error";
    } catch (const sun::support::SunError& error) {
      const std::string message = error.what();
      EXPECT_NE(message.find("moon exact dependency: library 'a.moon'"),
                std::string::npos);
      EXPECT_NE(message.find("requires declaration 'Value' from bundle " +
                             std::string(64, 'b')),
                std::string::npos);
      EXPECT_NE(message.find("Explicitly import the required exact bundle."),
                std::string::npos);
      if (!detail.empty()) EXPECT_NE(message.find(detail), std::string::npos);
    }
  }
};
}  // namespace

/** Registration across all bundles precedes validation in either AST order. */
TEST_F(TypeAnalysis_SemanticPipeline, PreparesFieldsInEitherDependencyOrder) {
  for (bool dependencyFirst : {false, true}) {
    auto importer = bundle('a', false, key('b'));
    importer->requiredDeclarations.push_back({key('b'), "Value", std::nullopt});
    std::vector<std::unique_ptr<ExprAST>> nodes;
    if (dependencyFirst) nodes.push_back(bundle('b'));
    nodes.push_back(std::move(importer));
    if (!dependencyFirst) nodes.push_back(bundle('b'));
    BlockExprAST ordered(std::move(nodes), BlockKind::Module);
    auto session = std::make_shared<AnalysisResults>();
    SemanticAnalyzer isolatedAnalyzer(session);
    auto& isolated = isolatedAnalyzer.context();
    ASSERT_NO_THROW(isolatedAnalyzer.pipeline().run(ordered));
    EXPECT_TRUE(session->declarations.findPortable(key('b')));
    EXPECT_EQ(isolated.rootScope().childModules.size(), 2u);
    for (const auto& node : ordered.getBody()) {
      const auto& moon = static_cast<const MoonScopeAST&>(*node);
      const auto& type = *moon.getBody().getBody()[0];
      EXPECT_TRUE(type.getDeclarationId());
      ASSERT_TRUE(type.hasResolvedType());
      auto prepared = session->types->getClass(type.getDeclarationId());
      ASSERT_NE(prepared->getField("value"), nullptr);
    }
    auto importerType =
        session->types->getClass(session->declarations.findPortable(key('a')));
    EXPECT_EQ(
        importerType->getField("value")->type,
        session->types->getClass(session->declarations.findPortable(key('b'))));
  }
}

/** Missing exact declarations are rejected even when source never uses them. */
TEST_F(TypeAnalysis_SemanticPipeline, MissingDependency) {
  add(bundle('a'))
      .requiredDeclarations.push_back({key('b'), "Value", std::nullopt});
  expectDependencyError();
}

/** A declaration with the same name from another bundle cannot satisfy a key.
 */
TEST_F(TypeAnalysis_SemanticPipeline, ConflictingVersion) {
  add(bundle('a'))
      .requiredDeclarations.push_back({key('b'), "Value", std::nullopt});
  add(bundle('c'));
  expectDependencyError("conflicting bundle supplied");
}

/** Interface requirements reject an exact key that identifies a class. */
TEST_F(TypeAnalysis_SemanticPipeline, WrongTypeKind) {
  add(bundle('a'))
      .requiredDeclarations.push_back(
          {key('b'), "Value", sun::types::Type::Kind::Interface});
  add(bundle('b'));
  expectDependencyError("declaration has the wrong type kind");
}

/** Generic metadata requirements are checked without instantiating templates.
 */
TEST_F(TypeAnalysis_SemanticPipeline, UnusedGenericRequirement) {
  auto& moon = add(bundle('a', true));
  moon.requiredDeclarations.push_back(
      {key('b'), "Value", sun::types::Type::Kind::Interface});
  expectDependencyError();
  EXPECT_EQ(context.lookupGenericClass("Value"), nullptr);
  EXPECT_FALSE(moon.getBody().getBody()[0]->getDeclarationId());
}

/** Own-bundle metadata is ignored while its source declarations are analyzed.
 */
TEST_F(TypeAnalysis_SemanticPipeline, TreatsOwnBundleAsSource) {
  auto source = bundle('a')->getBody().getBody()[0]->clone();
  source->setPrecompiled(false);
  auto* sourceType = source.get();
  std::vector<std::unique_ptr<ExprAST>> nodes;
  nodes.push_back(std::move(source));
  tree.prependExpressions(std::move(nodes));

  auto ownSource = bundle('b')->getBody().getBody()[0]->clone();
  ownSource->setPrecompiled(false);
  auto* ownType = ownSource.get();
  std::vector<std::unique_ptr<ExprAST>> ownNodes;
  ownNodes.push_back(std::move(ownSource));
  auto own = MoonScopeAST::forOwnBundle(
      "$own$",
      std::make_unique<BlockExprAST>(std::move(ownNodes), BlockKind::Module));
  own->importedDeclarations.push_back({"invalid", 0, "", "", ""});
  own->requiredDeclarations.push_back({key('b'), "Value", std::nullopt});
  add(std::move(own));

  ASSERT_NO_THROW(analyzer.pipeline().run(tree));
  EXPECT_TRUE(sourceType->hasResolvedType());
  EXPECT_TRUE(ownType->hasResolvedType());
  EXPECT_FALSE(results->declarations.findPortable(key('b')));
}

/** Import preparation attaches template identities without specializing them.
 */
TEST_F(TypeAnalysis_SemanticPipeline,
       RegistersGenericTemplatesForLaterSpecialization) {
  auto& moon = add(bundle('a', true));
  ASSERT_NO_THROW(analyzer.pipeline().run(tree));
  const auto expected = results->declarations.findPortable(key('a'));
  const auto count = results->declarations.size();
  const auto& type =
      static_cast<const ClassDefinitionAST&>(*moon.getBody().getBody()[0]);
  EXPECT_EQ(type.getDeclarationId(), expected);
  EXPECT_EQ(type.getFields()[0].declaration.id,
            results->declarations.findPortable(key('a', 3)));
  ASSERT_EQ(type.declarationIdentity().typeParameters.size(), 1u);
  EXPECT_EQ(type.declarationIdentity().typeParameters[0],
            results->declarations.findPortable(key('a', 4)));
  EXPECT_EQ(results->declarations.size(), count);
  EXPECT_TRUE(type.hasResolvedType());
  const auto* generic = context.lookupGenericClass(expected);
  ASSERT_NE(generic, nullptr);
  auto specialized = analyzer.generics().instantiateGenericClass(
      *generic, {sun::types::Types::Int32()});
  ASSERT_NE(specialized->getField("value"), nullptr);
  EXPECT_EQ(specialized->getField("value")->type, sun::types::Types::Int32());
  ASSERT_NO_THROW(analyzer.generics().analyzePendingBodies());
  EXPECT_FALSE(analyzer.generics().hasPendingBodies());
}

/** Imported function signatures and interface fields are ready on return. */
TEST_F(TypeAnalysis_SemanticPipeline, CompletesSignaturesAndInterfaces) {
  auto moon = bundle('a');
  auto proto = std::make_unique<PrototypeAST>(
      "read", std::vector<std::pair<std::string, TypeAnnotation>>{},
      TypeAnnotation("i32"));
  auto function = std::make_unique<FunctionAST>(std::move(proto), nullptr);
  function->setPrecompiled(true);
  function->declarationIdentity().imported =
      ImportedDeclarationIdentity{key('a', 10).encoding()};
  auto* importedFunction = function.get();
  std::vector<InterfaceFieldDecl> fields;
  fields.push_back({"size", TypeAnnotation("i32")});
  auto interface = std::make_unique<InterfaceDefinitionAST>(
      "Sized", std::vector<TypeParameter>{}, std::move(fields),
      std::vector<InterfaceMethodDecl>{}, true);
  interface->declarationIdentity().imported =
      ImportedDeclarationIdentity{key('a', 11).encoding()};
  interface->getFields()[0].declaration.imported =
      ImportedDeclarationIdentity{key('a', 12).encoding()};
  const auto root = key('a', 1).encoding();
  moon->importedDeclarations.push_back(
      {key('a', 10).encoding(),
       static_cast<uint32_t>(DeclarationKind::Function), "read", root, root});
  moon->importedDeclarations.push_back(
      {key('a', 11).encoding(),
       static_cast<uint32_t>(DeclarationKind::Interface), "Sized", root, root});
  moon->importedDeclarations.push_back(
      {key('a', 12).encoding(), static_cast<uint32_t>(DeclarationKind::Field),
       "size", key('a', 11).encoding(), root});
  std::vector<std::unique_ptr<ExprAST>> declarations;
  declarations.push_back(std::move(interface));
  declarations.push_back(std::move(function));
  const_cast<BlockExprAST&>(moon->getBody())
      .prependExpressions(std::move(declarations));
  add(std::move(moon));
  ASSERT_NO_THROW(analyzer.pipeline().run(tree));
  EXPECT_EQ(importedFunction->getProto().getResolvedReturnType(),
            sun::types::Types::Int32());
  auto interfaceType = results->types->getInterface(
      results->declarations.findPortable(key('a', 11)));
  ASSERT_NE(interfaceType->getField("size"), nullptr);
  EXPECT_EQ(interfaceType->getField("size")->type, sun::types::Types::Int32());
}

/** Source preparation sees completed imports and never analyzes them twice. */
TEST_F(TypeAnalysis_SemanticPipeline, PipelineSkipsCompletedImports) {
  auto& moon = add(bundle('a'));
  auto source = moon.getBody().getBody()[0]->clone();
  source->setPrecompiled(false);
  auto* sourceType = source.get();
  std::vector<std::unique_ptr<ExprAST>> sourceNodes;
  sourceNodes.push_back(std::move(source));
  tree.prependExpressions(std::move(sourceNodes));
  bool inspected = false;
  ASSERT_NO_THROW(analyzer.pipeline().run(tree, [&] {
    inspected = true;
    EXPECT_FALSE(sourceType->hasResolvedType());
    EXPECT_EQ(context.lookupClass("Value"), nullptr);
    const auto& type = *moon.getBody().getBody()[0];
    ASSERT_TRUE(type.hasResolvedType());
    EXPECT_TRUE(
        results->types->getClass(type.getDeclarationId())->hasField("value"));
  }));
  EXPECT_TRUE(inspected);
  EXPECT_TRUE(sourceType->hasResolvedType());
}

/** Imported globals are registered from their types without an initializer. */
TEST_F(TypeAnalysis_SemanticPipeline, RegistersModuleGlobals) {
  auto moon = bundle('a');
  TypeAnnotation annotation("Value");
  annotation.declarationKey = key('a');
  auto global = std::make_unique<VariableCreationAST>("shared", nullptr,
                                                      std::move(annotation));
  global->setPrecompiled(true);
  global->setVisibility(Visibility::Public);
  global->declarationIdentity().imported =
      ImportedDeclarationIdentity{key('a', 11).encoding()};
  auto* importedGlobal = global.get();
  std::vector<std::unique_ptr<ExprAST>> members;
  members.push_back(std::move(global));
  auto module = std::make_unique<ModuleAST>(
      "storage",
      std::make_unique<BlockExprAST>(std::move(members), BlockKind::Module));
  module->setPrecompiled(true);
  module->setVisibility(Visibility::Public);
  module->declarationIdentity().imported =
      ImportedDeclarationIdentity{key('a', 10).encoding()};
  const auto root = key('a', 1).encoding();
  moon->importedDeclarations.push_back(
      {key('a', 10).encoding(), static_cast<uint32_t>(DeclarationKind::Module),
       "storage", root, root});
  moon->importedDeclarations.push_back(
      {key('a', 11).encoding(),
       static_cast<uint32_t>(DeclarationKind::Variable), "shared",
       key('a', 10).encoding(), key('a', 10).encoding()});
  std::vector<std::unique_ptr<ExprAST>> declarations;
  declarations.push_back(std::move(module));
  const_cast<BlockExprAST&>(moon->getBody())
      .prependExpressions(std::move(declarations));
  add(std::move(moon));
  ASSERT_NO_THROW(analyzer.pipeline().run(tree));
  auto* scope = context.lookupModuleScope(
      results->declarations.findPortable(key('a', 10)));
  ASSERT_NE(scope, nullptr);
  auto* variable = scope->lookupVariable("shared");
  ASSERT_NE(variable, nullptr);
  EXPECT_EQ(variable->type, results->types->getClass(
                                results->declarations.findPortable(key('a'))));
  EXPECT_EQ(variable->declarationId, importedGlobal->getDeclarationId());
  EXPECT_FALSE(importedGlobal->getValue());
}

/** Failed member resolution restores the caller's scope and deferral state. */
TEST_F(TypeAnalysis_SemanticPipeline,
       FailedResolutionRestoresPreparationState) {
  add(bundle('a', false, key('b')));
  auto* originalScope = context.scope();
  EXPECT_THROW(analyzer.pipeline().run(tree), sun::support::SunError);
  EXPECT_EQ(context.scope(), originalScope);
  EXPECT_FALSE(context.isCollectingDeclarations());
}

/** Whole-tree preparation can prune imports without pruning own-bundle source.
 */
TEST_F(TypeAnalysis_SemanticPipeline, PreparationPassesOptionallySkipImports) {
  auto source = classWithDefault("Source");
  auto* sourceClass = source.get();
  std::vector<std::unique_ptr<ExprAST>> sourceNodes;
  sourceNodes.push_back(std::move(source));
  tree.prependExpressions(std::move(sourceNodes));

  auto own = classWithDefault("Own");
  auto* ownClass = own.get();
  std::vector<std::unique_ptr<ExprAST>> ownNodes;
  ownNodes.push_back(std::move(own));
  add(MoonScopeAST::forOwnBundle(
      "$own$",
      std::make_unique<BlockExprAST>(std::move(ownNodes), BlockKind::Module)));

  // Imported templates can retain source syntax within their subtrees.
  auto retained = classWithDefault("Retained");
  auto* retainedClass = retained.get();
  std::vector<std::unique_ptr<ExprAST>> retainedNodes;
  retainedNodes.push_back(std::move(retained));
  auto& imported = add(std::make_unique<MoonScopeAST>(
      "$import$", "", std::nullopt, "import.moon",
      std::make_unique<BlockExprAST>(std::move(retainedNodes),
                                     BlockKind::Module)));

  passes::FieldInitializerPreparationPass fields;
  passes::DeclarationIdentityPass identities(results->declarations);
  fields.run(tree, /*skipImportedMoons=*/true);
  int callbacks = 0;
  identities.run(tree, /*skipImportedMoons=*/true, [&] {
    ++callbacks;
    EXPECT_TRUE(sourceClass->getDeclarationId());
    EXPECT_TRUE(ownClass->getDeclarationId());
    EXPECT_FALSE(imported.getDeclarationId());
  });
  EXPECT_EQ(callbacks, 1);
  passes::DeclarationNamingPass names;
  names.run(tree, {}, true, /*skipImportedMoons=*/true);
  EXPECT_TRUE(sourceClass->hasQualifiedName());
  EXPECT_TRUE(ownClass->hasQualifiedName());
  EXPECT_FALSE(retainedClass->hasQualifiedName());
  EXPECT_EQ(retainedClass->getConstructor(), nullptr);
  EXPECT_FALSE(retainedClass->getDeclarationId());
  EXPECT_FALSE(imported.getDeclarationId());
  ASSERT_NE(sourceClass->getConstructor(), nullptr);
  ASSERT_NE(ownClass->getConstructor(), nullptr);
  EXPECT_TRUE(sourceClass->getDeclarationId());
  EXPECT_TRUE(ownClass->getDeclarationId());

  // The default traversal still prepares imports when requested by the
  // pipeline.
  fields.run(tree);
  identities.run(tree);
  names.run(tree);
  EXPECT_TRUE(retainedClass->hasQualifiedName());
  ASSERT_NE(retainedClass->getConstructor(), nullptr);
  EXPECT_TRUE(retainedClass->getDeclarationId());
  EXPECT_TRUE(imported.getDeclarationId());
  EXPECT_EQ(sourceClass->getConstructor()->function->getFieldInitializerCount(),
            1u);
  EXPECT_EQ(ownClass->getConstructor()->function->getFieldInitializerCount(),
            1u);
}
