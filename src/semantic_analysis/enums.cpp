// enums.cpp — All enum semantic analysis: definitions, payload validation,
// variant construction, generic enum templates/instantiation, and match
// analysis with exhaustiveness checking.

#include <map>
#include <set>

#include "error.h"
#include "semantic_analyzer.h"

using sun::unwrapRef;

// -------------------------------------------------------------------
// Local helpers
// -------------------------------------------------------------------

namespace {

// True if `type` embeds enum `self` by value, walking enum payloads and class
// fields. Pointers break the cycle (indirection is the fix we suggest).
bool embedsEnumByValue(const sun::TypePtr& type, const sun::EnumType* self,
                       std::set<const sun::Type*>& visited) {
  if (!type || !visited.insert(type.get()).second) return false;
  if (type->isEnum()) {
    auto* e = static_cast<const sun::EnumType*>(type.get());
    if (e == self || e->getName() == self->getName()) return true;
    for (const auto& v : e->getVariants()) {
      for (const auto& pt : v.payloadTypes) {
        if (embedsEnumByValue(pt, self, visited)) return true;
      }
    }
  } else if (type->isClass()) {
    auto* c = static_cast<const sun::ClassType*>(type.get());
    for (const auto& field : c->getFields()) {
      if (embedsEnumByValue(field.type, self, visited)) return true;
    }
  }
  return false;
}

// Unify a payload annotation against an argument type, binding directly
// mentioned type parameters (T, raw_ptr<T>, static_ptr<T>). Nested generic
// payloads contribute no bindings (annotate the target instead). Returns
// false on a conflicting binding.
bool unifyPayloadTypeParam(const TypeAnnotation& annot,
                           const sun::TypePtr& argType,
                           const std::vector<std::string>& typeParams,
                           std::map<std::string, sun::TypePtr>& bindings,
                           std::string& conflictParam) {
  if (!argType) return true;
  sun::TypePtr arg = sun::unwrapRef(argType);

  bool isParam = false;
  for (const auto& p : typeParams) {
    if (p == annot.baseName) {
      isParam = true;
      break;
    }
  }
  if (isParam && !annot.elementType && annot.typeArguments.empty()) {
    auto it = bindings.find(annot.baseName);
    if (it != bindings.end()) {
      if (!it->second->equals(*arg)) {
        conflictParam = annot.baseName;
        return false;
      }
      return true;
    }
    bindings[annot.baseName] = arg;
    return true;
  }

  if (annot.elementType) {
    if (annot.baseName == "raw_ptr" && arg->isRawPointer()) {
      return unifyPayloadTypeParam(
          *annot.elementType,
          static_cast<sun::RawPointerType*>(arg.get())->getPointeeType(),
          typeParams, bindings, conflictParam);
    }
    if (annot.baseName == "static_ptr" && arg->isStaticPointer()) {
      return unifyPayloadTypeParam(
          *annot.elementType,
          static_cast<sun::StaticPointerType*>(arg.get())->getPointeeType(),
          typeParams, bindings, conflictParam);
    }
  }
  return true;
}

}  // namespace

// -------------------------------------------------------------------
// Registration and lookup
// -------------------------------------------------------------------

void SemanticAnalyzer::registerEnum(const std::string& name,
                                    std::shared_ptr<sun::EnumType> enumType) {
  // Register in current scope
  currentScope->enums[name] = enumType;
}

void SemanticAnalyzer::registerGenericEnum(const std::string& name,
                                           GenericEnumInfo info) {
  info.definitionScope = currentScope->shared_from_this();
  currentScope->genericEnums[name] = std::move(info);
}

const GenericEnumInfo* SemanticAnalyzer::lookupGenericEnum(
    const std::string& name) const {
  return currentScope->lookupGenericEnum(name);
}

