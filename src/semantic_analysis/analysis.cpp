// analysis.cpp — Main analysis entry points for semantic analyzer

#include <algorithm>
#include <set>

#include "semantic_analysis/c_abi_types.h"
#include "semantic_analysis/field_initialization.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_rules.h"
#include "support/config.h"
#include "support/error.h"

using sun::unwrapRef;
using sun::access::methodVisibility;
using sun::names::getFunctionSignature;
using sun::names::isReservedIdentifier;
using sun::rules::isAssignableTo;
using sun::rules::isBorrowableLvalue;
using sun::rules::tryCoerceIntegerLiteral;

// -------------------------------------------------------------------
// Main analysis entry point
// -------------------------------------------------------------------

void SemanticAnalyzer::analyze(ExprAST& expr) { analyzeExpr(expr); }

// -------------------------------------------------------------------
// Borrow targets
// -------------------------------------------------------------------

void SemanticAnalyzer::rejectBorrowOfByValueCapture(const ExprAST& target,
                                                    const Position& loc) {
  if (target.getType() != ASTNodeType::VARIABLE_REFERENCE) return;
  const auto& varRef = static_cast<const VariableReferenceAST&>(target);
  VariableInfo* varInfo = ctx_.lookupVariable(varRef.getName());
  if (!varInfo || varInfo->captureKind != CaptureKind::ByValue) return;
  const std::string& name = varRef.getName();
  logAndThrowError(
      "Cannot borrow '" + name +
          "': the lambda captures it by value, so the reference would alias "
          "the closure's private copy, not the original. Capture it with "
          "'[ref " +
          name + "]() => ...' to share the original, or '[const ref " + name +
          "]() => ...' to read it",
      loc);
}

void SemanticAnalyzer::validateBorrowTarget(const ExprAST& target,
                                            const Position& loc) {
  if (!isBorrowableLvalue(target)) {
    logAndThrowError(
        "Reference target must be a variable, field, or array element", loc);
  }
  rejectBorrowOfByValueCapture(target, loc);
  if (target.getType() == ASTNodeType::TERNARY) {
    const auto& ternary = static_cast<const TernaryExprAST&>(target);
    validateBorrowTarget(*ternary.getThen(), loc);
    validateBorrowTarget(*ternary.getElse(), loc);
    return;
  }
  if (target.getType() == ASTNodeType::INDEX) {
    const auto& indexExpr = static_cast<const IndexAST&>(target);
    auto baseType = sun::unwrapRef(indexExpr.getTarget()->getResolvedType());
    if (baseType && baseType->isClass()) {
      logAndThrowError(
          "Cannot create a reference to a class __index__ element - it "
          "has no storage address",
          loc);
    }
    if (indexExpr.hasSlices()) {
      logAndThrowError("Cannot create a reference to a slice", loc);
    }
  }
  checkPackedFieldNotBorrowed(target, loc);
}

