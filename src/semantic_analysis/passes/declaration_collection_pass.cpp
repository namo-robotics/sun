#include "semantic_analysis/passes/import_traversal.h"

#include "semantic_analysis/method_signature_set.h"
// declaration_collection_pass.cpp — The declaration pre-pass (see
// declaration_collection_pass.h)

#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/passes/declaration_collection_pass.h"
#include "semantic_analysis/passes/declaration_preparation_guard.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/config.h"
#include "support/error.h"

using sun::semantic_analysis::QualifiedName;
using sun::types::TypePtr;
using sun::types::Types;

using sun::ast::ASTNodeType;
using sun::ast::BlockExprAST;
using sun::ast::ClassDefinitionAST;
using sun::ast::FunctionAST;
using sun::ast::ModuleAST;
using sun::ast::MoonScopeAST;
using sun::ast::PrototypeAST;
using sun::ast::VariableCreationAST;
using sun::support::logAndThrowError;

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

using sun::semantic_analysis::methodVisibility;

void DeclarationCollectionPass::run(BlockExprAST& block) {
  // Only hoist at module level (not inside function bodies where captures
  // and local variable ordering matter)
  if (!ctx_.isAtModuleLevel()) return;

  DeclarationPreparationGuard preparation(sema_.generics());

  // Bind this block's imports before anything in it is resolved. Declaration
  // order does not matter at module level, and a merged bundle places every
  // file-level `using` after the modules it precedes in source, so the
  // nested modules, class shapes and signatures below must not depend on
  // where the `using` sits. Type registration already created every module
  // scope, so the bindings resolve now.
  for (const auto& expr : block.getBody()) {
    SemanticContext::SourceFileGuard sourceFile(ctx_, expr->getSourceFileId());
    if (expr->getType() == ASTNodeType::USING) {
      registerUsing(static_cast<sun::ast::UsingAST&>(*expr));
    }
  }

  // Collect nested modules in the scopes prepared by type registration.
  for (const auto& expr : block.getBody()) {
    SemanticContext::SourceFileGuard sourceFile(ctx_, expr->getSourceFileId());
    switch (expr->getType()) {
      case ASTNodeType::MODULE: {
        auto& nsDecl = static_cast<ModuleAST&>(*expr);
        ctx_.enterModuleScope(nsDecl.getName());
        run(const_cast<BlockExprAST&>(nsDecl.getBody()));
        ctx_.exitScope();
        break;
      }
      case ASTNodeType::MOON_SCOPE: {
        // The bundle being built: its sources are collected here, in
        // declaration order with everything else, under the bundle's hash.
        // Imported bundles were completed by import preparation.
        auto& moonScope = static_cast<MoonScopeAST&>(*expr);
        if (!moonScope.isOwnBundle()) break;
        ctx_.enterModuleScope(moonScope.getContentHash());
        run(const_cast<BlockExprAST&>(moonScope.getBody()));
        ctx_.exitScope();
        break;
      }
      default:
        break;
    }
  }

  // Register class shapes (fields + method signatures) for the
  // non-generic classes. Function signatures collected below
  // may instantiate generic classes, and those specializations' method bodies
  // may call methods of any class in this block — so every class's methods
  // must be known before any signature is resolved.
  for (const auto& expr : block.getBody()) {
    SemanticContext::SourceFileGuard sourceFile(ctx_, expr->getSourceFileId());
    if (expr->getType() != ASTNodeType::CLASS_DEFINITION) continue;
    auto& classDef = static_cast<ClassDefinitionAST&>(*expr);
    if (classDef.isPartial() || classDef.isGeneric()) continue;
    QualifiedName qualifiedClass = classDef.getQualifiedName();
    if (ctx_.declarations().hasClassShape(classDef.getDeclarationId()))
      continue;
    auto classType = ctx_.lookupClass(classDef.getName());
    if (!classType) continue;
    // A duplicate name resolves to the first declaration. Body checking
    // reports the duplicate; do not register its fields on that first type.
    if (classType->getDeclarationId() != classDef.getDeclarationId()) continue;
    registerClassShape(classDef, qualifiedClass, classType);
  }

  // Register functions (signatures only, no body analysis).
  // Types are now available for parameter/return type resolution.
  for (const auto& expr : block.getBody()) {
    SemanticContext::SourceFileGuard sourceFile(ctx_, expr->getSourceFileId());
    if (expr->getType() == ASTNodeType::VARIABLE_CREATION) {
      auto& variable = static_cast<VariableCreationAST&>(*expr);
      if (variable.isCExtern()) {
        collectExternVariable(variable);
      } else if (variable.isPrecompiled()) {
        registerPrecompiledModuleVariable(variable);
      }
      continue;
    }

    if (expr->getType() == ASTNodeType::FUNCTION)
      collectFunctionSignature(static_cast<FunctionAST&>(*expr));
  }

  preparation.complete();
}