// -------------------------------------------------------------------
// Definition analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeEnumDefinition(EnumDefinitionAST& enumDef) {
  // Forbid redefinition of enum in same module
  if (definedSymbols_.count(enumDef.getName())) {
    logAndThrowError("Redefinition of enum '" + enumDef.getName() + "'",
                     enumDef.getLocation());
  }

  // Validate enum name
  validateNotReserved(enumDef.getName(), "Enum name", enumDef.getLocation());

  // Validate variant names and check for duplicates
  std::set<std::string> seenVariants;
  for (const auto& variant : enumDef.getVariants()) {
    validateNotReserved(variant.name, "Enum variant name", variant.location);
    if (seenVariants.count(variant.name)) {
      logAndThrowError("Duplicate enum variant '" + variant.name +
                           "' in enum '" + enumDef.getName() + "'",
                       variant.location);
    }
    seenVariants.insert(variant.name);
  }

  // Generic enums register as templates only; payload annotations are
  // resolved per instantiation with the type arguments bound
  if (enumDef.isGeneric()) {
    if (!lookupGenericEnum(enumDef.getName())) {
      registerGenericEnum(
          enumDef.getName(),
          {&enumDef, enumDef.getTypeParameters(),
           makeQualifiedName(enumDef.getName())});
    }
    definedSymbols_.insert(enumDef.getName());
    enumDef.setResolvedType(sun::Types::Void());
    return;
  }

  // Create the enum type
  auto enumType = typeRegistry->getEnum(enumDef.getName());
  enumType->visibility = enumDef.getVisibility();
  enumType->setQualifiedName(makeQualifiedName(enumDef.getName()));

  // Add variants to the enum type (idempotent: declaration collection
  // already registered them)
  for (const auto& variant : enumDef.getVariants()) {
    enumType->addVariant(variant.name, variant.value);
  }

  // Resolve payload type annotations. This runs in full analysis (not
  // declaration collection) because payloads may reference classes
  // registered later in the same collection pass.
  for (const auto& variant : enumDef.getVariants()) {
    if (!variant.hasPayload()) continue;
    std::vector<sun::TypePtr> payloadTypes;
    for (const auto& annot : variant.payloadTypes) {
      auto payloadType = typeAnnotationToType(annot);
      validateEnumPayloadType(payloadType, enumType, variant.name,
                              variant.location);
      payloadTypes.push_back(std::move(payloadType));
    }
    enumType->setVariantPayloadTypes(variant.name, std::move(payloadTypes));
  }

  // Register the enum in the namespace
  registerEnum(enumDef.getName(), enumType);

  // Track symbol for redefinition detection
  definedSymbols_.insert(enumDef.getName());

  enumDef.setResolvedType(sun::Types::Void());
}

// -------------------------------------------------------------------
// Payload validation (Stage 1 rules)
// -------------------------------------------------------------------

void SemanticAnalyzer::validateEnumPayloadType(
    const sun::TypePtr& type, const std::shared_ptr<sun::EnumType>& enumType,
    const std::string& variantName, const Position& location) {
  const std::string context = "Payload of variant '" + variantName +
                              "' in enum '" + enumType->getDisplayName() + "'";
  if (!type || type->isVoid()) {
    logAndThrowError(context + " cannot be void", location);
  }
  // Allowlist: primitives, pointers, enums, classes (including owning ones —
  // payload enums carry drop glue), interfaces (fat pointers are copyable
  // borrowed views), and references (the variant stores the referent's
  // address and owns nothing — this is what lets a container hand back
  // `Option<ref T>` for a peek instead of a copy of an element it still
  // owns). Arrays, slices, lambdas, threads etc. are deferred.
  bool allowed = type->isPrimitive() || type->isRawPointer() ||
                 type->isStaticPointer() || type->isEnum() ||
                 type->isClass() || type->isInterface() ||
                 type->isReference();
  if (!allowed) {
    logAndThrowError(context + " has unsupported type '" + type->toString() +
                         "'; supported: primitives, pointers, enums, "
                         "interfaces, and classes",
                     location);
  }

  std::set<const sun::Type*> visited;
  if (embedsEnumByValue(type, enumType.get(), visited)) {
    logAndThrowError("Recursive enum '" + enumType->getDisplayName() +
                         "' requires indirection (raw_ptr)",
                     location);
  }
}