// -------------------------------------------------------------------
// Expression analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeExpr(ExprAST& expr, sun::TypePtr expectedType) {
  SemanticContext::SourceFileGuard sourceFile(ctx_, expr.getSourceFileId());
  SemanticContext::LocationGuard locationGuard(ctx_, expr.getLocation());
  switch (expr.getType()) {
    case ASTNodeType::NUMBER:
      analyzeNumberLiteral(expr, expectedType);
      break;

    case ASTNodeType::CHAR_LITERAL: {
      // 'a' is always a char and b'a' is always a u8; neither takes its type
      // from context the way an integer literal does.
      expr.setResolvedType(types_.inferType(expr));
      break;
    }

    case ASTNodeType::STRING_LITERAL: {
      expr.setResolvedType(types_.inferType(expr));
      break;
    }

    case ASTNodeType::BOOL_LITERAL: {
      expr.setResolvedType(types_.inferType(expr));
      break;
    }

    case ASTNodeType::NULL_LITERAL: {
      expr.setResolvedType(types_.inferType(expr));
      break;
    }

    case ASTNodeType::STRUCT_LITERAL: {
      analyzeStructLiteral(static_cast<StructLiteralAST&>(expr), expectedType);
      break;
    }

    case ASTNodeType::ARRAY_LITERAL:
      analyzeArrayLiteral(static_cast<ArrayLiteralAST&>(expr), expectedType);
      break;

    case ASTNodeType::INDEX:
      analyzeIndexExpr(static_cast<IndexAST&>(expr));
      break;

    case ASTNodeType::SLICE:
      analyzeSliceExpr(expr);
      break;

    case ASTNodeType::VARIABLE_REFERENCE: {
      if (expr.getModuleQualifiedName()) {
        expr.setResolvedType(types_.inferType(expr));
        break;
      }
      auto& varRef = static_cast<VariableReferenceAST&>(expr);

      // An expected function-pointer type selects one overload without
      // changing ordinary call-site overload resolution.
      if (expectedType && expectedType->isFunction() &&
          !ctx_.lookupVariable(varRef.getName())) {
        sun::QualifiedName resolved =
            ctx_.resolveNameWithUsings(varRef.getName());
        std::vector<FunctionInfo> matches;
        for (const auto& candidate : ctx_.getAllFunctions(resolved.baseName)) {
          auto candidateType = sun::Types::Function(
              candidate.returnType, candidate.paramTypes, candidate.canThrow);
          if (isAssignableTo(candidateType, expectedType)) {
            matches.push_back(candidate);
          }
        }
        if (matches.size() == 1) {
          const FunctionInfo& match = matches.front();
          expr.setResolvedType(sun::Types::Function(
              match.returnType, match.paramTypes, match.canThrow));
          varRef.setQualifiedName(match.qualifiedName);
          break;
        }
        if (!ctx_.getAllFunctions(resolved.baseName).empty()) {
          logAndThrowError("No overload of '" + varRef.getName() +
                               "' matches expected type '" +
                               expectedType->toDisplayString() + "'",
                           varRef.getLocation());
        }
      }

      expr.setResolvedType(types_.inferType(expr));
      sun::QualifiedName resolved =
          ctx_.resolveNameWithUsings(varRef.getName());
      varRef.setQualifiedName(resolved);
      if (VariableInfo* info = ctx_.lookupVariable(varRef.getName())) {
        checkExternVariableAccessAllowed(*info, resolved.display(),
                                         varRef.getLocation());
      }
      break;
    }

    case ASTNodeType::VARIABLE_CREATION:
      analyzeVariableCreation(static_cast<VariableCreationAST&>(expr));
      break;

    case ASTNodeType::VARIABLE_ASSIGNMENT:
      analyzeVariableAssignment(static_cast<VariableAssignmentAST&>(expr));
      break;

    case ASTNodeType::COMPOUND_ASSIGNMENT:
      analyzeCompoundAssignment(static_cast<CompoundAssignmentAST&>(expr));
      break;

    case ASTNodeType::REFERENCE_CREATION:
      analyzeReferenceCreation(static_cast<ReferenceCreationAST&>(expr));
      break;

    case ASTNodeType::FUNCTION:
      analyzeFunctionDefinition(static_cast<FunctionAST&>(expr));
      break;

    case ASTNodeType::LAMBDA:
      analyzeLambdaExpr(static_cast<LambdaAST&>(expr));
      break;

    case ASTNodeType::BLOCK: {
      auto& block = static_cast<BlockExprAST&>(expr);
      analyzeBlock(block);
      expr.setResolvedType(types_.inferType(expr));
      break;
    }

    case ASTNodeType::IF:
      analyzeIfExpr(static_cast<IfExprAST&>(expr));
      break;

    case ASTNodeType::MATCH:
      analyzeMatchExpr(static_cast<MatchExprAST&>(expr), expectedType);
      break;

    case ASTNodeType::TERNARY:
      analyzeTernaryExpr(static_cast<TernaryExprAST&>(expr), expectedType);
      break;

    case ASTNodeType::FOR_LOOP:
      analyzeForLoop(static_cast<ForExprAST&>(expr));
      break;

    case ASTNodeType::FOR_IN_LOOP:
      analyzeForInLoop(static_cast<ForInExprAST&>(expr));
      break;

    case ASTNodeType::WHILE_LOOP: {
      auto& whileExpr = static_cast<WhileExprAST&>(expr);
      analyzeExpr(const_cast<ExprAST&>(*whileExpr.getCondition()));
      analyzeExpr(const_cast<ExprAST&>(*whileExpr.getBody()));
      expr.setResolvedType(sun::Types::Float64());  // while loops return 0.0
      break;
    }

    case ASTNodeType::BINARY:
      analyzeBinaryExpr(static_cast<BinaryExprAST&>(expr), expectedType);
      break;

    case ASTNodeType::UNARY:
      analyzeUnaryExpr(static_cast<UnaryExprAST&>(expr));
      break;

    case ASTNodeType::CALL: {
      auto& callExpr = static_cast<CallExprAST&>(expr);
      calls_.analyzeCall(callExpr, expectedType);
      break;
    }

    case ASTNodeType::INDEXED_ASSIGNMENT:
      analyzeIndexedAssignment(static_cast<IndexedAssignmentAST&>(expr));
      break;

    case ASTNodeType::RETURN:
      analyzeReturnExpr(static_cast<ReturnExprAST&>(expr));
      break;

    case ASTNodeType::MODULE:
      analyzeModuleDefinition(static_cast<ModuleAST&>(expr));
      break;

    case ASTNodeType::MOON_SCOPE:
      analyzeMoonScope(expr);
      break;

    case ASTNodeType::USING: {
      declarations_.registerUsing(static_cast<UsingAST&>(expr));
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::QUALIFIED_NAME:
      if (expr.getModuleQualifiedName())
        expr.setResolvedType(types_.inferType(expr));
      else
        analyzeQualifiedName(static_cast<QualifiedNameAST&>(expr));
      break;

    case ASTNodeType::CLASS_DEFINITION:
      analyzeClassDefinition(static_cast<ClassDefinitionAST&>(expr));
      break;

    case ASTNodeType::INTERFACE_DEFINITION:
      analyzeInterfaceDefinition(static_cast<InterfaceDefinitionAST&>(expr));
      break;

    case ASTNodeType::ENUM_DEFINITION: {
      analyzeEnumDefinition(static_cast<EnumDefinitionAST&>(expr));
      break;
    }

    case ASTNodeType::THIS: {
      expr.setResolvedType(types_.inferType(expr));
      break;
    }

    case ASTNodeType::MEMBER_ACCESS:
      if (expr.getModuleQualifiedName())
        expr.setResolvedType(types_.inferType(expr));
      else
        analyzeMemberAccess(static_cast<MemberAccessAST&>(expr), expectedType);
      break;

    case ASTNodeType::MEMBER_ASSIGNMENT:
      analyzeMemberAssignment(static_cast<MemberAssignmentAST&>(expr));
      break;

    case ASTNodeType::TRY_CATCH:
      analyzeTryCatch(static_cast<TryCatchExprAST&>(expr));
      break;

    case ASTNodeType::UNSAFE_BLOCK:
      analyzeUnsafeBlock(static_cast<UnsafeBlockAST&>(expr));
      break;

    case ASTNodeType::THROW:
      analyzeThrowExpr(static_cast<ThrowExprAST&>(expr));
      break;

    case ASTNodeType::GENERIC_CALL:
      calls_.analyzeGenericCall(static_cast<GenericCallAST&>(expr));
      break;

    case ASTNodeType::PACK_EXPANSION: {
      // Pack expansion is handled at codegen time
      // Just set the resolved type for now
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::DECLARE_TYPE:
      analyzeDeclareType(static_cast<DeclareTypeAST&>(expr));
      break;

    default:
      break;
  }
}

// -------------------------------------------------------------------
// Block analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeBlock(BlockExprAST& block) {
  // Declaration pre-pass: register all top-level declarations so that
  // ordering doesn't matter at module level.
  declarations_.collectDeclarations(block);

  // Sequential analysis of all statements (bodies, expressions, etc.)
  for (const auto& expr : block.getBody()) {
    analyzeExpr(*expr);
  }
}

// -------------------------------------------------------------------
// Parameter names and types
// -------------------------------------------------------------------

std::vector<sun::TypePtr> SemanticAnalyzer::validateAndResolveParamTypes(
    PrototypeAST& proto, std::optional<Position> loc,
    bool allowByValueObjects) {
  // Validate parameter names
  for (const auto& argName : proto.getArgNames()) {
    validateNotReserved(argName, "Parameter name", loc);
  }

  // Resolve parameter types
  std::vector<sun::TypePtr> paramTypes;
  for (auto& [argName, argType] : proto.getMutableArgs()) {
    sun::TypePtr paramType = types_.typeAnnotationToType(argType);

    // Check for compound types being passed by value
    if constexpr (sun::Config::REQUIRE_REF_FOR_COMPOUND_PARAMS) {
      // C externs are exempt: passing a struct by value is what the C ABI
      // specifies, so it is the callee's signature rather than a Sun choice.
      if (!allowByValueObjects && paramType && paramType->isCompound()) {
        // Error: compound types must be passed by reference
        logAndThrowError("Parameter '" + argName + "' has compound type '" +
                             paramType->toDisplayString() +
                             "' which cannot be passed by value. Use 'ref " +
                             paramType->toDisplayString() + "' instead.",
                         loc);
      }
    }
    // When REQUIRE_REF_FOR_COMPOUND_PARAMS is false, compound types are
    // passed by value with move semantics - no ref wrapping needed.

    paramTypes.push_back(paramType);
  }

  return paramTypes;
}

// -------------------------------------------------------------------
// Function info extraction (pure computation, no side effects)
// -------------------------------------------------------------------

