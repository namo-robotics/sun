// analysis.cpp — Main analysis entry points for semantic analyzer

#include <algorithm>
#include <set>
#include <unordered_set>

#include "codegen/intrinsics/intrinsics.h"
#include "semantic_analysis/field_initialization.h"
#include "semantic_analysis/generic_type_arguments.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_rules.h"
#include "support/config.h"
#include "support/error.h"

using sun::unwrapRef;
using sun::access::methodVisibility;
using sun::names::getFunctionSignature;
using sun::names::isIntrinsic;
using sun::names::isReservedIdentifier;
using sun::rules::checkCharOperands;
using sun::rules::coerceBinaryLiteralOperands;
using sun::rules::isAssignableTo;
using sun::rules::isBorrowableLvalue;
using sun::rules::tryCoerceIntegerLiteral;
using sun::rules::unifyTernaryTypes;

namespace {

using sun::formatTypeList;

// "\n  - trim()\n  - trim(ref HeapAllocator)" — the candidate list shown
// after "No matching overload".
std::string formatCandidates(
    const std::string& name,
    const std::vector<std::vector<sun::TypePtr>>& candidates) {
  std::string out;
  for (const auto& params : candidates) {
    out += "\n  - " + name + "(" + formatTypeList(params) + ")";
  }
  return out;
}

}  // namespace