// Register a named, non-lambda function's signature (no body analysis) in
// the current scope. Templates were registered by TypeRegistrationPass.
void DeclarationCollectionPass::collectFunctionSignature(FunctionAST& func) {
  SemanticContext::SourceFileGuard sourceFile(ctx_, func.getSourceFileId());
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Skip lambdas and anonymous functions
  if (proto.getName().empty()) return;

  // Generic and parameter-pack templates were registered before resolution.
  if (proto.isTemplate()) return;

  std::vector<TypePtr> paramTypes;
  for (auto& [argName, argType] : proto.getMutableArgs()) {
    paramTypes.push_back(sema_.typeResolver().typeAnnotationToType(argType));
  }
  TypePtr returnType = Types::Void();
  if (proto.hasReturnType()) {
    returnType =
        sema_.typeResolver().typeAnnotationToType(*proto.getReturnType());
  }

  const auto& qualifiedName = proto.getQualifiedName();

  // Minimal FunctionInfo (no captures — those require body analysis)
  FunctionInfo info;
  info.returnType = returnType;
  info.paramTypes = std::move(paramTypes);
  info.qualifiedName = qualifiedName;
  info.declarationId = proto.getDeclarationId();
  info.canThrow = proto.canThrow();
  info.isCVariadic = proto.isCVariadic();
  info.isCExtern = func.isCExtern();
  info.isForwardDeclaration =
      func.isExtern() && !func.isCExtern() && !func.isPrecompiled();
  info.visibility = func.getVisibility();

  ctx_.currentScope().declareFunction(qualifiedName.baseName, info,
                                      ctx_.currentLocation());
}

void DeclarationCollectionPass::collectExternVariable(
    VariableCreationAST& varCreate) {
  if (!varCreate.hasTypeAnnotation()) {
    logAndThrowError("Extern variable '" + varCreate.getName() +
                         "' requires an explicit type",
                     varCreate.getLocation());
  }

  TypePtr type =
      sema_.typeResolver().typeAnnotationToType(*varCreate.getTypeAnnotation());
  QualifiedName qualified = varCreate.getQualifiedName();
  varCreate.setResolvedType(type);

  auto found = ctx_.scope()->variables.find(varCreate.getName());
  if (found != ctx_.scope()->variables.end()) {
    if (!found->second.isCExtern || !found->second.type || !type ||
        !found->second.type->equals(*type)) {
      logAndThrowError("Conflicting declaration of extern variable '" +
                           varCreate.getName() + "'",
                       varCreate.getLocation());
    }
    return;
  }

  VariableInfo info{type, true, false, false};
  info.declarationId = varCreate.getDeclarationId();
  info.visibility = varCreate.getVisibility();
  info.qualifiedName = qualified;
  info.isCExtern = true;
  ctx_.scope()->variables[varCreate.getName()] = info;
  ctx_.currentScope().declareModuleVariable(qualified, type,
                                            varCreate.getVisibility(), false,
                                            true, varCreate.getDeclarationId());
}