FunctionInfo SemanticAnalyzer::getFunctionInfo(FunctionAST& func) {
  SemanticContext::SourceFileGuard sourceFile(ctx_, func.getSourceFileId());
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Only declared generic parameters may remain unresolved in a signature.
  SemanticContext::ScopeSwitchGuard signatureScope(ctx_, ctx_.scope());
  if (!proto.getTypeParameters().empty()) {
    std::vector<sun::TypePtr> parameters;
    for (const auto& parameter : proto.getTypeParameters()) {
      auto bound = ctx_.findTypeParameter(parameter.name);
      parameters.push_back(bound ? bound
                                 : sun::Types::TypeParameter(parameter.name));
    }
    ctx_.enterTypeParamScope(proto.getTypeParameterNames(), parameters);
  }

  // Validate function name (if named function, not lambda)
  if (!proto.getName().empty()) {
    validateNotReserved(proto.getName(), "Function name", func.getLocation());
  }

  std::vector<Capture> captures;

  // Validate and resolve parameter types. Only C externs may take objects by
  // value; see validateAndResolveParamTypes.
  std::vector<sun::TypePtr> paramTypes = validateAndResolveParamTypes(
      proto, func.getLocation(), /*allowByValueObjects=*/func.isCExtern());

  // Resolve return type if specified; Void for constructors (no return type)
  sun::TypePtr returnType = sun::Types::Void();
  if (proto.hasReturnType()) {
    returnType = types_.typeAnnotationToType(*proto.getReturnType());
    if (!returnType) {
      logAndThrowError("Failed to resolve return type for function '" +
                           proto.getName() + "'",
                       func.getLocation());
    }
  }

  // Compute qualified name (includes module path and function context for
  // nested functions). Precompiled stubs have pre-set qualified names with
  // content hash for symbol isolation.
  sun::QualifiedName qualifiedName;
  if (func.isCExtern()) {
    // The Sun-side name is scoped to its module like any other item, so
    // `public` and privacy mean what they say. Only the emitted symbol is
    // fixed by C — codegen takes that from the link name, never from here.
    // No overload suffix: C has no overloading.
    qualifiedName = ctx_.makeQualifiedName(proto.getName());
  } else if (proto.hasQualifiedName()) {
    qualifiedName = proto.getQualifiedName();
  } else {
    qualifiedName = ctx_.makeQualifiedName(proto.getName());
  }

  // Add param type suffix for overload disambiguation (unified with methods)
  // Skip for 'main' — it's an entry point with a fixed ABI name — and for
  // externs, whose ABI name is fixed by C.
  if (qualifiedName.paramSuffix.empty() && proto.getName() != "main" &&
      !func.isCExtern()) {
    qualifiedName.setParamSuffix(paramTypes);
  }

  FunctionInfo info;
  info.returnType = returnType;
  info.paramTypes = std::move(paramTypes);
  info.captures = std::move(captures);
  info.qualifiedName = qualifiedName;
  info.canThrow = proto.canThrow();
  info.isCVariadic = proto.isCVariadic();
  info.isCExtern = func.isCExtern();
  info.visibility = func.getVisibility();
  return info;
}

// -------------------------------------------------------------------
// Apply FunctionInfo to prototype
// -------------------------------------------------------------------

void SemanticAnalyzer::applyFunctionInfoToProto(PrototypeAST& proto,
                                                const FunctionInfo& info) {
  proto.setCaptures(info.captures);
  proto.setResolvedParamTypes(info.paramTypes);
  proto.setResolvedReturnType(info.returnType);
}

// -------------------------------------------------------------------
// Partial class analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::validateNotReserved(const std::string& name,
                                           const std::string& kind,
                                           std::optional<Position> location) {
  if (isReservedIdentifier(name)) {
    logAndThrowError(kind + " '" + name +
                         "' is invalid: names starting with '_' are "
                         "reserved for builtins",
                     location);
  }
}

// -------------------------------------------------------------------
// Class shape registration (fields + method signatures)
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzePartialClass(ClassDefinitionAST& classDef,
                                           ExprAST& expr) {
  const std::string& baseName = classDef.getName();

  auto existingClass = ctx_.lookupClass(baseName);
  if (existingClass) {
    // Primary already analyzed — validate and merge methods now
    for (const auto& extMethod : classDef.getMethods()) {
      const std::string& methodName = extMethod.function->getProto().getName();
      if (existingClass->getMethod(methodName)) {
        logAndThrowError("Method '" + methodName +
                             "' already defined in class '" + baseName + "'",
                         extMethod.function->getLocation());
      }
    }

    // Register and analyze extension methods on the existing class
    auto savedClass = ctx_.getCurrentClass();
    ctx_.setCurrentClass(existingClass);

    // Enter a Class scope to contain extension method scopes
    ctx_.enterClassScope(existingClass->getQualifiedName());

    // Register all extension methods first
    for (const auto& methodDecl : classDef.getMethods()) {
      FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);
      PrototypeAST& proto =
          const_cast<PrototypeAST&>(methodDecl.function->getProto());

      // Apply computed info to prototype
      applyFunctionInfoToProto(proto, methodInfo);

      auto& method = existingClass->addMethod(
          proto.getName(), methodInfo.returnType, methodInfo.paramTypes,
          methodDecl.isConstructor, proto.getTypeParameterNames(),
          proto.canThrow());
      method.visibility = methodVisibility(*methodDecl.function);
      method.isConst = methodDecl.isConst;
      std::string mangledName =
          existingClass->getMangledMethodName(proto.getName());
      std::vector<sun::TypePtr> methodParamTypes;
      methodParamTypes.push_back(existingClass);
      for (const auto& pt : methodInfo.paramTypes) {
        methodParamTypes.push_back(pt);
      }
      ctx_.registerFunctionInCurrentScope(
          mangledName, {methodInfo.returnType, methodParamTypes, {}});
    }

    // Analyze extension method bodies
    for (const auto& methodDecl : classDef.getMethods()) {
      analyzeFunction(*methodDecl.function);
    }
    // The parser rejects constructors in a partial class, so this is only a
    // backstop — and like the primary path it runs after every body is
    // analyzed, since the walk follows calls into them
    if (!classDef.isPrecompiled()) {
      for (const auto& methodDecl : classDef.getMethods()) {
        if (!methodDecl.isConstructor) continue;
        sun::checkFieldInitialization(*methodDecl.function, *existingClass,
                                      classDef.getMethods());
      }
    }

    ctx_.exitScope();  // Class scope

    // Merge methods into primary AST so codegen generates them
    for (auto* s = ctx_.scope(); s != nullptr; s = s->parent) {
      auto it = s->classDefinitions.find(baseName);
      if (it != s->classDefinitions.end()) {
        for (auto& extMethod : classDef.getMutableMethods()) {
          it->second->getMutableMethods().push_back(std::move(extMethod));
        }
        break;
      }
    }

    ctx_.setCurrentClass(savedClass);
  } else {
    // Primary not yet seen — stash for merging when primary is analyzed
    declarations_.deferExtension(baseName, &classDef);
  }
  expr.setResolvedType(sun::Types::Void());
}