void SemanticAnalyzer::reportNoMethodForArgCount(
    const sun::ClassType& cls, const std::string& name,
    const std::vector<sun::TypePtr>& argTypes, const Position& loc) const {
  std::vector<std::vector<sun::TypePtr>> candidates;
  for (const auto& method : cls.getMethods()) {
    if (method.name != name) continue;
    // A generic method's recorded parameters are the uninstantiated ones, so
    // their count is not something to hold the call to.
    if (method.isGeneric()) return;
    if (method.paramTypes.size() == argTypes.size()) return;
    candidates.push_back(method.paramTypes);
  }
  if (candidates.empty()) return;

  logAndThrowError(
      "No matching overload of '" + name + "' for argument types (" +
          formatTypeList(argTypes) +
          "). Available overloads:" + formatCandidates(name, candidates),
      loc);
}

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
          "'lambda [ref " +
          name + "]' to share the original, or 'lambda [const ref " + name +
          "]' to read it",
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
      analyzeArrayLiteral(static_cast<ArrayLiteralAST&>(expr));
      break;

    case ASTNodeType::INDEX:
      analyzeIndexExpr(static_cast<IndexAST&>(expr));
      break;

    case ASTNodeType::SLICE:
      analyzeSliceExpr(expr);
      break;

    case ASTNodeType::VARIABLE_REFERENCE: {
      auto& varRef = static_cast<VariableReferenceAST&>(expr);
      expr.setResolvedType(types_.inferType(expr));
      sun::QualifiedName resolved =
          ctx_.resolveNameWithUsings(varRef.getName());
      varRef.setQualifiedName(resolved);
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
      analyzeCall(callExpr, expectedType);
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
      analyzeGenericCallExpr(static_cast<GenericCallAST&>(expr));
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
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Validate function name (if named function, not lambda)
  if (!proto.getName().empty()) {
    validateNotReserved(proto.getName(), "Function name", func.getLocation());
  }

  // Build captures using current scope information
  std::vector<Capture> captures = buildCaptures(func);

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
  if (classType->getMethod("init")) {
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

// An intrinsic that reads or writes unchecked memory has to be written inside
// an `unsafe { }` block. `sun::requiresUnsafeBlock` decides which ones; this is
// the only place it is applied, for generic and non-generic intrinsics alike.
void SemanticAnalyzer::checkRequiresUnsafeBlock(const std::string& name,
                                                   const Position& loc) const {
  if (ctx_.isInUnsafeBlock() || !sun::requiresUnsafeBlock(name)) return;
  logAndThrowError(
      "'" + name +
          "' reads or writes memory nothing has checked, so it can only be "
          "used in an unsafe block. Wrap the call in `unsafe { ... }`, or "
          "expose it through a safe Sun wrapper.",
      loc);
}

void SemanticAnalyzer::checkExternCallAllowed(const FunctionInfo& info,
                                              const std::string& displayName,
                                              const Position& loc) const {
  if (!info.isCExtern || ctx_.isInUnsafeBlock()) return;
  logAndThrowError(
      "Calling extern function '" + displayName +
          "' requires an unsafe block: C code is outside the borrow "
          "checker's guarantees. Wrap the call in `unsafe { ... }`, or "
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

const FunctionInfo* SemanticAnalyzer::resolveModuleQualifiedCall(
    const MemberAccessAST& memberAccess, const sun::TypePtr& objectType,
    const std::vector<sun::TypePtr>& argTypes) const {
  if (!objectType || !objectType->isModule()) return nullptr;

  auto* moduleType = static_cast<sun::ModuleType*>(objectType.get());
  SymbolMatch match = ctx_.findSymbolInModule(moduleType->getModulePath(),
                                              memberAccess.getMemberName(),
                                              SymbolKind::Function, &argTypes);
  if (!match || !match.functionInfo) return nullptr;

  checkExternCallAllowed(*match.functionInfo, memberAccess.getMemberName(),
                         memberAccess.getLocation());
  memberAccess.setQualifiedName(match.functionInfo->qualifiedName);
  return match.functionInfo;
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

  // What codegen can lower to a C-compatible signature:
  //  - primitives, which map 1:1
  //  - raw_ptr<T>, a bare pointer
  //  - ref T, which also lowers to a bare pointer and so *is* C's `T*`.
  //    Class layout already matches C (declaration order, natural padding),
  //    so `ref SomeClass` is exactly `struct SomeClass*`.
  //  - classes by value, via per-target C ABI classification (see abi/c_abi.h)
  // Still excluded are the types with no C spelling at all: arrays and slices
  // (fat pointers), interfaces (vtable pairs), lambdas (closures), and
  // error unions.
  // Payload enums have a Sun-private tagged-union layout with no C ABI
  // classification yet; only payload-free (i32) enums cross the C boundary.
  auto isCStyleEnum = [](const sun::TypePtr& t) {
    return t->isEnum() &&
           !static_cast<const sun::EnumType*>(t.get())->hasPayload();
  };
  auto isABISafeParam = [&](const sun::TypePtr& t) {
    return t && (t->isPrimitive() || t->isRawPointer() || t->isReference() ||
                 t->isClass() || isCStyleEnum(t));
  };
  // Returns allow the same, minus `ref`: Sun's ref return has auto-deref
  // semantics that do not correspond to anything C returns. Use raw_ptr<T>
  // for a returned pointer.
  auto isABISafeReturn = [&](const sun::TypePtr& t) {
    return t && (t->isPrimitive() || t->isRawPointer() || t->isClass() ||
                 isCStyleEnum(t));
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
      if (!isABISafeParam(params[i])) {
        logAndThrowError(
            "Parameter '" + proto.getArgs()[i].first +
                "' of extern function '" + proto.getName() + "' has type '" +
                describe(params[i]) +
                "', which has no C equivalent. Extern parameters must be a "
                "primitive, an enum, raw_ptr<T>, ref T (which is C's T*), or "
                "a class (passed by value per the C ABI).",
            func.getLocation());
      }
    }
  }

  if (proto.hasResolvedReturnType() &&
      !isABISafeReturn(proto.getResolvedReturnType())) {
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

// A bare '[ref]' lambda type cannot be a return type: which frame the
// returned value's environment lives in cannot be told apart from the frame
// that is dying. A NAMED lifetime unpins it - 'function pick<'a>(...)
// <'a>() -> i32' ties the result to frames the caller can see - so
// declarations that may name lifetimes (functions and methods, not lambda
// literals) pass allowNamed.
void SemanticAnalyzer::rejectRefEnvReturnType(
    const std::optional<TypeAnnotation>& returnType, const Position& location,
    bool allowNamed) {
  if (returnType && returnType->refEnv) {
    if (allowNamed && !returnType->lifetimeName.empty()) return;
    if (!returnType->lifetimeName.empty()) {
      logAndThrowError(
          "a '[ref]' lambda type cannot be a lambda's return type, even "
          "with a lifetime name - only named functions and methods declare "
          "lifetimes",
          location);
    }
    logAndThrowError(
        "a '[ref]' lambda type cannot be a return type - its captured "
        "environment lives in a stack frame that dies when the function "
        "returns. Name the frame with a lifetime to allow it: "
        "function f<'a>(x: <'a>() -> i32) <'a>() -> i32",
        location);
  }
}

// Reject any lifetime name in the annotation that is not usable here: a
// name is usable when an enclosing function, class or interface declared
// it; the builtin 'this is usable only inside class and interface members.
void SemanticAnalyzer::checkAnnotationLifetimes(const TypeAnnotation& annot,
                                                const Position& location) {
  auto checkName = [&](const std::string& name) {
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
                           ". Declare it on the function (function f<'" +
                           name + ">) or the class (class C<'" + name + ">)",
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
      logAndThrowError("duplicate lifetime parameter '" + lp.name,
                       lp.span);
    }
    if (std::find(activeLifetimeNames_.begin(), activeLifetimeNames_.end(),
                  lp.name) != activeLifetimeNames_.end()) {
      logAndThrowError("lifetime '" + lp.name +
                           " is already declared by the enclosing class",
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
          tp.name, tp.constraint ? tp.constraint->name : ""));
    }
    ctx_.addTypeParameterBindings(typeParams, typeParamTypes);
  }

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
  analyzeBlock(const_cast<BlockExprAST&>(func.getBody()));
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

  // Analyze the lambda body
  analyzeBlock(const_cast<BlockExprAST&>(lambda.getBody()));

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

  // Step 5: Declare method parameters with substituted types
  for (size_t i = 0; i < proto.getArgs().size(); ++i) {
    const auto& [argName, argType] = proto.getArgs()[i];
    ctx_.declareVariable(argName, substitutedParamTypes[i], /*isParam=*/true);
  }

  // Step 5.5: Clear old resolved types before re-analysis
  // This is critical for shared generic ASTs that may have resolvedType set
  // from a previous specialization (e.g., Map<i64,i64> types on Map<i64,i32>)
  clearResolvedTypes(const_cast<BlockExprAST&>(methodFunc.getBody()));

  // Step 6: Analyze the method body
  analyzeBlock(const_cast<BlockExprAST&>(methodFunc.getBody()));

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

SemanticAnalyzer::CalleeResolution SemanticAnalyzer::resolveCallee(
    CallExprAST& callExpr, const std::vector<sun::TypePtr>& argTypes) {
  auto calleeASTType = callExpr.getCallee()->getType();
  const auto& args = callExpr.getArgs();
  std::optional<FunctionInfo> resolvedFunc;
  std::shared_ptr<sun::ClassType> classType = nullptr;
  // Set when the callee swallows a variadic pack, whose arguments are not
  // part of the recorded parameter list — the arity check sits out.
  bool calleeTakesPack = false;
  // Set when a method is called on a constant receiver (see
  // checkMethodReceiver): a `ref T` result becomes `const ref T`
  bool receiverImmutable = false;

  // For function calls by name, do overload resolution before analyzing
  // callee This avoids errors for overloaded functions referenced by name
  if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
    auto& varRef = static_cast<VariableReferenceAST&>(
        const_cast<ExprAST&>(*callExpr.getCallee()));
    // Resolve the name through using imports (e.g., Vec -> sun_Vec)
    sun::QualifiedName resolved = ctx_.resolveNameWithUsings(varRef.getName());

    // Store the qualified name so codegen doesn't need to do name resolution
    if (resolved.mangled() != varRef.getName()) {
      varRef.setQualifiedName(resolved);
    }

    resolvedFunc = ctx_.lookupFunction(resolved.baseName, argTypes);
    if (resolvedFunc) {
      checkExternCallAllowed(*resolvedFunc, varRef.getName(),
                             callExpr.getLocation());
      // Set resolved type on the callee directly
      varRef.setResolvedType(sun::Types::Function(resolvedFunc->returnType,
                                                  resolvedFunc->paramTypes,
                                                  resolvedFunc->canThrow));
      // Set qualified name from the resolved function (handles import scopes)
      if (!resolvedFunc->qualifiedName.empty()) {
        varRef.setQualifiedName(resolvedFunc->qualifiedName);
      }
    } else {
      // Check if this is a class constructor call: ClassName(args...)
      // This creates a stack-allocated class instance
      classType = ctx_.lookupClass(resolved.baseName);
      // A generic function called without type arguments — `identity(42)`.
      // The arguments say what T is, so instantiate that specialization and
      // let the call resolve to it like any other named function.
      const GenericFunctionInfo* genericFunc =
          classType ? nullptr : ctx_.lookupGenericFunction(resolved.baseName);
      if (classType) {
        // Set resolved type on the callee to indicate this is a class
        // constructor call (stack-allocated)
        varRef.setResolvedType(classType);
      } else if (genericFunc) {
        // Inference reads the fixed parameters only — it stops at
        // genericInfo.params — so a call filling a pack still says what T is
        // from its leading arguments: `spawn(f, 1, 2)`.
        auto typeArgs = sun::generics::inferGenericTypeArguments(
            *genericFunc, argTypes, varRef.getName(), callExpr.getLocation());
        if (generics_.templateStillAbstract(*genericFunc, typeArgs)) {
          // In a template body the arguments are still type parameters; the
          // specialization is made when the enclosing generic is
          // instantiated. Until then the call has the substituted signature —
          // which lists the fixed parameters only, so a pack callee's extra
          // arguments must not be counted against it yet.
          if (genericFunc->AST &&
              genericFunc->AST->getProto().hasVariadicParam()) {
            calleeTakesPack = true;
          }
          varRef.setResolvedType(
              generics_.genericFunctionSignature(*genericFunc, typeArgs));
        } else {
          std::optional<std::vector<sun::TypePtr>> packArgTypes;
          if (genericFunc->AST &&
              genericFunc->AST->getProto().hasVariadicParam()) {
            // No calleeTakesPack here: the specialization's parameter list
            // already includes the pack's elements, so the ordinary
            // exact-arity check below is the right one.
            packArgTypes = generics_.splitPackArgTypes(
                genericFunc->AST->getProto(), argTypes, varRef.getName(),
                callExpr.getLocation());
          }
          SpecializedFunctionInfo specialized =
              generics_.requireGenericSpecialization(
                  *genericFunc, typeArgs, varRef.getName(),
                  callExpr.getLocation(), packArgTypes);
          resolvedFunc = specialized.asFunctionInfo();
          varRef.setQualifiedName(specialized.qualifiedName);
          varRef.setResolvedType(specialized.functionType());
        }
      } else {
        // Check if there are overloads for this function name - if so,
        // report a helpful "no matching overload" error
        auto allOverloads = ctx_.getAllFunctions(resolved.baseName);
        if (!allOverloads.empty()) {
          // Build error message with arg types and available overloads
          std::string argTypesStr;
          for (size_t i = 0; i < argTypes.size(); ++i) {
            if (i > 0) argTypesStr += ", ";
            argTypesStr +=
                argTypes[i] ? argTypes[i]->toDisplayString() : "unknown";
          }
          std::string overloadsStr;
          for (const auto& overload : allOverloads) {
            overloadsStr += "\n  - " + resolved.baseName + "(";
            for (size_t i = 0; i < overload.paramTypes.size(); ++i) {
              if (i > 0) overloadsStr += ", ";
              overloadsStr += overload.paramTypes[i]
                                  ? overload.paramTypes[i]->toDisplayString()
                                  : "unknown";
            }
            if (overload.isCVariadic) {
              overloadsStr += overload.paramTypes.empty() ? "..." : ", ...";
            }
            overloadsStr += ")";
          }
          logAndThrowError("No matching overload of '" + resolved.baseName +
                               "' for argument types (" + argTypesStr +
                               "). Available overloads:" + overloadsStr,
                           callExpr.getLocation());
        }
        // Not a function or class - analyze normally (will check variables,
        // etc.)
        analyzeExpr(varRef);
      }
    }
  } else if (calleeASTType == ASTNodeType::MEMBER_ACCESS) {
    // Handle method calls: object.method(args...)
    auto& memberAccess = static_cast<MemberAccessAST&>(
        const_cast<ExprAST&>(*callExpr.getCallee()));

    // First analyze the object expression to get its type
    analyzeExpr(const_cast<ExprAST&>(*memberAccess.getObject()));

    // Get object type (unwrap references)
    sun::TypePtr objectType = memberAccess.getObject()->getResolvedType();
    if (!objectType) {
      objectType = types_.inferType(*memberAccess.getObject());
    }
    objectType = unwrapRef(objectType);

    // For class types, do method overload resolution with argument types
    if (objectType && objectType->isClass()) {
      const auto* classType =
          static_cast<const sun::ClassType*>(objectType.get());
      const std::string& methodName = memberAccess.getMemberName();

      // Generic method ending in an `args...` pack (e.g.
      // allocator.create<Point>(...)): specialize HERE, where the actual call
      // argument types are known, so overloaded constructors resolve and the
      // specialization is keyed (mangled) by the pack's arg types. The
      // inferType trigger defers variadic methods to this path.
      FunctionAST* genericMethod =
          generics_.findGenericMethodAST(classType, methodName);
      bool variadicMethod =
          genericMethod && genericMethod->getProto().hasVariadicParam();
      if (variadicMethod && memberAccess.hasTypeArguments()) {
        calleeTakesPack = true;
        std::vector<sun::TypePtr> typeArgPtrs;
        for (const auto& ta : memberAccess.getTypeArguments()) {
          typeArgPtrs.push_back(types_.typeAnnotationToType(*ta));
        }
        memberAccess.setResolvedTypeArgs(typeArgPtrs);
        // Only what is left after the method's fixed parameters fills the
        // pack; `create<T>(args...)` has none, but `(x: i32, args...)` does.
        std::vector<sun::TypePtr> packArgTypes = *generics_.splitPackArgTypes(
            genericMethod->getProto(), argTypes, methodName,
            memberAccess.getLocation());
        memberAccess.setResolvedVariadicArgTypes(packArgTypes);

        auto mutableClassType =
            std::static_pointer_cast<sun::ClassType>(objectType);
        // Point the call at the specialization, under the name given where
        // it was instantiated (pack suffix included).
        if (auto specialized = generics_.instantiateGenericMethod(
                mutableClassType, methodName, typeArgPtrs, packArgTypes)) {
          memberAccess.setQualifiedName(
              specialized->getProto().getQualifiedName());
        }

        const sun::ClassMethod* method = ctx_.accessibleMethod(
            *classType, methodName, memberAccess.getLocation());
        if (method) {
          memberAccess.setResolvedType(
              sun::Types::Function(method->returnType, method->paramTypes));
          receiverImmutable = checkMethodReceiver(
              *memberAccess.getObject(), methodName, method->isConst,
              method->isConstructor, memberAccess.getLocation());
        }
      } else if (genericMethod && !variadicMethod) {
        // A generic method: whatever type arguments the call leaves out are
        // inferred from its arguments, then types_.inferType() instantiates the
        // specialization from the complete list (and does the same when all
        // of them were written).
        const sun::ClassMethod* method = ctx_.accessibleMethod(
            *classType, methodName, memberAccess.getLocation());
        std::vector<sun::TypePtr> written = types_.resolveTypeArguments(
            memberAccess.getTypeArguments(), memberAccess.getLocation(),
            "generic method call");
        if (method && written.size() < method->typeParameters.size()) {
          memberAccess.setResolvedTypeArgs(
              sun::generics::inferMethodTypeArguments(
                  *method, argTypes,
                  classType->getDisplayName() + "." + methodName,
                  memberAccess.getLocation(), written));
        }
        memberAccess.setResolvedType(types_.inferType(memberAccess));
        if (method) {
          receiverImmutable = checkMethodReceiver(
              *memberAccess.getObject(), methodName, method->isConst,
              method->isConstructor, memberAccess.getLocation());
        }
      } else {
        // Try to find a method overload matching the argument types
        const sun::ClassMethod* method = ctx_.accessibleMethodForArgs(
            *classType, methodName, argTypes, memberAccess.getLocation());
        if (method) {
          // Set the resolved type on the member access for later use
          memberAccess.setResolvedType(
              sun::Types::Function(method->returnType, method->paramTypes));
          receiverImmutable = checkMethodReceiver(
              *memberAccess.getObject(), methodName, method->isConst,
              method->isConstructor, memberAccess.getLocation());
        } else {
          // No overload took these arguments. When the mismatch is the
          // argument *count*, say so here: the fallback below picks an
          // arbitrary overload, and a zero-parameter one leaves nothing for
          // the arity check to compare against (issue #87).
          reportNoMethodForArgCount(*classType, methodName, argTypes,
                                    memberAccess.getLocation());
          // Fall back to first method with this name (will error on type
          // mismatch). Set the type directly (the object is already
          // analyzed) instead of analyzeExpr, so the callee is not converted
          // to a bound-method lambda — call position requires a FunctionType.
          memberAccess.setResolvedType(types_.inferType(memberAccess));
        }
      }
    } else if (const FunctionInfo* modFunc = resolveModuleQualifiedCall(
                   memberAccess, objectType, argTypes)) {
      // Module-qualified call: the overload is chosen from the argument
      // types here. types_.inferType() alone would only see the first overload.
      memberAccess.setResolvedType(
          sun::Types::Function(modFunc->returnType, modFunc->paramTypes));
    } else if (auto* staticPtr = types_.asNonClassStaticPtr(objectType)) {
      // static_ptr<T> builtin methods: length(), raw()
      memberAccess.setResolvedType(types_.inferStaticPtrMethodType(
          *staticPtr, memberAccess.getMemberName(), callExpr.getArgs().size(),
          memberAccess.getLocation()));
    } else {
      // Not a class type (interface, module, ptr-to-class, builtin...).
      // Set the type directly (the object is already analyzed) instead of
      // analyzeExpr, so a ptr-to-class method callee is not converted to a
      // bound-method lambda — call position requires a FunctionType.
      memberAccess.setResolvedType(types_.inferType(memberAccess));
      if (objectType && objectType->isInterface()) {
        const auto* iface =
            static_cast<const sun::InterfaceType*>(objectType.get());
        if (const auto* method =
                iface->getMethod(memberAccess.getMemberName())) {
          receiverImmutable = checkMethodReceiver(
              *memberAccess.getObject(), method->name, method->isConst,
              /*isConstructor=*/false, memberAccess.getLocation());
        }
      }
    }
  } else {
    // Not a simple variable reference or method call - analyze the callee
    // expression
    analyzeExpr(const_cast<ExprAST&>(*callExpr.getCallee()));
  }

  return {std::move(resolvedFunc), std::move(classType), calleeTakesPack,
          receiverImmutable};
}

// The `args...` of a call, analyzed and typed. Arguments go first so that
// overload resolution has real types to match, which means an argument that
// needs a hint — an array literal, an overloaded bound method reference —
// has to get it from a provisional look at the callee before it is analyzed.
std::vector<sun::TypePtr> SemanticAnalyzer::analyzeCallArguments(
    CallExprAST& callExpr, sun::TypePtr expectedType) {
  auto calleeASTType = callExpr.getCallee()->getType();
  // Get parameter types early for array literal type propagation
  std::vector<sun::TypePtr> expectedParamTypes;
  if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*callExpr.getCallee());
    // Resolve the name through using imports
    sun::QualifiedName resolved = ctx_.resolveNameWithUsings(varRef.getName());
    // Try to look up function parameters
    auto allFuncs = ctx_.getAllFunctions(resolved.baseName);
    if (!allFuncs.empty()) {
      // Use first overload's param types for type propagation
      expectedParamTypes = allFuncs[0].paramTypes;
    } else {
      // Check if this is a class constructor (use base name for lookup)
      auto classType = ctx_.lookupClass(resolved.baseName);
      if (classType) {
        // Get init method parameters
        if (auto* initMethod = classType->getMethod("init")) {
          expectedParamTypes = initMethod->paramTypes;
        }
      }
    }
  }

  // Propagate expected types to array literal arguments before analysis
  // This allows array literals to generate with the correct element type
  const auto& args = callExpr.getArgs();
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    if (args[i]->getType() == ASTNodeType::ARRAY_LITERAL) {
      sun::TypePtr paramType = expectedParamTypes[i];
      // Handle ref array<T> -> array<T>
      if (paramType && paramType->isReference()) {
        auto* refType = static_cast<const sun::ReferenceType*>(paramType.get());
        paramType = refType->getReferencedType();
      }
      if (paramType && paramType->isArray()) {
        // Set the expected type on the array literal before analysis
        const_cast<ExprAST&>(*args[i]).setResolvedType(paramType);
      }
    }
  }

  // Analyze arguments FIRST (before callee) to get types for overload
  // resolution. Member-access args get the expected param type so an
  // overloaded bound method reference can be disambiguated (kept narrow to
  // avoid changing literal coercion or free-function overload resolution).
  for (size_t i = 0; i < callExpr.getArgs().size(); ++i) {
    const auto& arg = callExpr.getArgs()[i];
    sun::TypePtr expected = (arg->getType() == ASTNodeType::MEMBER_ACCESS &&
                             i < expectedParamTypes.size())
                                ? expectedParamTypes[i]
                                : nullptr;
    analyzeExpr(const_cast<ExprAST&>(*arg), expected);
  }

  // Expand any variadic pack (`f(args...)`) into concrete typed args before
  // overload resolution, so argTypes below reflects the real arguments.
  expandPackArguments(callExpr.getArgsMutable());

  // Collect argument types for overload resolution
  std::vector<sun::TypePtr> argTypes;
  for (const auto& arg : callExpr.getArgs()) {
    argTypes.push_back(arg->getResolvedType());
  }

  return argTypes;
}