// -------------------------------------------------------------------
// Generic enum instantiation (monomorphization, mirrors generic classes):
// resolve each variant's payload annotations under the type parameter
// bindings and register the specialized EnumType. Specializations are
// recorded on the template AST, like other generic ASTs.
// -------------------------------------------------------------------

std::shared_ptr<sun::EnumType> SemanticAnalyzer::instantiateGenericEnum(
    const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs) {
  auto* genericInfo = lookupGenericEnum(baseName);
  if (!genericInfo || !genericInfo->AST) {
    return nullptr;
  }

  // Mangle from the template's registered name, not the spelling the caller
  // used: `sun.Option<i32>` and `Option<i32>` name the same specialization
  // (generic classes derive their name the same way).
  const std::string& templateName = genericInfo->qualifiedName.baseName.empty()
                                        ? baseName
                                        : genericInfo->qualifiedName.baseName;
  std::string mangledName =
      sun::Types::mangleGenericClassName(templateName, typeArgs);
  if (typeRegistry->hasEnum(mangledName)) {
    auto existing = typeRegistry->getEnum(mangledName);
    registerEnum(mangledName, existing);
    return existing;
  }

  if (typeArgs.size() != genericInfo->typeParameters.size()) {
    logAndThrowError("Generic enum '" + baseName + "' expects " +
                     std::to_string(genericInfo->typeParameters.size()) +
                     " type argument(s), got " +
                     std::to_string(typeArgs.size()));
  }

  auto specialized = typeRegistry->getEnum(mangledName);
  specialized->setBaseName(templateName);
  specialized->setGenericOrigin(templateName, typeArgs);
  specialized->visibility = genericInfo->AST->getVisibility();
  specialized->setQualifiedName(sun::QualifiedName(
      genericInfo->qualifiedName.scopePath, mangledName,
      genericInfo->qualifiedName.modulePath));

  {
    // Payload annotations resolve in the enum's definition scope; the result
    // is registered in the requesting scope below
    ScopeSwitchGuard definitionScope(*this, definitionScopeOf(*genericInfo));
    enterTypeParamScope(genericInfo->typeParameters, typeArgs);
    for (const auto& variant : genericInfo->AST->getVariants()) {
      specialized->addVariant(variant.name, variant.value);
      if (!variant.hasPayload()) continue;
      std::vector<sun::TypePtr> payloadTypes;
      for (const auto& annot : variant.payloadTypes) {
        auto payloadType = typeAnnotationToType(annot);
        payloadType = substituteTypeParameters(payloadType);
        validateEnumPayloadType(payloadType, specialized, variant.name,
                                variant.location);
        payloadTypes.push_back(std::move(payloadType));
      }
      specialized->setVariantPayloadTypes(variant.name,
                                          std::move(payloadTypes));
    }
    exitScope();
  }

  registerEnum(mangledName, specialized);
  // Record on the template AST (mirrors generic classes); codegen walks
  // these to build storage structs
  genericInfo->AST->addSpecialization(mangledName, specialized);
  return specialized;
}

// -------------------------------------------------------------------
// Variant construction: EnumName.Variant(args...)
// -------------------------------------------------------------------

// Intercepts calls whose callee is `EnumName.Variant` for concrete and
// generic enums. Returns true if the call was an enum construction (analyzed
// here); false lets analyzeCall continue with normal call handling.
bool SemanticAnalyzer::tryAnalyzeEnumConstruction(CallExprAST& callExpr,
                                                  sun::TypePtr expectedType) {
  if (callExpr.getCallee()->getType() != ASTNodeType::MEMBER_ACCESS) {
    return false;
  }
  auto& memberAccess =
      static_cast<MemberAccessAST&>(const_cast<ExprAST&>(*callExpr.getCallee()));
  if (memberAccess.getObject()->getType() !=
      ASTNodeType::VARIABLE_REFERENCE) {
    return false;
  }
  const auto& objRef =
      static_cast<const VariableReferenceAST&>(*memberAccess.getObject());
  // A local variable shadows an enum type name
  if (lookupVariable(objRef.getName())) {
    return false;
  }
  if (auto enumType = lookupEnum(objRef.getName())) {
    const_cast<ExprAST&>(*memberAccess.getObject()).setResolvedType(enumType);
    analyzeEnumVariantConstruction(callExpr, memberAccess, enumType);
    return true;
  }
  if (const auto* genericEnum = lookupGenericEnum(objRef.getName())) {
    analyzeGenericEnumConstruction(callExpr, memberAccess, objRef.getName(),
                                   *genericEnum, expectedType);
    return true;
  }
  return false;
}