// -------------------------------------------------------------------
// Function body analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeStructLiteral(StructLiteralAST& literal,
                                            const sun::TypePtr& expectedType) {
  if (!expectedType || !expectedType->isClass()) {
    logAndThrowError(
        "A '{ field: value }' literal needs a known class type. Annotate the "
        "target, as in `var x: MyClass = { ... };`.",
        literal.getLocation());
    return;
  }

  auto* classType = static_cast<sun::ClassType*>(expectedType.get());

  // A class with its own init is constructed through it; allowing both would
  // give two ways to build one object with different invariants.
  if (const auto* init = classType->getMethod("init");
      init && !init->isSynthesizedConstructor) {
    logAndThrowError("Class '" + classType->getDisplayName() +
                         "' declares an 'init', so construct it with "
                         "'" +
                         classType->getDisplayName() +
                         "(...)' rather than a '{ field: value }' literal.",
                     literal.getLocation());
    return;
  }

  std::set<std::string> seen;
  for (auto& field : literal.getMutableFields()) {
    const sun::ClassField* classField =
        ctx_.accessibleField(*classType, field.name, field.location);
    if (!classField) {
      logAndThrowError("Class '" + classType->getDisplayName() +
                           "' has no field '" + field.name + "'",
                       field.location);
      continue;
    }
    if (!seen.insert(field.name).second) {
      logAndThrowError(
          "Field '" + field.name + "' is initialized more than once",
          field.location);
      continue;
    }

    analyzeExpr(*field.value, classField->type);
    sun::TypePtr valueType = field.value->getResolvedType();
    checkMoveSource(*field.value, field.location);
    if (valueType && classField->type &&
        !isAssignableTo(valueType, classField->type)) {
      if (!tryCoerceIntegerLiteral(field.value.get(), classField->type,
                                   false)) {
        logAndThrowError(
            "Cannot initialize field '" + field.name + "' of type '" +
                classField->type->toDisplayString() +
                "' with a value of type '" + valueType->toDisplayString() + "'",
            field.location);
      }
    }
  }

  // Every field must be named. A field left out would silently be zero, which
  // is exactly the class of bug this syntax exists to prevent.
  std::string missing;
  for (const auto& classField : classType->getFields()) {
    if (seen.count(classField.name)) continue;
    if (!missing.empty()) missing += ", ";
    missing += classField.name;
  }
  if (!missing.empty()) {
    logAndThrowError("Struct literal for '" + classType->getDisplayName() +
                         "' is missing field(s): " + missing,
                     literal.getLocation());
  }

  literal.setResolvedType(expectedType);
}

void SemanticAnalyzer::checkExternVariableAccessAllowed(
    const VariableInfo& info, const std::string& displayName,
    const Position& loc) const {
  if (!info.isCExtern || ctx_.isInUnsafeBlock()) return;
  logAndThrowError(
      "Accessing extern variable '" + displayName +
          "' requires an unsafe block: C-owned storage is outside the borrow "
          "checker's guarantees. Wrap the access in `unsafe { ... }`, or "
          "expose it through a safe Sun wrapper.",
      loc);
}

// `mod.name = value`. A module is a namespace rather than an object, so the
// target is the module's own variable: it must exist, be visible, be
// assignable, and take the value's type. Codegen writes the global directly.
void SemanticAnalyzer::analyzeModuleGlobalAssignment(
    MemberAssignmentAST& assign, const sun::Type& objectType) {
  const auto& moduleType = static_cast<const sun::ModuleType&>(objectType);
  const std::string& modPath = moduleType.getModulePath();
  const std::string& memberName = assign.getMemberName();

  SymbolMatch match = ctx_.findSymbolInModule(modPath, memberName);
  if (!match) {
    logAndThrowError(
        "Unknown member '" + memberName + "' in module '" + modPath + "'",
        assign.getLocation());
  }
  if (match.kind != SymbolKind::Variable || !match.variableInfo) {
    logAndThrowError(
        "Cannot assign to '" + match.display() + "': it is not a variable",
        assign.getLocation());
  }

  const VariableInfo& target = *match.variableInfo;
  // display() names the declaring module without any library-hash scope
  std::string full = target.qualifiedName.display();
  checkExternVariableAccessAllowed(target, full, assign.getLocation());
  if (target.isConst) {
    logAndThrowError("Cannot assign to constant '" + full +
                         "'; declare it with 'var' if it must change",
                     assign.getLocation());
  }
  if (sun::isConstRef(target.type)) {
    logAndThrowError("Cannot assign through const reference '" + full + "'",
                     assign.getLocation());
  }

  // The declaration's own qualified name is the symbol codegen emitted the
  // global under, so that is what the write is pointed at
  assign.setQualifiedName(target.qualifiedName);

  sun::TypePtr expectedType = unwrapRef(target.type);
  analyzeExpr(const_cast<ExprAST&>(*assign.getValue()), expectedType);
  checkMoveSource(*assign.getValue(), assign.getLocation());

  sun::TypePtr rhsType = assign.getValue()->getResolvedType();
  if (rhsType && expectedType && !isAssignableTo(rhsType, expectedType)) {
    if (!tryCoerceIntegerLiteral(const_cast<ExprAST*>(assign.getValue()),
                                 expectedType, false)) {
      logAndThrowError("Cannot assign value of type '" +
                           rhsType->toDisplayString() + "' to '" + full +
                           "' of type '" + expectedType->toDisplayString() +
                           "'",
                       assign.getLocation());
    }
  }
}

void SemanticAnalyzer::validateExternSignature(FunctionAST& func) {
  const PrototypeAST& proto = func.getProto();

  // C varargs only make sense at a C boundary — a Sun function body has no
  // way to read them (no va_arg), so allowing `...` there would compile to a
  // signature nothing can use.
  if (proto.hasVariadicParam()) {
    logAndThrowError("Extern function '" + proto.getName() +
                         "' cannot use a named variadic pack; use C varargs "
                         "('...') instead",
                     func.getLocation());
  }

  auto describe = [](const sun::TypePtr& t) {
    return t ? t->toDisplayString() : std::string("<unresolved>");
  };

  auto validateCallback = [&](const sun::FunctionType& callback,
                              const std::string& paramName) {
    if (callback.canThrow()) {
      logAndThrowError("C callback parameter '" + paramName + "' cannot throw",
                       func.getLocation());
    }
    for (const auto& callbackParam : callback.getParamTypes()) {
      if (!sun::c_abi::isCallbackParameter(callbackParam)) {
        logAndThrowError("C callback parameter '" + paramName +
                             "' has unsupported callback argument type '" +
                             describe(callbackParam) + "'",
                         func.getLocation());
      }
    }
    if (!sun::c_abi::isCallbackReturn(callback.getReturnType())) {
      logAndThrowError("C callback parameter '" + paramName +
                           "' has unsupported callback return type '" +
                           describe(callback.getReturnType()) + "'",
                       func.getLocation());
    }
  };

  if (proto.hasResolvedParamTypes()) {
    const auto& params = proto.getResolvedParamTypes();
    for (size_t i = 0; i < params.size(); ++i) {
      if (params[i] && params[i]->isVoid()) {
        logAndThrowError("Parameter '" + proto.getArgs()[i].first +
                             "' of extern function '" + proto.getName() +
                             "' cannot be void",
                         func.getLocation());
      }
      if (auto* callback = sun::tryGetType<sun::FunctionType>(params[i])) {
        validateCallback(*callback, proto.getArgs()[i].first);
      }
      if (!sun::c_abi::isValue(params[i])) {
        logAndThrowError(
            "Parameter '" + proto.getArgs()[i].first +
                "' of extern function '" + proto.getName() + "' has type '" +
                describe(params[i]) +
                "', which has no C equivalent. Extern parameters must be a "
                "primitive, an enum, raw_ptr<T>, ref T (which is C's T*), or "
                "a class or function pointer.",
            func.getLocation());
      }
    }
  }

  if (proto.hasResolvedReturnType() &&
      !sun::c_abi::isReturn(proto.getResolvedReturnType())) {
    logAndThrowError(
        "Extern function '" + proto.getName() + "' returns '" +
            describe(proto.getResolvedReturnType()) +
            "', which has no C equivalent. Extern return types must be a "
            "primitive, an enum, raw_ptr<T>, or a class. Note that `ref T` "
            "cannot be returned; use raw_ptr<T>.",
        func.getLocation());
  }
}