void SemanticAnalyzer::analyzeCall(CallExprAST& callExpr,
                                   sun::TypePtr expectedType) {
  // Non-generic intrinsics. The generic ones (_load<T>, _to_ref<T>, ...) are a
  // GenericCallAST and go through analyzeGenericCallExpr, which applies the
  // same predicate.
  auto calleeASTType = callExpr.getCallee()->getType();
  if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*callExpr.getCallee());
    checkRequiresUnsafeBlock(varRef.getName(), callExpr.getLocation());
  }

  // Enum variant construction: EnumName.Variant(args...) for concrete and
  // generic enums; intercepted before generic callee analysis (see enums.cpp)
  if (tryAnalyzeEnumConstruction(callExpr, expectedType)) {
    return;
  }

  std::vector<sun::TypePtr> argTypes =
      analyzeCallArguments(callExpr, expectedType);
  const auto& args = callExpr.getArgs();

  // Work out what is actually being called, and what that means for the
  // arguments: which overload, whether it is a constructor, whether it
  // swallows a pack, and whether its receiver was constant.
  CalleeResolution callee = resolveCallee(callExpr, argTypes);
  const std::optional<FunctionInfo>& resolvedFunc = callee.function;
  // Not const: a module-qualified constructor call fills this in below.
  std::shared_ptr<sun::ClassType>& classType = callee.classType;
  const bool calleeTakesPack = callee.takesPack;
  const bool receiverImmutable = callee.receiverImmutable;

  // Type check: verify argument types match parameter types
  sun::TypePtr calleeSunType = callExpr.getCallee()->getResolvedType();
  if (!calleeSunType) {
    calleeSunType = types_.inferType(*callExpr.getCallee());
  }
  // A module-qualified constructor call (`m.Point(...)`) resolves the callee
  // to the class type; treat it like `Point(...)` below. Only a member of a
  // module names a class this way — a method that returns a class, such as
  // `t.join()` on a `Thread<Point>`, has the same callee type but is a call,
  // not a construction.
  if (!classType && calleeSunType && calleeSunType->isClass() &&
      callExpr.getCallee()->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto& calleeMember =
        static_cast<const MemberAccessAST&>(*callExpr.getCallee());
    sun::TypePtr ownerType = calleeMember.getObject()->getResolvedType();
    if (!ownerType) ownerType = types_.inferType(*calleeMember.getObject());
    if (ownerType && ownerType->isModule()) {
      classType = std::static_pointer_cast<sun::ClassType>(calleeSunType);
    }
  }
  std::vector<sun::TypePtr> paramTypes;
  // Whether paramTypes came from a callee whose signature we actually know.
  // An empty parameter list is a real signature (`f()`), so emptiness alone
  // cannot stand in for "unknown" — that is what let calls to zero-parameter
  // methods and lambdas past the arity check (issue #87).
  bool knownSignature = false;

  if (resolvedFunc) {
    paramTypes = resolvedFunc->paramTypes;
    knownSignature = true;
  } else if (calleeSunType && calleeSunType->isFunction()) {
    paramTypes = static_cast<const sun::FunctionType*>(calleeSunType.get())
                     ->getParamTypes();
    knownSignature = true;
  } else if (calleeSunType && calleeSunType->isLambda()) {
    paramTypes = static_cast<const sun::LambdaType*>(calleeSunType.get())
                     ->getParamTypes();
    knownSignature = true;
  } else if (classType && classType->isClass()) {
    // Class constructor call: look up init method with overload resolution
    auto* ct = static_cast<const sun::ClassType*>(classType.get());
    const auto* initMethod = ctx_.accessibleMethodForArgs(
        *ct, "init", argTypes, callExpr.getLocation());
    if (initMethod) {
      paramTypes = initMethod->paramTypes;
      knownSignature = true;
    } else if (!ct->getMethod("init") && !args.empty()) {
      // No init at all, but arguments were supplied. Field-wise construction
      // is spelled with a struct literal, where each field is named: relying
      // on declaration order would silently change meaning if two same-typed
      // fields were ever reordered.
      logAndThrowError(
          "Class '" + ct->toDisplayString() +
              "' declares no 'init', so it cannot be constructed positionally."
              " Use a struct literal naming each field: `var x: " +
              ct->toDisplayString() + " = { ... };`",
          callExpr.getLocation());
    } else if (ct->getMethod("init")) {
      // The class declares one or more init methods but none are compatible
      // with the supplied arguments.
      std::string argList;
      for (size_t i = 0; i < argTypes.size(); ++i) {
        if (i > 0) argList += ", ";
        argList += argTypes[i] ? argTypes[i]->toDisplayString() : "?";
      }
      // List what the class does declare — with overloads, "no match" alone
      // leaves the caller guessing which one they nearly hit.
      std::string candidates;
      for (const auto& method : ct->getMethods()) {
        if (method.name != "init") continue;
        std::string params;
        for (size_t i = 0; i < method.paramTypes.size(); ++i) {
          if (i > 0) params += ", ";
          params += method.paramTypes[i]
                        ? method.paramTypes[i]->toDisplayString()
                        : "?";
        }
        candidates += "\n       candidate: init(" + params + ")";
      }
      logAndThrowError("No matching constructor for '" + ct->toDisplayString() +
                           "' with arguments (" + argList + ")" + candidates,
                       callExpr.getLocation());
    }
  }

  // What to call the callee in diagnostics: a plain call gives its function
  // name, a method call its member name. Only a plain call can name an
  // intrinsic, so the intrinsic-only conversions below key off that form.
  std::string funcName = "<unknown>";
  if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
    funcName = static_cast<const VariableReferenceAST&>(*callExpr.getCallee())
                   .getName();
  } else if (calleeASTType == ASTNodeType::MEMBER_ACCESS) {
    funcName = static_cast<const MemberAccessAST&>(*callExpr.getCallee())
                   .getMemberName();
  }
  bool calleeIsIntrinsic =
      calleeASTType == ASTNodeType::VARIABLE_REFERENCE && isIntrinsic(funcName);

  // Check argument count. A C-variadic callee fixes only its leading
  // parameters, so extra trailing arguments are allowed.
  bool calleeIsCVariadic = resolvedFunc && resolvedFunc->isCVariadic;
  bool badArgCount = calleeIsCVariadic ? args.size() < paramTypes.size()
                                       : args.size() != paramTypes.size();
  if (knownSignature && !calleeTakesPack && badArgCount) {
    logAndThrowError("Function '" + funcName + "' expects " +
                         (calleeIsCVariadic ? "at least " : "") +
                         std::to_string(paramTypes.size()) +
                         " arguments, got " + std::to_string(args.size()),
                     callExpr.getLocation());
  }

  checkPackedRefArguments(args, paramTypes);
  if (knownSignature) {
    checkArgumentPlaces(args, paramTypes, funcName, callExpr.getLocation());
  }

  // If we found a function via overload resolution, types are already
  // compatible Otherwise, check each argument type manually
  if (!resolvedFunc) {
    for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
      sun::TypePtr argType = args[i]->getResolvedType();
      sun::TypePtr paramType = paramTypes[i];

      // Try to coerce integer literal to parameter type
      if (tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                                  paramType)) {
        argType = paramType;  // Update for subsequent checks
      }

      if (argType && paramType && !paramType->equals(*argType)) {
        // Allow implicit conversions for compatible types
        bool compatible = false;

        // Reference parameter accepts the referenced type directly
        if (paramType->isReference()) {
          auto* refType =
              static_cast<const sun::ReferenceType*>(paramType.get());
          if (refType->getReferencedType()->equals(*argType)) {
            compatible = true;
          }
          // A borrow of the other mutability: only ref -> const ref
          if (argType->isReference()) {
            auto* argRef =
                static_cast<const sun::ReferenceType*>(argType.get());
            if (sun::refMutabilityConvertible(*argRef, *refType) &&
                refType->getReferencedType()->equals(
                    *argRef->getReferencedType())) {
              compatible = true;
            }
          }
          // ref array<T> (unsized) accepts any array<T, dims...>
          if (refType->getReferencedType()->isArray() && argType->isArray()) {
            auto* paramArray = static_cast<const sun::ArrayType*>(
                refType->getReferencedType().get());
            auto* argArray = static_cast<const sun::ArrayType*>(argType.get());
            if (paramArray->isUnsized() && paramArray->getElementType()->equals(
                                               *argArray->getElementType())) {
              compatible = true;
            }
          }
          // Auto-deref: raw_ptr<T> is compatible with ref T
          if (argType->isRawPointer()) {
            auto* ptrType =
                static_cast<const sun::RawPointerType*>(argType.get());
            if (ptrType->getPointeeType()->equals(
                    *refType->getReferencedType())) {
              compatible = true;
            }
          }
        }

        // Auto-deref: raw_ptr<T> can be passed where T or ref T is expected
        if (argType->isRawPointer() && !paramType->isRawPointer()) {
          auto* ptrType =
              static_cast<const sun::RawPointerType*>(argType.get());
          sun::TypePtr pointeeType = ptrType->getPointeeType();
          // For primitives, auto-deref to value is allowed
          if (pointeeType->equals(*paramType) && paramType->isPrimitive() &&
              !paramType->isReference()) {
            compatible = true;
          }
          // For any type, auto-deref to ref is allowed
          if (paramType->isReference()) {
            auto* refType =
                static_cast<const sun::ReferenceType*>(paramType.get());
            if (pointeeType->equals(*refType->getReferencedType())) {
              compatible = true;
            }
          }
        }

        // Null is compatible with any pointer type
        if (argType->isNullPointer() && paramType->isAnyPointer()) {
          compatible = true;
        }

        // Integer widening: smaller int types can be passed to larger int
        // params i8 -> i16 -> i32 -> i64, u8 -> u16 -> u32 -> u64
        if (!compatible && argType->isPrimitive() && paramType->isPrimitive()) {
          if ((argType->isInt8() || argType->isInt16() || argType->isInt32()) &&
              paramType->isInt64()) {
            compatible = true;
          } else if ((argType->isInt8() || argType->isInt16()) &&
                     paramType->isInt32()) {
            compatible = true;
          } else if (argType->isInt8() && paramType->isInt16()) {
            compatible = true;
          }
          // Unsigned widening
          else if ((argType->isUInt8() || argType->isUInt16() ||
                    argType->isUInt32()) &&
                   paramType->isUInt64()) {
            compatible = true;
          } else if ((argType->isUInt8() || argType->isUInt16()) &&
                     paramType->isUInt32()) {
            compatible = true;
          } else if (argType->isUInt8() && paramType->isUInt16()) {
            compatible = true;
          }
          // Float widening: f32 -> f64
          else if (argType->isFloat32() && paramType->isFloat64()) {
            compatible = true;
          }
        }

        // static_ptr<T> is compatible with raw_ptr<T>
        if (argType->isStaticPointer() && paramType->isRawPointer()) {
          auto* staticPtr =
              static_cast<const sun::StaticPointerType*>(argType.get());
          auto* rawPtr =
              static_cast<const sun::RawPointerType*>(paramType.get());
          if (staticPtr->getPointeeType()->equals(*rawPtr->getPointeeType())) {
            compatible = true;
          }
        }

        // raw_ptr<T> is compatible with byte pointers raw_ptr<i8>/raw_ptr<u8>
        // (like C's void*). Only for intrinsics (functions starting with '_')
        // to avoid accidental type erasure in user code
        if (argType->isRawPointer() && paramType->isRawPointer() &&
            calleeIsIntrinsic) {
          auto* paramRawPtr =
              static_cast<const sun::RawPointerType*>(paramType.get());
          if (paramRawPtr->getPointeeType()->isInt8() ||
              paramRawPtr->getPointeeType()->isUInt8()) {
            compatible = true;
          }
        }

        // Class-to-interface compatibility:
        // A class C can be passed where interface I is expected if C implements
        // I
        if (!compatible && isAssignableTo(argType, paramType)) {
          compatible = true;
        }

        if (!compatible) {
          // The common way to land here now: handing a borrowed element to a
          // by-value parameter. Say what to do about it.
          std::string hint;
          if (argType->isReference() && !paramType->isReference() &&
              !sun::typeCopiesByRead(paramType)) {
            hint = ". It is borrowed, and a '" + paramType->toDisplayString() +
                   "' cannot be read out of a borrow: take the parameter by "
                   "'ref', pass a clone(), or move the value out first "
                   "(take()/pop()/remove() on a container)";
          }
          logAndThrowError("Type mismatch in argument " +
                               std::to_string(i + 1) + " of call to '" +
                               funcName + "': expected " +
                               paramType->toDisplayString() + ", got " +
                               argType->toDisplayString() + hint,
                           callExpr.getLocation());
        }
      }
    }
  }

  // Check for error propagation: calling a throwing function or lambda
  // requires either being inside a try block or being in a function declared
  // with "throws IError"
  bool calleeThrows = resolvedFunc && resolvedFunc->canThrow;
  if (!calleeThrows) {
    sun::TypePtr calleeType = callExpr.getCallee()->getResolvedType();
    if (calleeType && calleeType->isLambda()) {
      calleeThrows =
          static_cast<const sun::LambdaType*>(calleeType.get())->canThrow();
    }
  }
  if (calleeThrows) {
    if (!ctx_.isInTryBlock() && !ctx_.isInThrowingFunction()) {
      logAndThrowError(
          "Call to throwing function '" + funcName +
              "' must be in a try block or in a function declared with ', "
              "IError'",
          callExpr.getLocation());
    }
  }

  // Record how each argument reaches its parameter. Codegen carries these
  // out and never compares Sun types at the call boundary itself.
  if (knownSignature) {
    callExpr.setArgConversions(sun::conversions::classifyArguments(
        callExpr.getResolvedArgTypes(), paramTypes, calleeIsCVariadic, funcName,
        callExpr.getLocation()));
  }

  // A borrow handed out by a method seen through an immutable receiver may
  // only be read through
  sun::TypePtr resultType = types_.inferType(callExpr);
  if (receiverImmutable) resultType = types_.createConstView(resultType);
  callExpr.setResolvedType(resultType);
}

