// analysis.cpp — Main analysis entry points for semantic analyzer

#include <algorithm>
#include <cassert>
#include <set>

#include "codegen/abi/c_abi_types.h"
#include "semantic_analysis/field_initialization.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_rules.h"
#include "semantic_analysis/visibility.h"
#include "support/config.h"
#include "support/error.h"

using sun::semantic_analysis::ClassMethod;
using sun::semantic_analysis::LambdaType;
using sun::semantic_analysis::TypePtr;
using sun::semantic_analysis::Types;

using sun::ast::ASTNodeType;
using sun::ast::BlockExprAST;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::ast::IndexAST;
using sun::ast::MemberAccessAST;
using sun::ast::MemberAssignmentAST;
using sun::ast::PrototypeAST;
using sun::ast::TernaryExprAST;
using sun::support::logAndThrowError;
using sun::support::Position;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

using sun::semantic_analysis::isAssignableTo;
using sun::semantic_analysis::isBorrowableLvalue;
using sun::semantic_analysis::isReservedIdentifier;
using sun::semantic_analysis::methodVisibility;
using sun::semantic_analysis::tryCoerceIntegerLiteral;
using sun::semantic_analysis::unwrapRef;

// -------------------------------------------------------------------
// Borrow targets
// -------------------------------------------------------------------