// Sun has no implicit returns: a function whose signature promises a value
// must leave through an explicit `return` (or a throw) on every path. Checked
// after the body is analyzed, so match discriminants carry their types.
static void checkAllPathsReturn(const PrototypeAST& proto,
                                const BlockExprAST& body,
                                const sun::TypePtr& returnType,
                                const Position& loc) {
  if (!returnType || returnType->isVoid()) return;
  if (sun::rules::alwaysExits(body)) return;
  const std::string name =
      proto.getName().empty() ? "lambda" : "'" + proto.getName() + "'";
  logAndThrowError(
      "Function " + name + " can reach the end of its body without a value: " +
          "it must end in a `return` (or a throw) on every path. Sun has no "
          "implicit returns.",
      loc);
}

// An anonymous `<'_>` lambda type cannot be a return type: which frame the
// returned value's environment lives in cannot be told apart from the frame
// that is dying. A NAMED lifetime unpins it - 'function pick<'a>(...)
// <'a>() => i32' ties the result to frames the caller can see - so
// declarations that bind their own lifetimes pass allowNamed.
void SemanticAnalyzer::rejectRefEnvReturnType(
    const std::optional<TypeAnnotation>& returnType, const Position& location,
    bool allowNamed) {
  if (returnType && returnType->refEnv) {
    if (allowNamed && !returnType->lifetimeName.empty() &&
        returnType->lifetimeName != "_") {
      return;
    }
    if (!returnType->lifetimeName.empty() && returnType->lifetimeName != "_") {
      logAndThrowError(
          "a named frame-bound return type requires a lifetime available "
          "in this declaration",
          location);
    }
    logAndThrowError(
        "an anonymous <'_> lambda type cannot be a return type - its captured "
        "environment lives in a stack frame that dies when the function "
        "returns. Name the frame with a lifetime to allow it: "
        "function f<'a>(x: <'a>() => i32) <'a>() => i32",
        location);
  }
}

// Reject any lifetime name in the annotation that is not usable here: a
// name is usable when an enclosing function, lambda, class or interface
// declared it; the builtin 'this is usable only inside class and interface
// members.
void SemanticAnalyzer::checkAnnotationLifetimes(const TypeAnnotation& annot,
                                                const Position& location) {
  auto checkName = [&](const std::string& name) {
    if (name == "_") return;
    if (name == "this") {
      if (!allowThisLifetime_) {
        logAndThrowError(
            "the 'this lifetime is only usable inside class and interface "
            "members - it names the receiver's lifetime",
            location);
      }
      return;
    }
    if (std::find(activeLifetimeNames_.begin(), activeLifetimeNames_.end(),
                  name) == activeLifetimeNames_.end()) {
      logAndThrowError("use of undeclared lifetime '" + name +
                           ". Declare it on a function (function f<'" + name +
                           ">) or lambda literal (<'" + name +
                           ">(...) => ...), or on the class (class C<'" + name +
                           ">)",
                       location);
    }
  };
  if (!annot.lifetimeName.empty()) checkName(annot.lifetimeName);
  for (const auto& name : annot.lifetimeArguments) checkName(name);
  if (annot.elementType) checkAnnotationLifetimes(*annot.elementType, location);
  for (const auto& param : annot.paramTypes) {
    checkAnnotationLifetimes(*param, location);
  }
  if (annot.returnType) checkAnnotationLifetimes(*annot.returnType, location);
  for (const auto& typeArg : annot.typeArguments) {
    checkAnnotationLifetimes(*typeArg, location);
  }
}

// A signature's own lifetime list must have distinct names that do not
// shadow the enclosing class's, and every lifetime its annotations mention
// must be declared.
void SemanticAnalyzer::checkSignatureLifetimes(const PrototypeAST& proto,
                                               const Position& location) {
  for (const auto& lp : proto.getLifetimeParameters()) {
    if (std::count_if(proto.getLifetimeParameters().begin(),
                      proto.getLifetimeParameters().end(),
                      [&](const LifetimeParameter& other) {
                        return other.name == lp.name;
                      }) > 1) {
      logAndThrowError("duplicate lifetime parameter '" + lp.name, lp.span);
    }
    if (std::find(activeLifetimeNames_.begin(), activeLifetimeNames_.end(),
                  lp.name) != activeLifetimeNames_.end()) {
      logAndThrowError("lifetime '" + lp.name +
                           " is already declared by an enclosing declaration",
                       lp.span);
    }
  }
  size_t mark = activeLifetimeNames_.size();
  for (const auto& lp : proto.getLifetimeParameters()) {
    activeLifetimeNames_.push_back(lp.name);
  }
  for (const auto& [argName, argType] : proto.getArgs()) {
    checkAnnotationLifetimes(argType, location);
  }
  if (proto.hasReturnType()) {
    checkAnnotationLifetimes(*proto.getReturnType(), location);
  }
  activeLifetimeNames_.resize(mark);
}