// -------------------------------------------------------------------
// Intrinsic call analysis (e.g., _load<T>, _store<T>, _address_of<T>)
// -------------------------------------------------------------------

void SemanticAnalyzer::expandPackArguments(
    std::vector<std::unique_ptr<ExprAST>>& args) {
  auto* fnScope = ctx_.currentFunctionScope();
  if (!fnScope || !fnScope->variadicParam) return;
  const auto& [packName, types] = *fnScope->variadicParam;

  // Is there a pack expansion for this function's variadic param to expand?
  auto isPack = [&](const std::unique_ptr<ExprAST>& a) {
    return a->getType() == ASTNodeType::PACK_EXPANSION &&
           static_cast<const PackExpansionAST&>(*a).getPackName() == packName;
  };
  bool hasPack = false;
  for (const auto& a : args) {
    if (isPack(a)) {
      hasPack = true;
      break;
    }
  }
  if (!hasPack) return;

  // Rewrite `args...` into concrete, already-typed references to the elements
  // the pack was materialized as ("args.0", "args.1", ...). Other args pass
  // through unchanged.
  std::vector<std::unique_ptr<ExprAST>> rebuilt;
  rebuilt.reserve(args.size() + types.size());
  for (auto& a : args) {
    if (isPack(a)) {
      for (size_t i = 0; i < types.size(); ++i) {
        auto vref = std::make_unique<VariableReferenceAST>(packName + "." +
                                                           std::to_string(i));
        vref->setResolvedType(types[i]);
        rebuilt.push_back(std::move(vref));
      }
    } else {
      rebuilt.push_back(std::move(a));
    }
  }
  args = std::move(rebuilt);
}