void DeclarationCollectionPass::registerPrecompiledModuleVariable(
    VariableCreationAST& varCreate) {
  // A bundle always states a global's type and never ships its initializer
  if (!varCreate.hasTypeAnnotation()) return;
  TypePtr type =
      sema_.typeResolver().typeAnnotationToType(*varCreate.getTypeAnnotation());
  if (!type) return;
  varCreate.setResolvedType(type);

  // A `const` the library computed at compile time can be computed with here
  // too; the storage is still the library's.
  if (varCreate.isConst() && varCreate.getImportedConstant() &&
      !varCreate.getGlobalInit()) {
    if (auto value = constants::adoptType(*varCreate.getImportedConstant(),
                                          sun::types::unwrapRef(type))) {
      constants::GlobalInitRecord record;
      record.kind = constants::GlobalInitKind::Image;
      record.isConst = true;
      record.value = std::move(*value);
      varCreate.setGlobalInit(std::move(record));
    }
  }

  // Bare-name lookup goes through `variables`, which body analysis would
  // normally populate; there is no body to analyze here.
  const std::string& name = varCreate.getName();
  VariableInfo info{type, true, false, false};
  info.declarationId = varCreate.getDeclarationId();
  info.visibility = varCreate.getVisibility();
  info.isConst = varCreate.isConst();
  info.isCExtern = varCreate.isCExtern();
  info.qualifiedName = varCreate.getQualifiedName();
  ctx_.scope()->variables[name] = info;

  // The stub's qualified name is already scoped by content hash; it must be
  // the one registered, since that is the symbol the bundle defines.
  ctx_.currentScope().declareModuleVariable(
      varCreate.getQualifiedName(), type, varCreate.getVisibility(),
      varCreate.isConst(), varCreate.isCExtern(), varCreate.getDeclarationId());
}

void DeclarationCollectionPass::registerUsing(sun::ast::UsingAST& usingDecl) {
  // "using A.B;" where A.B is a module name means "import all from A.B"
  std::string namespacePath = usingDecl.getNamespacePathString();
  std::string target = usingDecl.getTarget();
  if (usingDecl.getModuleDeclaration()) {
    auto id = ctx_.results().declarations.findPortable(
        *usingDecl.getModuleDeclaration());
    // A retained using may serve only a body already compiled into the bundle.
    // Actual nominal and module references still require their exact
    // dependency.
    if (!id) return;
    auto* scope = ctx_.lookupModuleScope(id);
    namespacePath =
        static_cast<const ModuleScope&>(*scope).qualifiedName.lookupName();
    if (usingDecl.isModuleImport()) target = "*";
  }

  if (!usingDecl.getModuleDeclaration() && !usingDecl.isModuleImport()) {
    std::string displayPath =
        namespacePath.empty() ? target : namespacePath + "." + target;
    if (auto* modScope = ctx_.lookupModuleScope(displayPath)) {
      ctx_.requireModuleAccessible(*modScope, usingDecl.getLocation());
      UsingImport import(displayPath, "*");
      ctx_.addUsingImport(import);
      ctx_.addImportBinding(ImportBinding::wildcard(modScope));
      return;
    }
  }

  // Normal case: import symbol or wildcard from namespace
  UsingImport import(namespacePath, target);
  ctx_.addUsingImport(import);
  if (auto* modScope = ctx_.lookupModuleScope(namespacePath)) {
    ctx_.requireModuleAccessible(*modScope, usingDecl.getLocation());
    if (import.isWildcard) {
      ctx_.addImportBinding(ImportBinding::wildcard(modScope));
    } else {
      ctx_.addImportBinding(ImportBinding(target, modScope, target));
    }
  }
}