void SemanticAnalyzer::analyzeFunction(FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  rejectRefEnvReturnType(proto.getReturnType(), func.getLocation(),
                         /*allowNamed=*/true);

  // Lifetime names in the signature must be declared; 'this needs a class
  bool savedAllowThis = allowThisLifetime_;
  allowThisLifetime_ = ctx_.getCurrentClass() != nullptr;
  checkSignatureLifetimes(proto, func.getLocation());
  allowThisLifetime_ = savedAllowThis;

  // For extern functions (no body), just validate and return
  if (func.isExtern()) {
    if (!proto.hasReturnType()) {
      logAndThrowError("Extern function '" + proto.getName() +
                           "' must have an explicit return type",
                       func.getLocation());
    }
    if (func.isCExtern()) validateExternSignature(func);
    return;
  }

  // A pack's arity and types come from the call site, so a template body
  // holding one has nothing concrete to check yet. Each specialization is
  // analyzed on instantiation (instantiateGenericFunction/Method).
  if (proto.hasVariadicParam()) return;

  // Sun has no va_arg, so C varargs are only meaningful on an extern
  // declaration where the callee is C code.
  if (proto.isCVariadic()) {
    logAndThrowError(
        "C varargs ('...') are only allowed on 'extern function' "
        "declarations; '" +
            proto.getName() + "' has a body",
        func.getLocation());
  }

  // Compute function signature from qualified name and resolved param types
  // This signature is used to create unique names for nested functions
  std::string funcSig = getFunctionSignature(proto.getMangledName(),
                                             proto.getResolvedParamTypes());

  // Return type for return-position inference. Some paths (class method
  // pass 2) reach here before the proto's resolved return type is applied;
  // resolve the annotation in the current scope (type parameter bindings for
  // specialized classes are active here).
  sun::TypePtr scopeReturnType = proto.getResolvedReturnType();
  if (!scopeReturnType && proto.hasReturnType() && !proto.isGeneric()) {
    scopeReturnType = types_.typeAnnotationToType(*proto.getReturnType());
  }

  // Enter function scope with signature for nested function qualification
  // Pass canThrow flag so throw expressions can be validated. A const method
  // body sees the const view of its return type: borrows of `this` are
  // `const ref` there, and the declared `ref` result is what callers with a
  // mutable receiver get.
  if (proto.isConstMethod())
    scopeReturnType = types_.createConstView(scopeReturnType);
  ctx_.enterFunctionScope(funcSig, proto.getQualifiedName(), proto.canThrow(),
                          scopeReturnType);

  // Declare 'this' for methods (when we're inside a class context); it is
  // immutable inside a const method
  if (ctx_.getCurrentClass()) {
    ctx_.declareVariable("this", ctx_.getCurrentClass(), /*isParam=*/true,
                         /*isConst=*/proto.isConstMethod());
  }

  // If this is a generic function/method, bind each type parameter to itself
  // so the body can be analyzed before any specialization exists. The binding
  // carries the parameter's constraint, which is what lets `<T: IShape>` reach
  // IShape's members on a value of type T (see inferMemberAccessType).
  if (proto.isGeneric()) {
    std::vector<std::string> typeParams;
    std::vector<sun::TypePtr> typeParamTypes;
    for (const auto& tp : proto.getTypeParameters()) {
      typeParams.push_back(tp.name);
      typeParamTypes.push_back(sun::Types::TypeParameter(
          tp.name, tp.constraint
                       ? (tp.constraint->qualifiedName
                              ? tp.constraint->qualifiedName->mangled()
                              : tp.constraint->name)
                       : ""));
    }
    ctx_.addTypeParameterBindings(typeParams, typeParamTypes);
  }

  // Field defaults see the definition scope and this, before parameters exist.
  bool savedAllowThisForDefaults = allowThisLifetime_;
  allowThisLifetime_ = ctx_.getCurrentClass() != nullptr;
  const auto& statements = func.getBody().getBody();
  for (size_t i = 0; i < func.getFieldInitializerCount(); ++i) {
    analyzeExpr(*statements.at(i));
  }
  allowThisLifetime_ = savedAllowThisForDefaults;

  // Declare parameters
  for (const auto& [argName, argType] : proto.getArgs()) {
    sun::TypePtr paramType = types_.typeAnnotationToType(argType);
    ctx_.declareVariable(argName, paramType, /*isParam=*/true);
  }

  // Add captured variables to scope (so nested functions can see them),
  // marked as captures so mutation checks and nested capture lists can
  // distinguish them from ordinary locals
  for (const auto& cap : proto.getCaptures()) {
    ctx_.declareVariable(cap.name, cap.type);
    if (VariableInfo* vi = ctx_.lookupVariable(cap.name)) {
      vi->captureKind = cap.kind;
      vi->isConst = cap.isConst;
    }
  }

  // Analyze the function body. The signature's lifetime names stay active
  // so annotations inside the body (locals, lambdas) can use them.
  size_t lifetimeMark = activeLifetimeNames_.size();
  for (const auto& lp : proto.getLifetimeParameters()) {
    activeLifetimeNames_.push_back(lp.name);
  }
  bool savedAllowThisForBody = allowThisLifetime_;
  allowThisLifetime_ = ctx_.getCurrentClass() != nullptr;
  declarations_.collectDeclarations(const_cast<BlockExprAST&>(func.getBody()));
  for (size_t i = func.getFieldInitializerCount(); i < statements.size(); ++i) {
    analyzeExpr(*statements[i]);
  }
  allowThisLifetime_ = savedAllowThisForBody;
  activeLifetimeNames_.resize(lifetimeMark);

  // No implicit returns: a non-void signature must be met by an explicit
  // return (or throw) on every path. Moon stubs carry no body to check.
  if (!ctx_.isInMoonScope()) {
    checkAllPathsReturn(proto, func.getBody(), scopeReturnType,
                        func.getLocation());
  }

  ctx_.exitScope();
}

// -------------------------------------------------------------------
// Lambda signature extraction (pure computation, no side effects)
// -------------------------------------------------------------------

FunctionInfo SemanticAnalyzer::getLambdaInfo(LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  // Build captures using current scope information
  std::vector<Capture> captures = buildCaptures(lambda);

  // Validate and resolve parameter types
  std::vector<sun::TypePtr> paramTypes = validateAndResolveParamTypes(proto);

  // Resolve return type (Sun requires return type annotations on lambdas,
  // parser enforces this, but check defensively)
  sun::TypePtr returnType = sun::Types::Void();
  if (proto.hasReturnType()) {
    returnType = types_.typeAnnotationToType(*proto.getReturnType());
    if (!returnType) {
      logAndThrowError("Failed to resolve return type for lambda",
                       lambda.getLocation());
    }
  }

  return {returnType, paramTypes, captures};
}

// -------------------------------------------------------------------
// Lambda body analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeLambda(LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  // Enter function scope (empty signature - lambdas are anonymous)
  // Nested functions in lambdas will still get outer function prefixes
  // Pass canThrow flag from the lambda's prototype
  ctx_.enterFunctionScope("", sun::QualifiedName(), proto.canThrow(),
                          proto.getResolvedReturnType());

  // Lambdas don't have type parameters (no generic lambdas)

  // Declare parameters
  for (const auto& [argName, argType] : proto.getArgs()) {
    sun::TypePtr paramType = types_.typeAnnotationToType(argType);
    ctx_.declareVariable(argName, paramType, /*isParam=*/true);
  }

  // Add captured variables to scope (so nested functions can see them),
  // marked as captures so mutation checks and nested capture lists can
  // distinguish them from ordinary locals
  for (const auto& cap : proto.getCaptures()) {
    ctx_.declareVariable(cap.name, cap.type);
    if (VariableInfo* vi = ctx_.lookupVariable(cap.name)) {
      vi->captureKind = cap.kind;
      vi->isConst = cap.isConst;
    }
  }

  // Keep the lambda's lifetime binders active for annotations nested in
  // its body, just as a named function does.
  size_t lifetimeMark = activeLifetimeNames_.size();
  for (const auto& lp : proto.getLifetimeParameters()) {
    activeLifetimeNames_.push_back(lp.name);
  }
  analyzeBlock(const_cast<BlockExprAST&>(lambda.getBody()));
  activeLifetimeNames_.resize(lifetimeMark);

  // Same rule as named functions: no implicit returns
  checkAllPathsReturn(proto, lambda.getBody(), proto.getResolvedReturnType(),
                      lambda.getLocation());

  ctx_.exitScope();
}

// -------------------------------------------------------------------
// Type parameter validation
// -------------------------------------------------------------------

void SemanticAnalyzer::validateTypeParameter(const sun::TypePtr& type,
                                             const ExprAST& node) {
  if (!type || !type->isTypeParameter()) return;

  auto* typeParam = static_cast<const sun::TypeParameterType*>(type.get());

  // Type traits (_Integer, _Float, etc.) are not scope-bound type parameters
  if (sun::isTypeTrait(typeParam->getName())) return;

  sun::TypePtr found = ctx_.findTypeParameter(typeParam->getName());
  if (!found) {
    const Position& loc = node.getLocation();
    std::string msg = "Unknown type parameter '" + typeParam->getName() +
                      "' at " + std::to_string(loc.line) + ":" +
                      std::to_string(loc.column) + " in '" + node.toString() +
                      "'. This is a bug in the compiler - please report it.";
    logAndThrowError(msg, loc);
  }
}