void SemanticAnalyzer::analyzeIntrinsicCall(GenericCallAST& genericCall) {
  // Intrinsics are handled at codegen time - just analyze arguments
  for (const auto& arg : genericCall.getArgs()) {
    analyzeExpr(const_cast<ExprAST&>(*arg));
  }
  // Expand any variadic pack into concrete typed args (e.g. _init<T>(p,
  // args...))
  expandPackArguments(genericCall.getArgsMutable());
  genericCall.setResolvedType(types_.inferGenericCallType(genericCall));

  // _spawn is the one intrinsic that passes its arguments on to something
  // else — the spawned lambda — so, like any other call, how each argument
  // reaches its parameter is decided here rather than in codegen. A compound
  // argument moves: the thread owns it from the moment it starts.
  if (genericCall.getFunctionName() == "_spawn") {
    recordSpawnArgumentConversions(genericCall);
  }
}

// The lambda is argument 0 and is taken apart rather than passed on, so it
// stands in for itself; everything after it fills the lambda's parameters.
void SemanticAnalyzer::recordSpawnArgumentConversions(
    GenericCallAST& genericCall) {
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  auto* lambda = typeArgs.empty()
                     ? nullptr
                     : sun::tryGetType<sun::LambdaType>(typeArgs[0]);
  if (!lambda) {
    logAndThrowError("_spawn<F> requires a lambda type argument",
                     genericCall.getLocation());
  }

  const auto& args = genericCall.getArgs();
  std::vector<sun::TypePtr> paramTypes{typeArgs[0]};
  for (const auto& param : lambda->getParamTypes()) {
    paramTypes.push_back(param);
  }
  if (args.size() != paramTypes.size()) {
    logAndThrowError(
        "_spawn<F> takes the lambda and one argument per parameter it "
        "declares: " +
            std::to_string(paramTypes.size()) + " in all, got " +
            std::to_string(args.size()),
        genericCall.getLocation());
  }

  std::vector<sun::TypePtr> argTypes;
  for (const auto& arg : args) {
    argTypes.push_back(arg->getResolvedType());
    // Whatever the thread takes over cannot be used here afterwards.
    checkMoveSource(*arg, genericCall.getLocation());
  }
  genericCall.setArgConversions(sun::conversions::classifyArguments(
      argTypes, paramTypes, /*cVariadic=*/false, "_spawn",
      genericCall.getLocation()));
}

