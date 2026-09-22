/** Checks imported declaration preparation without loading files or types. */
#include <gtest/gtest.h>

#include "ast/class_definition_ast.h"
#include "ast/moon_scope_ast.h"
#include "semantic_analysis/passes/declaration_identity_pass.h"
#include "semantic_analysis/passes/moon_import_preparation_pass.h"
#include "support/error.h"

/** Keeps in-memory bundle builders local to these tests. */
namespace {
using namespace sun::ast;
using namespace sun::semantic_analysis;
using sun::semantic_analysis::passes::DeclarationIdentityPass;
using sun::semantic_analysis::passes::MoonImportPreparationPass;

/** Creates a portable identity for a declaration in a synthetic bundle. */
PortableDeclarationKey key(char bundle, uint64_t ordinal = 2) {
  return PortableDeclarationKey::original(std::string(64, bundle), ordinal);
}

/** Builds a compiled class stub with an intentionally unresolved field. */
std::unique_ptr<MoonScopeAST> bundle(char artifact, bool generic = false) {
  const std::string scope = "$" + std::string(64, artifact) + "$";
  std::vector<TypeParameter> parameters;
  if (generic) parameters.emplace_back("T");
  std::vector<ClassFieldDecl> fields;
  fields.push_back({"value", TypeAnnotation("Missing")});
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

/** Owns a complete AST and a fresh analysis session without a parser. */
class TypeAnalysis_MoonImportPreparation : public ::testing::Test {
 protected:
  std::shared_ptr<AnalysisResults> results =
      std::make_shared<AnalysisResults>();
  SemanticContext context{results};
  BlockExprAST tree;
  MoonImportPreparationPass pass{context};

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
      pass.run(tree);
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
TEST_F(TypeAnalysis_MoonImportPreparation, DependencyOrderDoesNotResolveTypes) {
  for (bool dependencyFirst : {false, true}) {
    auto importer = bundle('a');
    importer->requiredDeclarations.push_back({key('b'), "Value", std::nullopt});
    std::vector<std::unique_ptr<ExprAST>> nodes;
    if (dependencyFirst) nodes.push_back(bundle('b'));
    nodes.push_back(std::move(importer));
    if (!dependencyFirst) nodes.push_back(bundle('b'));
    BlockExprAST ordered(std::move(nodes), BlockKind::Module);
    auto session = std::make_shared<AnalysisResults>();
    SemanticContext isolated(session);
    MoonImportPreparationPass preparation(isolated);
    ASSERT_NO_THROW(preparation.run(ordered));
    EXPECT_TRUE(session->declarations.findPortable(key('b')));
    EXPECT_TRUE(isolated.rootScope().childModules.empty());
    for (const auto& node : ordered.getBody()) {
      const auto& moon = static_cast<const MoonScopeAST&>(*node);
      const auto& type = *moon.getBody().getBody()[0];
      EXPECT_FALSE(type.getDeclarationId());
      EXPECT_FALSE(type.hasResolvedType());
    }
    const auto count = session->declarations.size();
    ASSERT_NO_THROW(preparation.run(ordered));
    EXPECT_EQ(session->declarations.size(), count);
  }
}

/** Missing exact declarations are rejected even when source never uses them. */
TEST_F(TypeAnalysis_MoonImportPreparation, MissingDependency) {
  add(bundle('a'))
      .requiredDeclarations.push_back({key('b'), "Value", std::nullopt});
  expectDependencyError();
}

/** A declaration with the same name from another bundle cannot satisfy a key.
 */
TEST_F(TypeAnalysis_MoonImportPreparation, ConflictingVersion) {
  add(bundle('a'))
      .requiredDeclarations.push_back({key('b'), "Value", std::nullopt});
  add(bundle('c'));
  expectDependencyError("conflicting bundle supplied");
}

/** Interface requirements reject an exact key that identifies a class. */
TEST_F(TypeAnalysis_MoonImportPreparation, WrongTypeKind) {
  add(bundle('a'))
      .requiredDeclarations.push_back(
          {key('b'), "Value", sun::types::Type::Kind::Interface});
  add(bundle('b'));
  expectDependencyError("declaration has the wrong type kind");
}

/** Generic metadata requirements are checked without instantiating templates.
 */
TEST_F(TypeAnalysis_MoonImportPreparation, UnusedGenericRequirement) {
  auto& moon = add(bundle('a', true));
  moon.requiredDeclarations.push_back(
      {key('b'), "Value", sun::types::Type::Kind::Interface});
  expectDependencyError();
  EXPECT_EQ(context.lookupGenericClass("Value"), nullptr);
  EXPECT_FALSE(moon.getBody().getBody()[0]->getDeclarationId());
}

/** Source declarations and own-bundle wrappers are not imported records. */
TEST_F(TypeAnalysis_MoonImportPreparation, SkipsSourceAndOwnBundle) {
  auto source = bundle('a')->getBody().getBody()[0]->clone();
  source->setPrecompiled(false);
  std::vector<std::unique_ptr<ExprAST>> nodes;
  nodes.push_back(std::move(source));
  tree.prependExpressions(std::move(nodes));
  const auto count = results->declarations.size();
  ASSERT_NO_THROW(pass.run(tree));
  EXPECT_EQ(results->declarations.size(), count);
  auto own =
      MoonScopeAST::forOwnBundle("$own$", std::make_unique<BlockExprAST>());
  own->importedDeclarations.push_back({"invalid", 0, "", "", ""});
  own->requiredDeclarations.push_back({key('b'), "Value", std::nullopt});
  add(std::move(own));
  ASSERT_NO_THROW(pass.run(tree));
  EXPECT_EQ(results->declarations.size(), count);
}

/** Identity assignment attaches imported syntax to the prepared records. */
TEST_F(TypeAnalysis_MoonImportPreparation,
       IdentityAssignmentUsesPreparedRecords) {
  auto& moon = add(bundle('a', true));
  ASSERT_NO_THROW(pass.run(tree));
  const auto expected = results->declarations.findPortable(key('a'));
  const auto count = results->declarations.size();
  ASSERT_NO_THROW(DeclarationIdentityPass(results->declarations).run(tree));
  const auto& type =
      static_cast<const ClassDefinitionAST&>(*moon.getBody().getBody()[0]);
  EXPECT_EQ(type.getDeclarationId(), expected);
  EXPECT_EQ(type.getFields()[0].declaration.id,
            results->declarations.findPortable(key('a', 3)));
  ASSERT_EQ(type.declarationIdentity().typeParameters.size(), 1u);
  EXPECT_EQ(type.declarationIdentity().typeParameters[0],
            results->declarations.findPortable(key('a', 4)));
  EXPECT_EQ(results->declarations.size(), count);
  EXPECT_FALSE(type.hasResolvedType());
}