// -------------------------------------------------------------------
// Clear resolved types (for re-analysis of shared generic ASTs)
// -------------------------------------------------------------------

void SemanticAnalyzer::clearResolvedTypes(ExprAST& expr) {
  expr.clearResolvedType();

  // Recursively clear based on expression type
  switch (expr.getType()) {
    case ASTNodeType::BLOCK: {
      auto& block = static_cast<BlockExprAST&>(expr);
      for (const auto& stmt : block.getBody()) {
        clearResolvedTypes(const_cast<ExprAST&>(*stmt));
      }
      break;
    }
    case ASTNodeType::BINARY: {
      auto& bin = static_cast<BinaryExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*bin.getLHS()));
      clearResolvedTypes(const_cast<ExprAST&>(*bin.getRHS()));
      break;
    }
    case ASTNodeType::UNARY: {
      auto& unary = static_cast<UnaryExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*unary.getOperand()));
      break;
    }
    case ASTNodeType::CALL: {
      auto& call = static_cast<CallExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*call.getCallee()));
      for (const auto& arg : call.getArgs()) {
        clearResolvedTypes(const_cast<ExprAST&>(*arg));
      }
      break;
    }
    case ASTNodeType::MEMBER_ACCESS: {
      auto& ma = static_cast<MemberAccessAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ma.getObject()));
      break;
    }
    case ASTNodeType::INDEX: {
      auto& idx = static_cast<IndexAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*idx.getTarget()));
      for (const auto& slice : idx.getIndices()) {
        if (slice->hasStart())
          clearResolvedTypes(const_cast<ExprAST&>(*slice->getStart()));
        if (slice->hasEnd())
          clearResolvedTypes(const_cast<ExprAST&>(*slice->getEnd()));
      }
      break;
    }
    case ASTNodeType::VARIABLE_CREATION: {
      auto& vc = static_cast<VariableCreationAST&>(expr);
      if (vc.getValue()) {
        clearResolvedTypes(const_cast<ExprAST&>(*vc.getValue()));
      }
      break;
    }
    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      auto& va = static_cast<VariableAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*va.getValue()));
      break;
    }
    case ASTNodeType::MEMBER_ASSIGNMENT: {
      auto& ma = static_cast<MemberAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ma.getObject()));
      clearResolvedTypes(const_cast<ExprAST&>(*ma.getValue()));
      break;
    }
    case ASTNodeType::INDEXED_ASSIGNMENT: {
      auto& ia = static_cast<IndexedAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ia.getTarget()));
      clearResolvedTypes(const_cast<ExprAST&>(*ia.getValue()));
      break;
    }
    case ASTNodeType::COMPOUND_ASSIGNMENT: {
      auto& ca = static_cast<CompoundAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ca.getTarget()));
      clearResolvedTypes(const_cast<ExprAST&>(*ca.getValue()));
      break;
    }
    case ASTNodeType::IF: {
      auto& ifExpr = static_cast<IfExprAST&>(expr);
      clearResolvedTypes(*ifExpr.getCond());
      clearResolvedTypes(*ifExpr.getThen());
      if (ifExpr.getElse()) {
        clearResolvedTypes(*ifExpr.getElse());
      }
      break;
    }
    case ASTNodeType::TERNARY: {
      auto& ternary = static_cast<TernaryExprAST&>(expr);
      clearResolvedTypes(*ternary.getCond());
      clearResolvedTypes(*ternary.getThen());
      clearResolvedTypes(*ternary.getElse());
      break;
    }
    case ASTNodeType::FOR_LOOP: {
      auto& loop = static_cast<ForExprAST&>(expr);
      if (loop.getInit())
        clearResolvedTypes(const_cast<ExprAST&>(*loop.getInit()));
      if (loop.getCondition())
        clearResolvedTypes(const_cast<ExprAST&>(*loop.getCondition()));
      if (loop.getIncrement())
        clearResolvedTypes(const_cast<ExprAST&>(*loop.getIncrement()));
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getBody()));
      break;
    }
    case ASTNodeType::FOR_IN_LOOP: {
      auto& loop = static_cast<ForInExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getIterable()));
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getBody()));
      break;
    }
    case ASTNodeType::WHILE_LOOP: {
      auto& loop = static_cast<WhileExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getCondition()));
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getBody()));
      break;
    }
    case ASTNodeType::RETURN: {
      auto& ret = static_cast<ReturnExprAST&>(expr);
      if (ret.hasValue()) {
        clearResolvedTypes(const_cast<ExprAST&>(*ret.getValue()));
      }
      break;
    }
    case ASTNodeType::REFERENCE_CREATION: {
      auto& ref = static_cast<ReferenceCreationAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ref.getTarget()));
      break;
    }
    case ASTNodeType::GENERIC_CALL: {
      auto& gc = static_cast<GenericCallAST&>(expr);
      for (const auto& arg : gc.getArgs()) {
        clearResolvedTypes(const_cast<ExprAST&>(*arg));
      }
      break;
    }
    case ASTNodeType::TRY_CATCH: {
      auto& tc = static_cast<TryCatchExprAST&>(expr);
      clearResolvedTypes(const_cast<BlockExprAST&>(tc.getTryBlock()));
      for (const auto& clause : tc.getCatchClauses()) {
        clearResolvedTypes(*clause.body);
      }
      break;
    }
    case ASTNodeType::UNSAFE_BLOCK: {
      auto& ub = static_cast<UnsafeBlockAST&>(expr);
      clearResolvedTypes(ub.getBody());
      break;
    }
    case ASTNodeType::THROW: {
      auto& th = static_cast<ThrowExprAST&>(expr);
      if (th.hasErrorExpr()) {
        clearResolvedTypes(const_cast<ExprAST&>(th.getErrorExpr()));
      }
      break;
    }
    case ASTNodeType::ARRAY_LITERAL: {
      auto& arr = static_cast<ArrayLiteralAST&>(expr);
      for (const auto& elem : arr.getElements()) {
        clearResolvedTypes(const_cast<ExprAST&>(*elem));
      }
      break;
    }
    case ASTNodeType::MATCH: {
      auto& match = static_cast<MatchExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*match.getDiscriminant()));
      for (auto& arm : match.getArmsMutable()) {
        if (arm.pattern) {
          clearResolvedTypes(*arm.pattern);
        }
        arm.resolvedVariantTag = -1;
        for (auto& binding : arm.bindings) {
          binding.resolvedType = nullptr;
          binding.resolvedMangledName.clear();
        }
        clearResolvedTypes(*arm.body);
      }
      break;
    }
    // Terminal nodes (no children to recurse into)
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
    case ASTNodeType::CHAR_LITERAL:
    case ASTNodeType::BOOL_LITERAL:
    case ASTNodeType::NULL_LITERAL:
    case ASTNodeType::VARIABLE_REFERENCE:
    case ASTNodeType::THIS:
    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
      break;
    default:
      // For any other node types, just clear this node (may miss children)
      break;
  }
}

// -------------------------------------------------------------------
// Method analysis with type bindings
// -------------------------------------------------------------------