void SemanticAnalyzer::analyzeEnumVariantConstruction(
    CallExprAST& callExpr, MemberAccessAST& memberAccess,
    const std::shared_ptr<sun::EnumType>& enumType) {
  const std::string& variantName = memberAccess.getMemberName();
  const auto* variant = enumType->getVariant(variantName);
  if (!variant) {
    logAndThrowError("Unknown variant '" + variantName + "' in enum '" +
                         enumType->getDisplayName() + "'",
                     memberAccess.getLocation());
  }
  if (!variant->hasPayload()) {
    logAndThrowError("Variant '" + variantName + "' of enum '" +
                         enumType->getDisplayName() +
                         "' carries no payload; write '" +
                         enumType->getBaseName() + "." + variantName +
                         "' without arguments",
                     callExpr.getLocation());
  }

  const auto& args = callExpr.getArgs();
  if (args.size() != variant->payloadTypes.size()) {
    logAndThrowError(
        "Variant '" + variantName + "' of enum '" +
            enumType->getDisplayName() + "' expects " +
            std::to_string(variant->payloadTypes.size()) +
            " payload value(s), got " + std::to_string(args.size()),
        callExpr.getLocation());
  }

  for (size_t i = 0; i < args.size(); ++i) {
    const sun::TypePtr& payloadType = variant->payloadTypes[i];
    analyzeExpr(const_cast<ExprAST&>(*args[i]), payloadType);
    sun::TypePtr argType = args[i]->getResolvedType();
    // A `ref X` payload borrows, so it accepts an X the same way a `ref X`
    // parameter does: the variant stores the argument's address.
    if (argType && payloadType && payloadType->isReference() &&
        !argType->isReference() &&
        unwrapRef(payloadType)->equals(*argType)) {
      continue;
    }
    if (argType && !isAssignableTo(argType, payloadType)) {
      if (!tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                                   payloadType, /*throwOnFail=*/false)) {
        logAndThrowError("Payload value " + std::to_string(i + 1) + " of '" +
                             enumType->getBaseName() + "." + variantName +
                             "' has type '" + argType->toDisplayString() +
                             "', expected '" + payloadType->toDisplayString() +
                             "'",
                         args[i]->getLocation());
      }
    }
  }

  memberAccess.setResolvedType(enumType);
  callExpr.setResolvedType(enumType);
}

