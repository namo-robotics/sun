/** Checks function, module, and type-alias declarations. */
#include <set>

#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "support/error.h"

using sun::semantic_analysis::QualifiedName;
using sun::types::TypePtr;
using sun::types::Types;

using sun::ast::PrototypeAST;
using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::types::LambdaType;

void DeclarationAnalyzer::analyzeFunctionDefinition(
    sun::ast::FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Get function signature info (includes qualified name with function
  // context)
  FunctionInfo funcInfo = getFunctionInfo(func);

  if (funcInfo.isForwardDeclaration) {
    const sun::semantic_analysis::CallableSignature signature{
        funcInfo.qualifiedName.baseName, funcInfo.paramTypes};
    const auto definition = ctx_.scope()->functions.find(signature);
    if (definition != ctx_.scope()->functions.end() &&
        !definition->second.isForwardDeclaration) {
      if (!definition->second.returnType->equals(*funcInfo.returnType) ||
          definition->second.canThrow != funcInfo.canThrow)
        logAndThrowError("Forward declaration does not match its definition",
                         func.getLocation());
      func.setTargetDeclarationId(definition->second.declarationId);
    }
  }

  // Apply computed info to prototype
  applyFunctionInfoToProto(proto, funcInfo);

  // Templates were registered by the type registration pass.
  // Only register non-template functions in the normal function table.
  // Templates are looked up via the genericFunctions table instead.
  if (!proto.isTemplate()) {
    ctx_.currentScope().declareFunction(funcInfo.qualifiedName.baseName,
                                        funcInfo, ctx_.currentLocation());
  }

  // Analyze the function body
  sema_.bodies().analyzeFunction(func);

  // Set the function type on the function node
  func.setResolvedType(Types::Function(funcInfo.returnType, funcInfo.paramTypes,
                                       funcInfo.canThrow));
}

void DeclarationAnalyzer::analyzeModuleDefinition(sun::ast::ModuleAST& nsDecl) {
  // Enter the namespace scope
  ctx_.enterScope(ctx_.currentScope().declareModule(nsDecl));

  // Analyze the body of the namespace
  // Functions handle their own qualified name registration in FUNCTION case
  for (const auto& bodyExpr : nsDecl.getBody().getBody()) {
    if (bodyExpr->getType() == sun::ast::ASTNodeType::VARIABLE_CREATION) {
      // Variables need special handling to register in namespacedVariables
      auto& varCreate = static_cast<sun::ast::VariableCreationAST&>(*bodyExpr);
      if (bodyExpr->isPrecompiled()) {
        // A global imported from a .moon: the storage and its initial
        // value live in the bundle, so there is nothing to analyze — only
        // the type to resolve and the name to register.
        sema_.pipeline().declarations().registerPrecompiledModuleVariable(
            varCreate);
        continue;
      }
      // A tracked global registers itself, possibly before the walk gets
      // here; any other variable is registered now.
      const bool tracked = isTrackedGlobal(varCreate);
      sema_.analyzeExpr(*bodyExpr);
      if (!tracked) registerModuleVariable(varCreate);
    } else {
      sema_.analyzeExpr(*bodyExpr);
    }
  }

  // Exit the namespace scope
  ctx_.exitScope();
  nsDecl.setResolvedType(Types::Void());
}

void DeclarationAnalyzer::analyzeMoonScope(sun::ast::ExprAST& expr) {
  // A bundle's declarations live under its content hash, whether they are
  // an import's stubs or the sources of the bundle being built
  auto& moonScope = static_cast<sun::ast::MoonScopeAST&>(expr);
  const std::string& contentHash = moonScope.getContentHash();
  if (!contentHash.empty()) {
    ctx_.enterModuleScope(contentHash);
  }
  // An import's contents are stubs — signatures whose bodies were stripped
  // when the moon was built — so body-shape checks are off while we are in
  // there. The bundle being built is ordinary source and keeps every check.
  const bool stubs = !moonScope.isOwnBundle();
  if (stubs) ctx_.enterMoonScope();
  for (const auto& bodyExpr : moonScope.getBody().getBody()) {
    sema_.analyzeExpr(*bodyExpr);
  }
  if (stubs) ctx_.exitMoonScope();
  if (!contentHash.empty()) {
    ctx_.exitScope();
  }
  expr.setResolvedType(Types::Void());
}

void DeclarationAnalyzer::analyzeDeclareType(
    sun::ast::DeclareTypeAST& declareExpr) {
  // Trigger generic instantiation by resolving the type annotation
  TypePtr resolvedType = sema_.typeResolver().typeAnnotationToType(
      declareExpr.getTypeAnnotation(), true);
  declareExpr.setResolvedDeclaredType(resolvedType);

  // If there's an alias, register it
  if (declareExpr.hasAlias()) {
    const std::string& aliasName = declareExpr.getAliasName();
    ctx_.currentScope().declareTypeAlias(aliasName, resolvedType,
                                         declareExpr.getLocation());
  }

  declareExpr.setResolvedType(Types::Void());
}

}  // namespace sun::semantic_analysis