void DeclarationCollectionPass::registerClassShape(
    ClassDefinitionAST& classDef, const QualifiedName& qualifiedClass,
    std::shared_ptr<sun::types::ClassType> classType) {
  if (!ctx_.declarations().noteClassShape(classDef.getDeclarationId())) return;

  // The class's declared lifetimes must be visible before any signature
  // that applies them ('ref Bus<'this>') resolves
  {
    std::vector<std::string> lifetimeNames;
    for (const auto& lp : classDef.getLifetimeParameters()) {
      lifetimeNames.push_back(lp.name);
    }
    classType->setLifetimeParams(std::move(lifetimeNames));
  }

  // Fields
  for (const auto& field : classDef.getFields()) {
    if (classType->hasField(field.name)) {
      logAndThrowError("Field '" + field.name + "' already exists in class '" +
                           classDef.getName() + "'",
                       field.location);
    }
    TypePtr fieldType = sema_.typeResolver().typeAnnotationToType(field.type);

    if constexpr (sun::support::Config::FORBID_REF_FIELDS_IN_CLASSES) {
      if (fieldType && fieldType->isReference()) {
        logAndThrowError("Field '" + field.name + "' in class '" +
                             classDef.getName() + "' has reference type '" +
                             fieldType->toDisplayString() +
                             "'. References cannot be stored in class "
                             "fields. Use a pointer type or store a copy.",
                         field.location);
      }
    }

    sema_.checkPackedFieldType(classDef, field, fieldType);
    classType->addField(field.name, fieldType, field.declaration.id)
        .visibility = field.visibility;
  }

  // Implemented interfaces (fields inherited, implementation recorded)
  sema_.inheritInterfaceFields(classDef, classType);

  // Method signatures ('this' resolves against the class being shaped)
  auto savedClass = ctx_.getCurrentClass();
  ctx_.setCurrentClass(classType);
  MethodSignatureSet methodSignatures(ctx_, sema_.typeResolver());
  for (const auto& methodDecl : classDef.getMethods()) {
    FunctionInfo methodInfo = sema_.getFunctionInfo(*methodDecl.function);
    PrototypeAST& proto =
        const_cast<PrototypeAST&>(methodDecl.function->getProto());
    if (!methodSignatures.insert(proto, methodInfo.paramTypes))
      logAndThrowError(
          "Function '" + proto.getName() + "' is already defined in this scope",
          methodDecl.function->getLocation());
    sema_.applyFunctionInfoToProto(proto, methodInfo);
    auto& method =
        classType->addMethod(proto.getName(), methodInfo.returnType,
                             methodInfo.paramTypes, methodDecl.isConstructor,
                             proto.getTypeParameterNames(), proto.canThrow());
    method.declarationId = proto.getDeclarationId();
    if (proto.getName() == "deinit")
      classType->deinitializer = method.declarationId;
    method.visibility = methodVisibility(*methodDecl.function);
    method.isConst = methodDecl.isConst;
    method.isUnsafe = methodDecl.function->getProto().isUnsafeMethod();
    method.isSynthesizedConstructor =
        methodDecl.function->isSynthesizedConstructor();
  }
  ctx_.setCurrentClass(savedClass);

  // The builtin IError predates all source, so it is registered with
  // message() returning static_ptr<u8> — the only string type that exists at
  // that point. The stdlib upgrades the contract: once std.String is known,
  // IError.message() returns an owned String clone, and every implementation
  // compiled after this line must match that signature.
  const auto module =
      ctx_.results().declarations.get(classDef.getDeclarationId()).module;
  if (qualifiedClass.baseName == "String" && module &&
      ctx_.results().declarations.get(module).name == "std") {
    if (auto ierror = ctx_.types()->errorInterface) {
      ierror->setMethodReturnType("message", classType);
    }
  }
}

void DeclarationCollectionPass::run(
    const std::vector<sun::ast::MoonScopeAST*>& imports) {
  forEachImportedBody(imports, ctx_,
                      [this](sun::ast::BlockExprAST& body) { run(body); });
}

}  // namespace sun::semantic_analysis::passes