// -------------------------------------------------------------------
// Generic function call analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeGenericFunctionCall(GenericCallAST& genericCall) {
  const std::string& funcName = genericCall.getFunctionName();
  const auto& args = genericCall.getArgs();

  // Resolve the function name through using imports
  sun::QualifiedName resolved = ctx_.resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  auto* genFuncInfo = ctx_.lookupGenericFunction(lookupName);
  if (!genFuncInfo) {
    logAndThrowError("Unknown generic function '" + funcName + "'",
                     genericCall.getLocation());
  }

  // Store the generic function AST on the call node for codegen
  genericCall.setGenericFunctionAST(genFuncInfo->AST);

  // The callee's own signature, which says whether it ends in a pack.
  const PrototypeAST* calleeProto =
      genFuncInfo->AST ? &genFuncInfo->AST->getProto() : nullptr;
  bool calleeTakesPack = calleeProto && calleeProto->hasVariadicParam();

  // A call may name only the leading type parameters — `f<i32>(x)` for
  // `f<T, U>` — and leave the rest to the arguments, as a call with no type
  // arguments does. That needs the argument types first, and so does a pack:
  // its element types are what the specialization is keyed on.
  bool argsAnalyzed = false;
  std::vector<sun::TypePtr> argTypes;
  if (calleeTakesPack || genericCall.getResolvedTypeArgs().size() <
                             genFuncInfo->typeParameters.size()) {
    for (const auto& arg : args) {
      analyzeExpr(const_cast<ExprAST&>(*arg));
    }
    // One template forwarding its own pack into another: `g<T>(args...)`.
    if (calleeTakesPack) expandPackArguments(genericCall.getArgsMutable());
    for (const auto& arg : args) {
      argTypes.push_back(arg->getResolvedType());
    }
    argsAnalyzed = true;
    if (genericCall.getResolvedTypeArgs().size() <
        genFuncInfo->typeParameters.size()) {
      std::vector<sun::TypePtr> given = genericCall.getResolvedTypeArgs();
      genericCall.setResolvedTypeArgs(sun::generics::inferGenericTypeArguments(
          *genFuncInfo, argTypes, funcName, genericCall.getLocation(), given));
    }
  }
  const auto& typeArgs = genericCall.getResolvedTypeArgs();

  // Everything past the fixed parameters fills the pack.
  std::optional<std::vector<sun::TypePtr>> packArgTypes;
  if (calleeTakesPack) {
    packArgTypes = generics_.splitPackArgTypes(*calleeProto, argTypes, funcName,
                                               genericCall.getLocation());
  }

  // Try to get expected parameter types for array literal type propagation
  // Only instantiate if all type arguments are concrete (not type parameters)
  // If we're inside a generic function and T is still a type parameter,
  // we can't create a real specialization yet - it will be created when
  // the outer generic function is instantiated with concrete types.
  std::vector<sun::TypePtr> expectedParamTypes;
  bool allConcrete = !generics_.templateStillAbstract(*genFuncInfo, typeArgs);
  if (allConcrete) {
    SpecializedFunctionInfo specializedFunc =
        generics_.requireGenericSpecialization(*genFuncInfo, typeArgs, funcName,
                                               genericCall.getLocation(),
                                               packArgTypes);
    // Fixed parameters followed by the pack's elements, so the checks below
    // line up positionally with the expanded argument list.
    expectedParamTypes = specializedFunc.paramTypes;
    // Record the name so codegen calls exactly what was instantiated
    genericCall.setSpecializationName(specializedFunc.qualifiedName);
  }

  // Propagate expected types to array literal arguments before analysis
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    if (args[i]->getType() == ASTNodeType::ARRAY_LITERAL) {
      sun::TypePtr paramType = expectedParamTypes[i];
      // Handle ref array<T> -> array<T>
      if (paramType && paramType->isReference()) {
        auto* refType = static_cast<const sun::ReferenceType*>(paramType.get());
        paramType = refType->getReferencedType();
      }
      if (paramType && paramType->isArray()) {
        const_cast<ExprAST&>(*args[i]).setResolvedType(paramType);
      }
    }
  }

  // Analyze all arguments
  if (!argsAnalyzed) {
    for (const auto& arg : args) {
      analyzeExpr(const_cast<ExprAST&>(*arg));
    }
  }

  // Coerce integer literals to the instantiated parameter types (there is
  // exactly one signature, so a non-fitting literal is a hard error)
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                            expectedParamTypes[i], /*throwOnFail=*/true);
  }
  checkArgumentPlaces(args, expectedParamTypes, funcName,
                      genericCall.getLocation());

  if (allConcrete) {
    std::vector<sun::TypePtr> argTypes;
    for (const auto& arg : args) argTypes.push_back(arg->getResolvedType());
    genericCall.setArgConversions(sun::conversions::classifyArguments(
        argTypes, expectedParamTypes, /*cVariadic=*/false, funcName,
        genericCall.getLocation()));
  }

  genericCall.setResolvedType(types_.inferGenericCallType(genericCall));
}