void SemanticAnalyzer::rejectBorrowOfByValueCapture(const ExprAST& target,
                                                    const Position& loc) {
  if (target.getType() != ASTNodeType::VARIABLE_REFERENCE) return;
  const auto& varRef =
      static_cast<const sun::ast::VariableReferenceAST&>(target);
  VariableInfo* varInfo = ctx_.currentScope().lookupVariable(varRef.getName());
  if (!varInfo || varInfo->captureKind != sun::ast::CaptureKind::ByValue)
    return;
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
    auto baseType = sun::semantic_analysis::unwrapRef(
        indexExpr.getTarget()->getResolvedType());
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

void SemanticAnalyzer::analyzeExpr(ExprAST& expr, TypePtr expectedType) {
  SemanticContext::SourceFileGuard sourceFile(ctx_, expr.getSourceFileId());
  SemanticContext::LocationGuard locationGuard(ctx_, expr.getLocation());
  sun::semantic_analysis::assignLocalDeclarationName(
      expr, ctx_.getCurrentScopePath());
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
      analyzeStructLiteral(static_cast<sun::ast::StructLiteralAST&>(expr),
                           expectedType);
      break;
    }

    case ASTNodeType::ARRAY_LITERAL:
      analyzeArrayLiteral(static_cast<sun::ast::ArrayLiteralAST&>(expr),
                          expectedType);
      break;

    case ASTNodeType::INDEX:
      analyzeIndexExpr(static_cast<IndexAST&>(expr));
      break;

    case ASTNodeType::SLICE:
      analyzeSliceExpr(expr);
      break;

    case ASTNodeType::VARIABLE_REFERENCE: {
      if (expr.getModuleDeclaration()) {
        expr.setResolvedType(types_.inferType(expr));
        break;
      }
      auto& varRef = static_cast<sun::ast::VariableReferenceAST&>(expr);

      // An expected function-pointer type selects one overload without
      // changing ordinary call-site overload resolution.
      if (expectedType && expectedType->isFunction() &&
          !ctx_.currentScope().lookupVariable(varRef.getName())) {
        sun::semantic_analysis::QualifiedName resolved =
            ctx_.resolveNameWithUsings(varRef.getName());
        std::vector<FunctionInfo> matches;
        for (const auto& candidate : ctx_.getAllFunctions(resolved.baseName)) {
          auto candidateType = Types::Function(
              candidate.returnType, candidate.paramTypes, candidate.canThrow);
          if (isAssignableTo(candidateType, expectedType)) {
            matches.push_back(candidate);
          }
        }
        if (matches.size() == 1) {
          const FunctionInfo& match = matches.front();
          expr.setResolvedType(Types::Function(
              match.returnType, match.paramTypes, match.canThrow));
          varRef.setQualifiedName(match.qualifiedName);
          varRef.setTargetDeclarationId(match.declarationId);
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
      sun::semantic_analysis::QualifiedName resolved =
          ctx_.resolveNameWithUsings(varRef.getName());
      varRef.setQualifiedName(resolved);
      if (VariableInfo* info =
              ctx_.currentScope().lookupVariable(varRef.getName())) {
        varRef.setTargetDeclarationId(info->declarationId);
        checkExternVariableAccessAllowed(*info, resolved.display(),
                                         varRef.getLocation());
      }
      break;
    }

    case ASTNodeType::VARIABLE_CREATION:
      analyzeVariableCreation(
          static_cast<sun::ast::VariableCreationAST&>(expr));
      break;

    case ASTNodeType::VARIABLE_ASSIGNMENT:
      analyzeVariableAssignment(
          static_cast<sun::ast::VariableAssignmentAST&>(expr));
      break;

    case ASTNodeType::COMPOUND_ASSIGNMENT:
      analyzeCompoundAssignment(
          static_cast<sun::ast::CompoundAssignmentAST&>(expr));
      break;

    case ASTNodeType::REFERENCE_CREATION:
      analyzeReferenceCreation(
          static_cast<sun::ast::ReferenceCreationAST&>(expr));
      break;

    case ASTNodeType::FUNCTION:
      analyzeFunctionDefinition(static_cast<FunctionAST&>(expr));
      break;

    case ASTNodeType::LAMBDA:
      analyzeLambdaExpr(static_cast<sun::ast::LambdaAST&>(expr));
      break;

    case ASTNodeType::BLOCK: {
      auto& block = static_cast<BlockExprAST&>(expr);
      ctx_.enterScope();
      bodies_.analyzeBlock(block);
      expr.setResolvedType(types_.inferType(expr));
      ctx_.exitScope();
      break;
    }

    case ASTNodeType::IF:
      analyzeIfExpr(static_cast<sun::ast::IfExprAST&>(expr));
      break;

    case ASTNodeType::MATCH:
      analyzeMatchExpr(static_cast<sun::ast::MatchExprAST&>(expr),
                       expectedType);
      break;

    case ASTNodeType::TERNARY:
      analyzeTernaryExpr(static_cast<TernaryExprAST&>(expr), expectedType);
      break;

    case ASTNodeType::FOR_LOOP:
      analyzeForLoop(static_cast<sun::ast::ForExprAST&>(expr));
      break;

    case ASTNodeType::FOR_IN_LOOP:
      analyzeForInLoop(static_cast<sun::ast::ForInExprAST&>(expr));
      break;

    case ASTNodeType::WHILE_LOOP: {
      auto& whileExpr = static_cast<sun::ast::WhileExprAST&>(expr);
      analyzeExpr(const_cast<ExprAST&>(*whileExpr.getCondition()));
      analyzeExpr(const_cast<ExprAST&>(*whileExpr.getBody()));
      expr.setResolvedType(Types::Float64());  // while loops return 0.0
      break;
    }

    case ASTNodeType::BINARY:
      analyzeBinaryExpr(static_cast<sun::ast::BinaryExprAST&>(expr),
                        expectedType);
      break;

    case ASTNodeType::UNARY:
      analyzeUnaryExpr(static_cast<sun::ast::UnaryExprAST&>(expr));
      break;

    case ASTNodeType::CALL: {
      auto& callExpr = static_cast<sun::ast::CallExprAST&>(expr);
      calls_.analyzeCall(callExpr, expectedType);
      break;
    }

    case ASTNodeType::INDEXED_ASSIGNMENT:
      analyzeIndexedAssignment(
          static_cast<sun::ast::IndexedAssignmentAST&>(expr));
      break;

    case ASTNodeType::RETURN:
      analyzeReturnExpr(static_cast<sun::ast::ReturnExprAST&>(expr));
      break;

    case ASTNodeType::MODULE:
      analyzeModuleDefinition(static_cast<sun::ast::ModuleAST&>(expr));
      break;

    case ASTNodeType::MOON_SCOPE:
      analyzeMoonScope(expr);
      break;

    case ASTNodeType::USING: {
      pipeline_.declarations().registerUsing(
          static_cast<sun::ast::UsingAST&>(expr));
      expr.setResolvedType(Types::Void());
      break;
    }

    case ASTNodeType::QUALIFIED_NAME:
      if (expr.getModuleDeclaration())
        expr.setResolvedType(types_.inferType(expr));
      else
        analyzeQualifiedName(static_cast<sun::ast::QualifiedNameAST&>(expr));
      break;

    case ASTNodeType::CLASS_DEFINITION:
      analyzeClassDefinition(static_cast<sun::ast::ClassDefinitionAST&>(expr));
      break;

    case ASTNodeType::INTERFACE_DEFINITION:
      analyzeInterfaceDefinition(
          static_cast<sun::ast::InterfaceDefinitionAST&>(expr));
      break;

    case ASTNodeType::ENUM_DEFINITION: {
      enums_.analyzeEnumDefinition(
          static_cast<sun::ast::EnumDefinitionAST&>(expr));
      break;
    }

    case ASTNodeType::THIS: {
      expr.setResolvedType(types_.inferType(expr));
      break;
    }

    case ASTNodeType::MEMBER_ACCESS:
      if (expr.getModuleDeclaration())
        expr.setResolvedType(types_.inferType(expr));
      else
        analyzeMemberAccess(static_cast<MemberAccessAST&>(expr), expectedType);
      break;

    case ASTNodeType::MEMBER_ASSIGNMENT:
      analyzeMemberAssignment(static_cast<MemberAssignmentAST&>(expr));
      break;

    case ASTNodeType::TRY_CATCH:
      analyzeTryCatch(static_cast<sun::ast::TryCatchExprAST&>(expr));
      break;

    case ASTNodeType::UNSAFE_BLOCK:
      analyzeUnsafeBlock(static_cast<sun::ast::UnsafeBlockAST&>(expr));
      break;

    case ASTNodeType::THROW:
      analyzeThrowExpr(static_cast<sun::ast::ThrowExprAST&>(expr));
      break;

    case ASTNodeType::GENERIC_CALL:
      calls_.analyzeGenericCall(static_cast<sun::ast::GenericCallAST&>(expr));
      break;

    case ASTNodeType::PACK_EXPANSION: {
      // Pack expansion is handled at codegen time
      // Just set the resolved type for now
      expr.setResolvedType(Types::Void());
      break;
    }

    case ASTNodeType::DECLARE_TYPE:
      analyzeDeclareType(static_cast<sun::ast::DeclareTypeAST&>(expr));
      break;

    default:
      break;
  }
}

// -------------------------------------------------------------------
// Parameter names and types
// -------------------------------------------------------------------

std::vector<TypePtr> SemanticAnalyzer::validateAndResolveParamTypes(
    PrototypeAST& proto, std::optional<Position> loc,
    bool allowByValueObjects) {
  // Validate parameter names
  for (const auto& argName : proto.getArgNames()) {
    validateNotReserved(argName, "Parameter name", loc);
  }

  // Resolve parameter types
  std::vector<TypePtr> paramTypes;
  for (auto& [argName, argType] : proto.getMutableArgs()) {
    TypePtr paramType = types_.typeAnnotationToType(argType);

    // Check for compound types being passed by value
    if constexpr (sun::support::Config::REQUIRE_REF_FOR_COMPOUND_PARAMS) {
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
// Resolve function signatures and record their emitted symbols
// -------------------------------------------------------------------

FunctionInfo SemanticAnalyzer::getFunctionInfo(FunctionAST& func) {
  SemanticContext::SourceFileGuard sourceFile(ctx_, func.getSourceFileId());
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Only declared generic parameters may remain unresolved in a signature.
  SemanticContext::ScopeSwitchGuard signatureScope(ctx_, ctx_.scope());
  if (!proto.getTypeParameters().empty()) {
    std::vector<TypePtr> parameters;
    for (size_t i = 0; i < proto.getTypeParameters().size(); ++i) {
      parameters.push_back(proto.getTypeParameters()[i].toSunType(
          ctx_.types()->declarations,
          proto.declarationIdentity().typeParameters.at(i)));
    }
    ctx_.enterTypeParamScope(proto.getTypeParameterNames(), parameters);
  }

  // Validate function name (if named function, not lambda)
  if (!proto.getName().empty()) {
    validateNotReserved(proto.getName(), "Function name", func.getLocation());
  }

  std::vector<sun::ast::Capture> captures;

  // Validate and resolve parameter types. Only C externs may take objects by
  // value; see validateAndResolveParamTypes.
  std::vector<TypePtr> paramTypes = validateAndResolveParamTypes(
      proto, func.getLocation(), /*allowByValueObjects=*/func.isCExtern());

  // Resolve return type if specified; Void for constructors (no return type)
  TypePtr returnType = Types::Void();
  if (proto.hasReturnType()) {
    returnType = types_.typeAnnotationToType(*proto.getReturnType());
    if (!returnType) {
      logAndThrowError("Failed to resolve return type for function '" +
                           proto.getName() + "'",
                       func.getLocation());
    }
  }

  assert(proto.hasQualifiedName() &&
         "Function declaration must be named first");
  const auto& qualifiedName = proto.getQualifiedName();

  FunctionInfo info;
  info.returnType = returnType;
  info.paramTypes = std::move(paramTypes);
  info.captures = std::move(captures);
  info.qualifiedName = qualifiedName;
  info.declarationId = proto.getDeclarationId();
  info.canThrow = proto.canThrow();
  info.isCVariadic = proto.isCVariadic();
  info.isCExtern = func.isCExtern();
  info.isForwardDeclaration =
      func.isExtern() && !func.isCExtern() && !func.isPrecompiled();
  info.visibility = func.getVisibility();
  return info;
}

// -------------------------------------------------------------------
// Apply FunctionInfo to prototype
// -------------------------------------------------------------------

void SemanticAnalyzer::applyFunctionInfoToProto(PrototypeAST& proto,
                                                const FunctionInfo& info) {
  proto.setQualifiedName(info.qualifiedName);
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

void SemanticAnalyzer::analyzePartialClass(
    sun::ast::ClassDefinitionAST& classDef, ExprAST& expr) {
  const std::string& baseName = classDef.getName();

  auto existingClass = ctx_.lookupClass(baseName);
  if (existingClass) {
    classDef.setTargetDeclarationId(existingClass->getDeclarationId());
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
      method.declarationId = proto.getDeclarationId();
      if (proto.getName() == "deinit")
        existingClass->deinitializer = method.declarationId;
      method.visibility = methodVisibility(*methodDecl.function);
      method.isConst = methodDecl.isConst;
      method.isUnsafe = methodDecl.function->getProto().isUnsafeMethod();
      std::string methodNameForScope = proto.getName();
      std::vector<TypePtr> methodParamTypes;
      methodParamTypes.push_back(existingClass);
      for (const auto& pt : methodInfo.paramTypes) {
        methodParamTypes.push_back(pt);
      }
      methodInfo.paramTypes = std::move(methodParamTypes);
      ctx_.currentScope().declareFunction(methodNameForScope, methodInfo,
                                          ctx_.currentLocation());
    }

    // Analyze extension method bodies
    for (const auto& methodDecl : classDef.getMethods()) {
      bodies_.analyzeFunction(*methodDecl.function);
    }
    // The parser rejects constructors in a partial class, so this is only a
    // backstop — and like the primary path it runs after every body is
    // analyzed, since the walk follows calls into them
    if (!classDef.isPrecompiled()) {
      for (const auto& methodDecl : classDef.getMethods()) {
        if (!methodDecl.isConstructor) continue;
        sun::semantic_analysis::checkFieldInitialization(
            *methodDecl.function, *existingClass, classDef.getMethods());
      }
    }

    ctx_.exitScope();  // Class scope

    // Merge methods into primary AST so codegen generates them
    for (auto* s = ctx_.scope(); s != nullptr; s = s->parent) {
      auto it = std::find_if(s->classDefinitions.begin(),
                             s->classDefinitions.end(), [&](const auto& entry) {
                               return entry.second->getDeclarationId() ==
                                      classDef.getTargetDeclarationId();
                             });
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
    ctx_.declarations().deferExtension(baseName, &classDef);
  }
  expr.setResolvedType(Types::Void());
}

// -------------------------------------------------------------------
// Function body analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeStructLiteral(sun::ast::StructLiteralAST& literal,
                                            const TypePtr& expectedType) {
  if (!expectedType || !expectedType->isClass()) {
    logAndThrowError(
        "A '{ field: value }' literal needs a known class type. Annotate the "
        "target, as in `var x: MyClass = { ... };`.",
        literal.getLocation());
    return;
  }

  auto* classType =
      static_cast<sun::semantic_analysis::ClassType*>(expectedType.get());

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

  literal.resolvedFields().clear();
  std::set<std::string> seen;
  for (auto& field : literal.getMutableFields()) {
    const sun::semantic_analysis::ClassField* classField =
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

    literal.resolvedFields().push_back(classField->declarationId);
    analyzeExpr(*field.value, classField->type);
    TypePtr valueType = field.value->getResolvedType();
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
    MemberAssignmentAST& assign,
    const sun::semantic_analysis::Type& objectType) {
  const auto& moduleType =
      static_cast<const sun::semantic_analysis::ModuleType&>(objectType);
  const std::string& modPath = moduleType.getModulePath();
  const std::string& memberName = assign.getMemberName();

  SymbolMatch match = ctx_.findSymbolInModule(modPath, memberName);
  if (!match) {
    logAndThrowError("Unknown member '" + memberName + "' in module '" +
                         sun::semantic_analysis::displayModulePath(modPath) +
                         "'",
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
  if (sun::semantic_analysis::isConstRef(target.type)) {
    logAndThrowError("Cannot assign through const reference '" + full + "'",
                     assign.getLocation());
  }

  // The declaration's own qualified name is the symbol codegen emitted the
  // global under, so that is what the write is pointed at
  assign.setQualifiedName(target.qualifiedName);
  assign.setTargetDeclarationId(target.declarationId);

  TypePtr expectedType = unwrapRef(target.type);
  analyzeExpr(const_cast<ExprAST&>(*assign.getValue()), expectedType);
  checkMoveSource(*assign.getValue(), assign.getLocation());

  TypePtr rhsType = assign.getValue()->getResolvedType();
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

  auto describe = [](const TypePtr& t) {
    return t ? t->toDisplayString() : std::string("<unresolved>");
  };

  auto validateCallback = [&](const sun::semantic_analysis::FunctionType&
                                  callback,
                              const std::string& paramName) {
    if (callback.canThrow()) {
      logAndThrowError("C callback parameter '" + paramName + "' cannot throw",
                       func.getLocation());
    }
    for (const auto& callbackParam : callback.getParamTypes()) {
      if (!sun::codegen::abi::isCallbackParameter(callbackParam)) {
        logAndThrowError("C callback parameter '" + paramName +
                             "' has unsupported callback argument type '" +
                             describe(callbackParam) + "'",
                         func.getLocation());
      }
    }
    if (!sun::codegen::abi::isCallbackReturn(callback.getReturnType())) {
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
      if (auto* callback = sun::codegen::support::tryGetType<
              sun::semantic_analysis::FunctionType>(params[i])) {
        validateCallback(*callback, proto.getArgs()[i].first);
      }
      if (!sun::codegen::abi::isValue(params[i])) {
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
      !sun::codegen::abi::isReturn(proto.getResolvedReturnType())) {
    logAndThrowError(
        "Extern function '" + proto.getName() + "' returns '" +
            describe(proto.getResolvedReturnType()) +
            "', which has no C equivalent. Extern return types must be a "
            "primitive, an enum, raw_ptr<T>, or a class. Note that `ref T` "
            "cannot be returned; use raw_ptr<T>.",
        func.getLocation());
  }
}

// An anonymous `<'_>` lambda type cannot be a return type: which frame the
// returned value's environment lives in cannot be told apart from the frame
// that is dying. A NAMED lifetime unpins it - 'function pick<'a>(...)
// <'a>() => i32' ties the result to frames the caller can see - so
// declarations that bind their own lifetimes pass allowNamed.
void SemanticAnalyzer::rejectRefEnvReturnType(
    const std::optional<sun::ast::TypeAnnotation>& returnType,
    const Position& location, bool allowNamed) {
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
void SemanticAnalyzer::checkAnnotationLifetimes(
    const sun::ast::TypeAnnotation& annot, const Position& location) {
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
                      [&](const sun::ast::LifetimeParameter& other) {
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

// -------------------------------------------------------------------
// Lambda signature extraction (pure computation, no side effects)
// -------------------------------------------------------------------

FunctionInfo SemanticAnalyzer::getLambdaInfo(sun::ast::LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  // Build captures using current scope information
  std::vector<sun::ast::Capture> captures = buildCaptures(lambda);

  // Validate and resolve parameter types
  std::vector<TypePtr> paramTypes = validateAndResolveParamTypes(proto);

  // Resolve return type (Sun requires return type annotations on lambdas,
  // parser enforces this, but check defensively)
  TypePtr returnType = Types::Void();
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
// Type parameter validation
// -------------------------------------------------------------------

void SemanticAnalyzer::validateTypeParameter(const TypePtr& type,
                                             const ExprAST& node) {
  if (!type || !type->isTypeParameter()) return;

  auto* typeParam =
      static_cast<const sun::semantic_analysis::TypeParameterType*>(type.get());

  // Type traits (_Integer, _Float, etc.) are not scope-bound type parameters
  if (sun::semantic_analysis::isTypeTrait(typeParam->getName())) return;

  TypePtr found = ctx_.findTypeParameter(typeParam->getName());
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
      auto& bin = static_cast<sun::ast::BinaryExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*bin.getLHS()));
      clearResolvedTypes(const_cast<ExprAST&>(*bin.getRHS()));
      break;
    }
    case ASTNodeType::UNARY: {
      auto& unary = static_cast<sun::ast::UnaryExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*unary.getOperand()));
      break;
    }
    case ASTNodeType::CALL: {
      auto& call = static_cast<sun::ast::CallExprAST&>(expr);
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
      auto& vc = static_cast<sun::ast::VariableCreationAST&>(expr);
      if (vc.getValue()) {
        clearResolvedTypes(const_cast<ExprAST&>(*vc.getValue()));
      }
      break;
    }
    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      auto& va = static_cast<sun::ast::VariableAssignmentAST&>(expr);
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
      auto& ia = static_cast<sun::ast::IndexedAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ia.getTarget()));
      clearResolvedTypes(const_cast<ExprAST&>(*ia.getValue()));
      break;
    }
    case ASTNodeType::COMPOUND_ASSIGNMENT: {
      auto& ca = static_cast<sun::ast::CompoundAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ca.getTarget()));
      clearResolvedTypes(const_cast<ExprAST&>(*ca.getValue()));
      break;
    }
    case ASTNodeType::IF: {
      auto& ifExpr = static_cast<sun::ast::IfExprAST&>(expr);
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
      auto& loop = static_cast<sun::ast::ForExprAST&>(expr);
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
      auto& loop = static_cast<sun::ast::ForInExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getIterable()));
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getBody()));
      break;
    }
    case ASTNodeType::WHILE_LOOP: {
      auto& loop = static_cast<sun::ast::WhileExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getCondition()));
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getBody()));
      break;
    }
    case ASTNodeType::RETURN: {
      auto& ret = static_cast<sun::ast::ReturnExprAST&>(expr);
      if (ret.hasValue()) {
        clearResolvedTypes(const_cast<ExprAST&>(*ret.getValue()));
      }
      break;
    }
    case ASTNodeType::REFERENCE_CREATION: {
      auto& ref = static_cast<sun::ast::ReferenceCreationAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ref.getTarget()));
      break;
    }
    case ASTNodeType::GENERIC_CALL: {
      auto& gc = static_cast<sun::ast::GenericCallAST&>(expr);
      for (const auto& arg : gc.getArgs()) {
        clearResolvedTypes(const_cast<ExprAST&>(*arg));
      }
      break;
    }
    case ASTNodeType::TRY_CATCH: {
      auto& tc = static_cast<sun::ast::TryCatchExprAST&>(expr);
      clearResolvedTypes(const_cast<BlockExprAST&>(tc.getTryBlock()));
      for (const auto& clause : tc.getCatchClauses()) {
        clearResolvedTypes(*clause.body);
      }
      break;
    }
    case ASTNodeType::UNSAFE_BLOCK: {
      auto& ub = static_cast<sun::ast::UnsafeBlockAST&>(expr);
      clearResolvedTypes(ub.getBody());
      break;
    }
    case ASTNodeType::THROW: {
      auto& th = static_cast<sun::ast::ThrowExprAST&>(expr);
      if (th.hasErrorExpr()) {
        clearResolvedTypes(const_cast<ExprAST&>(th.getErrorExpr()));
      }
      break;
    }
    case ASTNodeType::ARRAY_LITERAL: {
      auto& arr = static_cast<sun::ast::ArrayLiteralAST&>(expr);
      for (const auto& elem : arr.getElements()) {
        clearResolvedTypes(const_cast<ExprAST&>(*elem));
      }
      break;
    }
    case ASTNodeType::MATCH: {
      auto& match = static_cast<sun::ast::MatchExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*match.getDiscriminant()));
      for (auto& arm : match.getArmsMutable()) {
        if (arm.pattern) {
          clearResolvedTypes(*arm.pattern);
        }
        arm.resolvedVariantTag = -1;
        for (auto& binding : arm.bindings) {
          binding.resolvedType = nullptr;
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
// Call expression analysis
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// Bound method references: obj.method in value position
// -------------------------------------------------------------------

void SemanticAnalyzer::maybeResolveBoundMethodRef(MemberAccessAST& memberAccess,
                                                  TypePtr expectedType) {
  TypePtr objectType = unwrapRef(memberAccess.getObject()->getResolvedType());
  if (!objectType) return;

  // Unwrap raw_ptr<Class> / static_ptr<Class> (mirrors inferType)
  if (objectType->isRawPointer()) {
    TypePtr pointee =
        static_cast<sun::semantic_analysis::RawPointerType*>(objectType.get())
            ->getPointeeType();
    if (pointee && pointee->isClass()) objectType = pointee;
  } else if (objectType->isStaticPointer()) {
    TypePtr pointee = static_cast<sun::semantic_analysis::StaticPointerType*>(
                          objectType.get())
                          ->getPointeeType();
    if (pointee && pointee->isClass()) objectType = pointee;
  }

  const std::string& memberName = memberAccess.getMemberName();

  // Interface methods as values are not supported (would need a vtable
  // load at bind time). Only diagnose when a lambda is expected so
  // interface method calls stay untouched.
  if (objectType->isInterface() && expectedType && expectedType->isLambda()) {
    auto* ifaceType =
        static_cast<sun::semantic_analysis::InterfaceType*>(objectType.get());
    if (ifaceType->getMethod(memberName)) {
      logAndThrowError("Referencing interface method '" + memberName +
                           "' as a value is not supported",
                       memberAccess.getLocation());
    }
    return;
  }

  if (!objectType->isClass()) return;
  const auto* classType =
      static_cast<const sun::semantic_analysis::ClassType*>(objectType.get());
  if (classType->getField(memberName)) return;

  std::vector<const ClassMethod*> overloads;
  for (const auto& m : classType->getMethods()) {
    if (m.name == memberName) overloads.push_back(&m);
  }
  if (overloads.empty()) return;  // not a method (inferType already errored)

  const ClassMethod* chosen = nullptr;
  if (overloads.size() == 1) {
    chosen = overloads[0];
  } else if (expectedType && expectedType->isLambda()) {
    // Pick the overload matching the expected lambda signature. A
    // non-throwing method may bind where a throwing lambda is expected.
    const auto* expected = static_cast<const LambdaType*>(expectedType.get());
    std::vector<const ClassMethod*> matches;
    for (const auto* m : overloads) {
      LambdaType candidate(m->returnType, m->paramTypes, m->canThrow);
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

  auto boundType = Types::Lambda(chosen->returnType, chosen->paramTypes,
                                 chosen->canThrow, chosen->isUnsafe);
  // A bound method holds its receiver by reference, so the value is bound
  // to the frame the receiver lives in - the same escape rules as a lambda
  // with a `[ref ...]` capture list apply to it.
  static_cast<LambdaType*>(boundType.get())->setHasRefCaptures(true);
  memberAccess.setResolvedType(std::move(boundType));
  memberAccess.setTargetDeclarationId(chosen->declarationId);
  memberAccess.setIsBoundMethodRef(true);
}

// -------------------------------------------------------------------
// Payload enums

}  // namespace sun::semantic_analysis