void SemanticAnalyzer::analyzeGenericEnumConstruction(
    CallExprAST& callExpr, MemberAccessAST& memberAccess,
    const std::string& genericName, const GenericEnumInfo& genericInfo,
    sun::TypePtr expectedType) {
  const std::string& variantName = memberAccess.getMemberName();
  const EnumVariantDecl* variant = genericInfo.AST->getVariant(variantName);
  if (!variant) {
    logAndThrowError("Unknown variant '" + variantName + "' in enum '" +
                         genericName + "'",
                     memberAccess.getLocation());
  }
  if (!variant->hasPayload()) {
    logAndThrowError("Variant '" + variantName + "' of enum '" + genericName +
                         "' carries no payload; write '" + genericName + "." +
                         variantName + "' without arguments",
                     callExpr.getLocation());
  }

  const auto& args = callExpr.getArgs();
  if (args.size() != variant->payloadTypes.size()) {
    logAndThrowError("Variant '" + variantName + "' of enum '" + genericName +
                         "' expects " +
                         std::to_string(variant->payloadTypes.size()) +
                         " payload value(s), got " +
                         std::to_string(args.size()),
                     callExpr.getLocation());
  }

  // Analyze arguments to learn their types for unification
  for (const auto& arg : args) {
    analyzeExpr(const_cast<ExprAST&>(*arg));
  }

  std::map<std::string, sun::TypePtr> bindings;
  for (size_t i = 0; i < args.size(); ++i) {
    std::string conflictParam;
    if (!unifyPayloadTypeParam(variant->payloadTypes[i],
                               args[i]->getResolvedType(),
                               genericInfo.typeParameters, bindings,
                               conflictParam)) {
      logAndThrowError("Conflicting types inferred for type parameter '" +
                           conflictParam + "' of '" + genericName + "." +
                           variantName + "'",
                       callExpr.getLocation());
    }
  }

  // Fill parameters the arguments did not determine from the expected type
  const sun::EnumType* expectedEnum = nullptr;
  if (expectedType) {
    sun::TypePtr expected = unwrapRef(expectedType);
    if (expected && expected->isEnum()) {
      auto* et = static_cast<sun::EnumType*>(expected.get());
      if (et->getGenericBase() == genericName ||
          et->getBaseName() == genericName) {
        expectedEnum = et;
      }
    }
  }

  std::vector<sun::TypePtr> typeArgs;
  for (size_t i = 0; i < genericInfo.typeParameters.size(); ++i) {
    const std::string& param = genericInfo.typeParameters[i];
    auto it = bindings.find(param);
    if (it != bindings.end()) {
      // Unification reads through a reference argument, so a `ref X` argument
      // binds X. When the target says it wants `ref X` — `Option<ref T>` from
      // a peek accessor — honour that: the variant borrows the referent
      // instead of copying it out.
      const sun::TypePtr* expectedArg =
          expectedEnum && i < expectedEnum->getGenericArgs().size()
              ? &expectedEnum->getGenericArgs()[i]
              : nullptr;
      if (expectedArg && *expectedArg && (*expectedArg)->isReference() &&
          unwrapRef(*expectedArg)->equals(*it->second)) {
        typeArgs.push_back(*expectedArg);
        continue;
      }
      typeArgs.push_back(it->second);
      continue;
    }
    if (expectedEnum && i < expectedEnum->getGenericArgs().size()) {
      typeArgs.push_back(expectedEnum->getGenericArgs()[i]);
      continue;
    }
    logAndThrowError("Cannot infer type argument '" + param + "' for '" +
                         genericName + "." + variantName +
                         "'; add a type annotation to the target",
                     callExpr.getLocation());
  }

  auto specialized = instantiateGenericEnum(genericName, typeArgs);
  if (!specialized) {
    logAndThrowError("Failed to instantiate generic enum '" + genericName +
                         "'",
                     callExpr.getLocation());
  }
  const_cast<ExprAST&>(*memberAccess.getObject()).setResolvedType(specialized);
  analyzeEnumVariantConstruction(callExpr, memberAccess, specialized);
}

// -------------------------------------------------------------------
// Generic enum unit variants: Option.None (type args from expected type)
// -------------------------------------------------------------------