// -------------------------------------------------------------------
// Generic class construction analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeGenericClassConstruction(
    GenericCallAST& genericCall) {
  const std::string& funcName = genericCall.getFunctionName();
  const auto& args = genericCall.getArgs();
  const auto& typeArgs = genericCall.getResolvedTypeArgs();

  // Resolve the class name through using imports
  sun::QualifiedName resolved = ctx_.resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  auto* genericClassInfo = ctx_.lookupGenericClass(lookupName);
  if (!genericClassInfo) {
    logAndThrowError("Unknown generic class '" + funcName + "'",
                     genericCall.getLocation());
  }

  // Instantiate the generic class to get init method parameters
  std::vector<sun::TypePtr> expectedParamTypes;
  auto specializedClass =
      generics_.instantiateGenericClass(lookupName, typeArgs);
  if (specializedClass) {
    if (auto* initMethod = ctx_.accessibleMethod(*specializedClass, "init",
                                                 genericCall.getLocation())) {
      expectedParamTypes = initMethod->paramTypes;
    }
  }

  // Propagate expected types to array literal arguments before analysis
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    if (args[i]->getType() == ASTNodeType::ARRAY_LITERAL) {
      sun::TypePtr paramType = expectedParamTypes[i];
      // Handle ref array<T> -> array<T>
      if (paramType && paramType->isReference()) {
        auto* refType = static_cast<const sun::ReferenceType*>(paramType.get());
        paramType = refType->getReferencedType();
      }
      if (paramType && paramType->isArray()) {
        const_cast<ExprAST&>(*args[i]).setResolvedType(paramType);
      }
    }
  }

  // Analyze all arguments
  for (const auto& arg : args) {
    analyzeExpr(const_cast<ExprAST&>(*arg));
  }

  // Pick the init overload from the argument types, exactly as for a
  // non-generic class. Without this, a call with the wrong argument count
  // would silently skip the constructor in codegen.
  if (specializedClass) {
    std::vector<sun::TypePtr> argTypes;
    for (const auto& arg : args) argTypes.push_back(arg->getResolvedType());
    const auto* initMethod = ctx_.accessibleMethodForArgs(
        *specializedClass, "init", argTypes, genericCall.getLocation());
    if (initMethod) {
      expectedParamTypes = initMethod->paramTypes;
    } else if (!specializedClass->getMethod("init") && !args.empty()) {
      logAndThrowError(
          "Class '" + specializedClass->toDisplayString() +
              "' declares no 'init', so it cannot be constructed positionally."
              " Use a struct literal naming each field.",
          genericCall.getLocation());
    } else if (specializedClass->getMethod("init")) {
      std::string argList;
      for (size_t i = 0; i < argTypes.size(); ++i) {
        if (i > 0) argList += ", ";
        argList += argTypes[i] ? argTypes[i]->toDisplayString() : "?";
      }
      logAndThrowError("No matching constructor for '" +
                           specializedClass->toDisplayString() +
                           "' with arguments (" + argList + ")",
                       genericCall.getLocation());
    }
  }

  // Coerce integer literals to the init method's parameter types
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                            expectedParamTypes[i], /*throwOnFail=*/true);
  }

  if (specializedClass) {
    checkArgumentPlaces(args, expectedParamTypes,
                        specializedClass->toDisplayString() + ".init",
                        genericCall.getLocation());
    std::vector<sun::TypePtr> argTypes;
    for (const auto& arg : args) argTypes.push_back(arg->getResolvedType());
    genericCall.setArgConversions(sun::conversions::classifyArguments(
        argTypes, expectedParamTypes, /*cVariadic=*/false,
        specializedClass->toDisplayString() + ".init",
        genericCall.getLocation()));
  }

  genericCall.setResolvedType(types_.inferGenericCallType(genericCall));
}

// -------------------------------------------------------------------
// Payload enums