// Analyze one (cloned) method body of a specialized class. The caller has
// entered the specialized class's scope inside the template's definition
// scope, so the body sees exactly the names the template was written against.
void SemanticAnalyzer::analyzeMethodWithBindings(
    FunctionAST& methodFunc, std::shared_ptr<sun::ClassType> classType,
    const std::vector<std::string>& typeParams,
    const std::vector<sun::TypePtr>& typeArgs) {
  SemanticContext::SourceFileGuard sourceFile(ctx_,
                                              methodFunc.getSourceFileId());
  // Step 2: Set up scope with type parameter bindings (only if needed)
  // For generic class methods, type bindings are already in the Class scope
  bool needsTypeParamScope =
      !typeParams.empty() && typeParams.size() == typeArgs.size();
  if (needsTypeParamScope) {
    ctx_.enterTypeParamScope(typeParams, typeArgs);
  }

  // Step 3: Set class context for 'this' member access resolution
  auto savedClass = ctx_.getCurrentClass();
  if (classType) {
    ctx_.setCurrentClass(classType);
  }

  // Step 4: Enter method scope and declare 'this' parameter
  // Compute method signature with substituted param types for nested function
  // qualification
  const auto& proto = methodFunc.getProto();
  std::vector<sun::TypePtr> substitutedParamTypes;
  for (const auto& [argName, argType] : proto.getArgs()) {
    substitutedParamTypes.push_back(types_.typeAnnotationToType(argType));
  }
  std::string methodSig = getFunctionSignature(
      classType->getMangledMethodName(proto.getName()), substitutedParamTypes);
  std::string mangledMethodName =
      classType->getMangledMethodName(proto.getName());
  // Resolve the return type under the active bindings so return-position
  // inference (e.g. `return Option.None;`) has the expected type
  sun::TypePtr methodReturnType;
  if (proto.hasReturnType()) {
    methodReturnType = types_.typeAnnotationToType(*proto.getReturnType());
  }
  // A const method body sees the const view of its return type
  if (proto.isConstMethod())
    methodReturnType = types_.createConstView(methodReturnType);
  ctx_.enterFunctionScope(
      methodSig,
      sun::QualifiedName(classType->getQualifiedName().scopePath,
                         mangledMethodName),
      proto.canThrow(), methodReturnType);
  if (classType) {
    ctx_.declareVariable("this", classType, /*isParam=*/true,
                         /*isConst=*/proto.isConstMethod());
  }

  clearResolvedTypes(const_cast<BlockExprAST&>(methodFunc.getBody()));
  const auto& statements = methodFunc.getBody().getBody();
  for (size_t i = 0; i < methodFunc.getFieldInitializerCount(); ++i) {
    analyzeExpr(*statements.at(i));
  }

  // Step 5: Declare method parameters with substituted types
  for (size_t i = 0; i < proto.getArgs().size(); ++i) {
    const auto& [argName, argType] = proto.getArgs()[i];
    ctx_.declareVariable(argName, substitutedParamTypes[i], /*isParam=*/true);
  }

  // Analyze the source body after its parameters are in scope.
  declarations_.collectDeclarations(
      const_cast<BlockExprAST&>(methodFunc.getBody()));
  for (size_t i = methodFunc.getFieldInitializerCount(); i < statements.size();
       ++i) {
    analyzeExpr(*statements[i]);
  }

  // Step 7: Pop scopes and restore context
  ctx_.exitScope();  // method scope
  if (needsTypeParamScope) {
    ctx_.exitScope();  // type param scope
  }
  ctx_.setCurrentClass(savedClass);
}

// -------------------------------------------------------------------
// Call expression analysis
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// Bound method references: obj.method in value position
// -------------------------------------------------------------------

void SemanticAnalyzer::maybeResolveBoundMethodRef(MemberAccessAST& memberAccess,
                                                  sun::TypePtr expectedType) {
  sun::TypePtr objectType =
      unwrapRef(memberAccess.getObject()->getResolvedType());
  if (!objectType) return;

  // Unwrap raw_ptr<Class> / static_ptr<Class> (mirrors inferType)
  if (objectType->isRawPointer()) {
    sun::TypePtr pointee =
        static_cast<sun::RawPointerType*>(objectType.get())->getPointeeType();
    if (pointee && pointee->isClass()) objectType = pointee;
  } else if (objectType->isStaticPointer()) {
    sun::TypePtr pointee =
        static_cast<sun::StaticPointerType*>(objectType.get())
            ->getPointeeType();
    if (pointee && pointee->isClass()) objectType = pointee;
  }

  const std::string& memberName = memberAccess.getMemberName();

  // Interface methods as values are not supported (would need a vtable
  // load at bind time). Only diagnose when a lambda is expected so
  // interface method calls stay untouched.
  if (objectType->isInterface() && expectedType && expectedType->isLambda()) {
    auto* ifaceType = static_cast<sun::InterfaceType*>(objectType.get());
    if (ifaceType->getMethod(memberName)) {
      logAndThrowError("Referencing interface method '" + memberName +
                           "' as a value is not supported",
                       memberAccess.getLocation());
    }
    return;
  }

  if (!objectType->isClass()) return;
  const auto* classType = static_cast<const sun::ClassType*>(objectType.get());
  if (classType->getField(memberName)) return;

  std::vector<const sun::ClassMethod*> overloads;
  for (const auto& m : classType->getMethods()) {
    if (m.name == memberName) overloads.push_back(&m);
  }
  if (overloads.empty()) return;  // not a method (inferType already errored)

  const sun::ClassMethod* chosen = nullptr;
  if (overloads.size() == 1) {
    chosen = overloads[0];
  } else if (expectedType && expectedType->isLambda()) {
    // Pick the overload matching the expected lambda signature. A
    // non-throwing method may bind where a throwing lambda is expected.
    const auto* expected =
        static_cast<const sun::LambdaType*>(expectedType.get());
    std::vector<const sun::ClassMethod*> matches;
    for (const auto* m : overloads) {
      sun::LambdaType candidate(m->returnType, m->paramTypes, m->canThrow);
      if (candidate.equalsIgnoringThrow(*expected) &&
          (expected->canThrow() || !m->canThrow)) {
        matches.push_back(m);
      }
    }
    if (matches.size() == 1) chosen = matches[0];
  }

  if (!chosen) {
    logAndThrowError("Cannot reference overloaded method '" + memberName +
                         "' as a value; add a type annotation or call it with "
                         "arguments",
                     memberAccess.getLocation());
    return;
  }

  if (chosen->isGeneric()) {
    logAndThrowError(
        "Cannot use generic method '" + memberName + "' as a value",
        memberAccess.getLocation());
    return;
  }

  // The bound method will run on this receiver later, so the receiver must
  // allow it now
  checkMethodReceiver(*memberAccess.getObject(), memberName, chosen->isConst,
                      chosen->isConstructor, memberAccess.getLocation());

  auto boundType = sun::Types::Lambda(chosen->returnType, chosen->paramTypes,
                                      chosen->canThrow);
  // A bound method holds its receiver by reference, so the value is bound
  // to the frame the receiver lives in - the same escape rules as a lambda
  // with a `[ref ...]` capture list apply to it.
  static_cast<sun::LambdaType*>(boundType.get())->setHasRefCaptures(true);
  memberAccess.setResolvedType(std::move(boundType));
  memberAccess.setIsBoundMethodRef(true);
}


// -------------------------------------------------------------------
// Payload enums