// Returns true if the member access was a generic-enum unit variant handled
// here (resolved type set); false lets the MEMBER_ACCESS case continue.
bool SemanticAnalyzer::tryAnalyzeGenericEnumUnitVariant(
    MemberAccessAST& memberAccess, sun::TypePtr expectedType) {
  if (memberAccess.getObject()->getType() !=
      ASTNodeType::VARIABLE_REFERENCE) {
    return false;
  }
  const auto& varRef =
      static_cast<const VariableReferenceAST&>(*memberAccess.getObject());
  if (lookupVariable(varRef.getName())) return false;
  const auto* genericEnum = lookupGenericEnum(varRef.getName());
  if (!genericEnum) return false;

  sun::TypePtr expected = expectedType ? unwrapRef(expectedType) : nullptr;
  sun::EnumType* expectedEnum = nullptr;
  if (expected && expected->isEnum()) {
    auto* et = static_cast<sun::EnumType*>(expected.get());
    if (et->getGenericBase() == varRef.getName() ||
        et->getBaseName() == varRef.getName()) {
      expectedEnum = et;
    }
  }
  if (!expectedEnum) {
    logAndThrowError("Cannot infer type arguments for '" + varRef.getName() +
                         "." + memberAccess.getMemberName() +
                         "'; add a type annotation to the target",
                     memberAccess.getLocation());
  }
  const auto* variant = expectedEnum->getVariant(memberAccess.getMemberName());
  if (!variant) {
    logAndThrowError("Unknown variant '" + memberAccess.getMemberName() +
                         "' in enum '" + varRef.getName() + "'",
                     memberAccess.getLocation());
  }
  if (variant->hasPayload()) {
    logAndThrowError("Variant '" + memberAccess.getMemberName() +
                         "' of enum '" + varRef.getName() +
                         "' carries a payload; construct it with '" +
                         varRef.getName() + "." +
                         memberAccess.getMemberName() + "(...)'",
                     memberAccess.getLocation());
  }
  const_cast<ExprAST&>(*memberAccess.getObject()).setResolvedType(expected);
  memberAccess.setResolvedType(expected);
  return true;
}

// -------------------------------------------------------------------
// Match analysis: variant patterns, payload bindings, exhaustiveness
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeEnumMatch(
    MatchExprAST& matchExpr, const std::shared_ptr<sun::EnumType>& enumType,
    sun::TypePtr expectedType) {
  std::set<int> coveredTags;
  bool sawWildcard = false;

  for (auto& arm : matchExpr.getArmsMutable()) {
    if (arm.isWildcard) {
      if (sawWildcard) {
        logWarning("Duplicate wildcard arm in match is unreachable",
                   matchExpr.getLocation());
      }
      sawWildcard = true;
      analyzeExpr(const_cast<ExprAST&>(*arm.body), expectedType);
      continue;
    }
    if (sawWildcard) {
      logWarning("Match arm after wildcard '_' is unreachable",
                 arm.pattern ? arm.pattern->getLocation()
                             : matchExpr.getLocation());
    }

    // Patterns on enum discriminants must be variant paths: Enum.Variant or
    // Enum.Variant(bindings). Resolve structurally - the last path segment
    // names the variant.
    if (arm.pattern->getType() != ASTNodeType::MEMBER_ACCESS) {
      logAndThrowError(
          "Match on enum '" + enumType->getDisplayName() +
              "' requires variant patterns (e.g. '" +
              enumType->getBaseName() + ".Variant') or '_'",
          arm.pattern->getLocation());
    }
    auto& patternAccess = static_cast<MemberAccessAST&>(*arm.pattern);
    const std::string& variantName = patternAccess.getMemberName();

    // The object must name this same enum type. For specializations the
    // pattern names the generic (Option.Some on an Option<i32> discriminant).
    sun::TypePtr objectType;
    if (patternAccess.getObject()->getType() ==
        ASTNodeType::VARIABLE_REFERENCE) {
      const auto& varRef = static_cast<const VariableReferenceAST&>(
          *patternAccess.getObject());
      objectType = lookupEnum(varRef.getName());
      if (!objectType && (varRef.getName() == enumType->getGenericBase() ||
                          varRef.getName() == enumType->getBaseName())) {
        objectType = enumType;
      }
    }
    if (!objectType) {
      objectType = inferType(*patternAccess.getObject());
    }
    if (!objectType || !objectType->isEnum() ||
        !objectType->equals(*enumType)) {
      logAndThrowError("Pattern does not match discriminant enum '" +
                           enumType->getDisplayName() + "'",
                       arm.pattern->getLocation());
    }
    const auto* variant = enumType->getVariant(variantName);
    if (!variant) {
      logAndThrowError("Unknown variant '" + variantName + "' in enum '" +
                           enumType->getDisplayName() + "'",
                       arm.pattern->getLocation());
    }

    // Payload arity: bindings must match the variant's payload count
    if (variant->hasPayload()) {
      if (!arm.hasPayloadParens) {
        logAndThrowError(
            "Variant '" + variantName + "' carries " +
                std::to_string(variant->payloadTypes.size()) +
                " payload value(s); bind them with '" +
                enumType->getBaseName() + "." + variantName + "(...)' or '_'",
            arm.pattern->getLocation());
      }
      if (arm.bindings.size() != variant->payloadTypes.size()) {
        logAndThrowError(
            "Variant '" + variantName + "' has " +
                std::to_string(variant->payloadTypes.size()) +
                " payload value(s), but pattern binds " +
                std::to_string(arm.bindings.size()),
            arm.pattern->getLocation());
      }
    } else if (arm.hasPayloadParens) {
      logAndThrowError("Variant '" + variantName +
                           "' carries no payload; remove the parentheses",
                       arm.pattern->getLocation());
    }

    if (coveredTags.count(static_cast<int>(variant->value))) {
      logWarning("Unreachable arm: variant '" + variantName +
                     "' is already matched",
                 arm.pattern->getLocation());
    }
    coveredTags.insert(static_cast<int>(variant->value));
    arm.resolvedVariantTag = static_cast<int>(variant->value);

    // Set resolved types on the pattern nodes so codegen and tooling see a
    // consistent tree (they are not analyzed via analyzeExpr).
    patternAccess.setResolvedType(enumType);
    const_cast<ExprAST&>(*patternAccess.getObject())
        .setResolvedType(enumType);

    // Bindings live in a per-arm scope
    enterScope();
    for (size_t i = 0; i < arm.bindings.size(); ++i) {
      auto& binding = arm.bindings[i];
      binding.resolvedType = variant->payloadTypes[i];
      if (!binding.isWildcard) {
        // Registered by plain name, like catch-clause bindings
        declareVariable(binding.name, binding.resolvedType);
      }
    }
    analyzeExpr(const_cast<ExprAST&>(*arm.body), expectedType);
    exitScope();
  }

  // Exhaustiveness: every variant covered, or a wildcard present
  if (!sawWildcard) {
    std::string missing;
    for (const auto& v : enumType->getVariants()) {
      if (!coveredTags.count(static_cast<int>(v.value))) {
        if (!missing.empty()) missing += ", ";
        missing += v.name;
      }
    }
    if (!missing.empty()) {
      logAndThrowError("Match on enum '" + enumType->getDisplayName() +
                           "' is not exhaustive; missing variants: " + missing,
                       matchExpr.getLocation());
    }
  } else if (coveredTags.size() == enumType->getNumVariants()) {
    logWarning("Wildcard '_' is unreachable: all variants of '" +
                   enumType->getDisplayName() + "' are already matched",
               matchExpr.getLocation());
  }
}

// -------------------------------------------------------------------
// Declaration-collection pre-pass
// -------------------------------------------------------------------

void SemanticAnalyzer::collectEnumDeclarations(const BlockExprAST& block) {
  for (const auto& expr : block.getBody()) {
    if (expr->getType() != ASTNodeType::ENUM_DEFINITION) continue;
    auto& enumDef = static_cast<EnumDefinitionAST&>(*expr);
    if (enumDef.isGeneric()) {
      if (!lookupGenericEnum(enumDef.getName())) {
        registerGenericEnum(
            enumDef.getName(),
            {&enumDef, enumDef.getTypeParameters(),
             makeQualifiedName(enumDef.getName())});
      }
      continue;
    }
    if (lookupEnum(enumDef.getName())) continue;
    auto enumType = typeRegistry->getEnum(enumDef.getName());
    for (const auto& variant : enumDef.getVariants()) {
      enumType->addVariant(variant.name, variant.value);
    }
    enumType->visibility = enumDef.getVisibility();
    enumType->setQualifiedName(makeQualifiedName(enumDef.getName()));
    registerEnum(enumDef.getName(), enumType);
  }
}
