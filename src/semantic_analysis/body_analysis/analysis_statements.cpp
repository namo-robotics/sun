/** Checks analysis statements within the semantic session. */
#include <algorithm>
#include <functional>

#include "codegen/abi/c_abi_types.h"
#include "semantic_analysis/expression_analysis/expression_properties.h"
#include "semantic_analysis/globals.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/type_analysis/type_rules.h"
#include "support/error.h"

using sun::types::ClassType;
using sun::types::TypePtr;
using sun::types::Types;

using sun::ast::ASTNodeType;
using sun::ast::ExprAST;
using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

using sun::semantic_analysis::isBorrowableLvalue;
using sun::semantic_analysis::type_analysis::isAssignableTo;
using sun::semantic_analysis::type_analysis::tryCoerceIntegerLiteral;
using sun::types::unwrapRef;

void BodyAnalyzer::analyzeVariableAssignment(
    sun::ast::VariableAssignmentAST& varAssign) {
  // Look up the variable's type first for expected type propagation
  sema_.declarations().ensureGlobalAnalyzed(varAssign.getName());
  VariableInfo* varInfo =
      ctx_.currentScope().lookupVariable(varAssign.getName());
  if (varInfo) varAssign.setTargetDeclarationId(varInfo->declarationId);
  // A module-level global is emitted using its declaration ID; record it so
  // codegen can find the symbol (locals keep the name as written).
  if (varInfo && varInfo->isGlobal) {
    varAssign.setQualifiedName(ctx_.resolveNameWithUsings(varAssign.getName()));
  }
  if (varInfo) {
    sema_.expressions().checkExternVariableAccessAllowed(
        *varInfo, varAssign.getName(), varAssign.getLocation());
  }
  if (varInfo && varInfo->isConst) {
    logAndThrowError("Cannot assign to constant '" + varAssign.getName() +
                         "'; declare it with 'var' if it must change",
                     varAssign.getLocation());
  }
  if (varInfo && sun::types::isConstRef(varInfo->type)) {
    logAndThrowError(
        "Cannot assign through const reference '" + varAssign.getName() + "'",
        varAssign.getLocation());
  }
  if (varInfo && varInfo->captureKind == sun::ast::CaptureKind::ByValue) {
    logAndThrowError("Cannot mutate by-value captured variable '" +
                         varAssign.getName() +
                         "': capture it by reference with '[ref " +
                         varAssign.getName() + "]() => ...'",
                     varAssign.getLocation());
  }
  TypePtr expectedTargetType = nullptr;
  if (varInfo) {
    expectedTargetType = varInfo->type;
    // For reference types, the target is the referenced type
    if (expectedTargetType && expectedTargetType->isReference()) {
      auto* refType =
          static_cast<sun::types::ReferenceType*>(expectedTargetType.get());
      expectedTargetType = refType->getReferencedType();
      if (expectedTargetType && expectedTargetType->isInterface())
        logAndThrowError("Cannot assign through a borrowed interface view",
                         varAssign.getLocation());
    }
  }

  // Analyze the value expression with expected type
  sema_.analyzeExpr(const_cast<ExprAST&>(*varAssign.getValue()),
                    expectedTargetType);
  TypePtr rhsType = varAssign.getValue()->getResolvedType();
  sema_.expressions().checkMoveSource(*varAssign.getValue(),
                                      varAssign.getLocation());

  if (varInfo) {
    // Check type compatibility for interface polymorphism
    if (rhsType && expectedTargetType &&
        !isAssignableTo(rhsType, expectedTargetType)) {
      // Allow integer literal coercion as a fallback
      if (!tryCoerceIntegerLiteral(const_cast<ExprAST*>(varAssign.getValue()),
                                   expectedTargetType, false)) {
        logAndThrowError("Cannot assign value of type '" +
                             rhsType->toDisplayString() + "' to variable '" +
                             varAssign.getName() + "' of type '" +
                             varInfo->type->toDisplayString() + "'",
                         varAssign.getLocation());
      }
    }
    varAssign.setResolvedType(varInfo->type);
  } else {
    varAssign.setResolvedType(
        sema_.expressions().requireResolvedType(*varAssign.getValue()));
  }
}

void BodyAnalyzer::analyzeCompoundAssignment(
    sun::ast::CompoundAssignmentAST& compound) {
  // By-value captures are immutable (mirror VARIABLE_ASSIGNMENT)
  if (compound.getTarget()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef = static_cast<const sun::ast::VariableReferenceAST&>(
        *compound.getTarget());
    VariableInfo* varInfo =
        ctx_.currentScope().lookupVariable(varRef.getName());
    if (varInfo && varInfo->captureKind == sun::ast::CaptureKind::ByValue) {
      logAndThrowError("Cannot mutate by-value captured variable '" +
                           varRef.getName() +
                           "': capture it by reference with '[ref " +
                           varRef.getName() + "]() => ...'",
                       compound.getLocation());
    }
  }

  // Analyze the target as a read: gives the whole target subtree
  // resolved types (codegen signedness depends on them)
  sema_.analyzeExpr(const_cast<ExprAST&>(*compound.getTarget()));
  sema_.expressions().requireMutablePlace(*compound.getTarget(), "assign to",
                                          compound.getLocation());
  TypePtr targetType =
      sun::types::unwrapRef(compound.getTarget()->getResolvedType());
  if (compound.getTarget()->getType() == ASTNodeType::INDEX) {
    const auto& index =
        static_cast<const sun::ast::IndexAST&>(*compound.getTarget());
    auto receiver = sun::types::unwrapRef(index.getTarget()->getResolvedType());
    if (const auto* cls =
            sun::codegen::support::tryGetType<ClassType>(receiver)) {
      const auto* method =
          ctx_.accessibleMethod(*cls, "__setindex__", compound.getLocation());
      if (!method)
        logAndThrowError("Class does not implement __setindex__ for assignment",
                         compound.getLocation());
      sema_.expressions().checkUnsafeCall(method->isUnsafe, "__setindex__",
                                          compound.getLocation());
      compound.setTargetDeclarationId(method->declarationId);
    }
  }

  // Analyze the value with the target's type as expected
  sema_.analyzeExpr(const_cast<ExprAST&>(*compound.getValue()), targetType);
  TypePtr rhsType = compound.getValue()->getResolvedType();

  if (rhsType && targetType && !isAssignableTo(rhsType, targetType)) {
    // Allow integer literal coercion as a fallback
    if (!tryCoerceIntegerLiteral(const_cast<ExprAST*>(compound.getValue()),
                                 targetType, false)) {
      logAndThrowError(
          "Cannot apply '" + compound.getOp().text + "' with value of type '" +
              rhsType->toDisplayString() + "' to target of type '" +
              targetType->toDisplayString() + "'",
          compound.getLocation());
    }
  }

  sema_.expressions().checkIntegerDivision(
      *compound.getTarget(), *compound.getValue(), compound.getOp().kind,
      compound.getLocation());

  // Compound assignment is a statement; codegen returns the stored value
  compound.setResolvedType(Types::Void());
}

void BodyAnalyzer::analyzeMemberAssignment(
    sun::ast::MemberAssignmentAST& memberAssign) {
  // Analyze the object first so the field type can flow into the value
  // as the expected type (e.g. `this.value = Option.None;`)
  sema_.analyzeExpr(const_cast<ExprAST&>(*memberAssign.getObject()));
  sema_.expressions().requireMutablePlace(
      *memberAssign.getObject(),
      "assign to field '" + memberAssign.getMemberName() + "' of",
      memberAssign.getLocation());

  TypePtr objectType = memberAssign.getObject()->getResolvedType();
  objectType = unwrapRef(objectType);

  // mod.global = value: a write to a module-level variable, not a field
  if (objectType && objectType->isModule()) {
    analyzeModuleGlobalAssignment(memberAssign, *objectType);
    memberAssign.setResolvedType(Types::Void());
    return;
  }

  if (objectType && objectType->isRawPointer()) {
    if (!ctx_.isInUnsafeBlock())
      logAndThrowError(
          "Dereferencing 'raw_ptr' can only be done in an unsafe block",
          memberAssign.getLocation());
    objectType = static_cast<const sun::types::RawPointerType&>(*objectType)
                     .getPointeeType();
  } else if (objectType && objectType->isStaticPointer()) {
    objectType = static_cast<const sun::types::StaticPointerType&>(*objectType)
                     .getPointeeType();
  }

  TypePtr expectedFieldType;
  if (objectType && objectType->isClass()) {
    auto* classType = static_cast<ClassType*>(objectType.get());
    if (const sun::types::ClassField* field =
            ctx_.accessibleField(*classType, memberAssign.getMemberName(),
                                 memberAssign.getLocation())) {
      memberAssign.setTargetDeclarationId(field->declarationId);
      expectedFieldType = field->type;
    }
  }
  sema_.analyzeExpr(const_cast<ExprAST&>(*memberAssign.getValue()),
                    expectedFieldType);
  sema_.expressions().checkMoveSource(*memberAssign.getValue(),
                                      memberAssign.getLocation());

  if (objectType && objectType->isClass()) {
    auto* classType = static_cast<ClassType*>(objectType.get());
    const sun::types::ClassField* field =
        classType->getField(memberAssign.getMemberName());
    if (field) {
      TypePtr rhsType = memberAssign.getValue()->getResolvedType();
      TypePtr fieldType = field->type;

      if (rhsType && !isAssignableTo(rhsType, fieldType)) {
        // Allow integer literal coercion as a fallback
        if (!tryCoerceIntegerLiteral(
                const_cast<ExprAST*>(memberAssign.getValue()), fieldType,
                false)) {
          logAndThrowError("Cannot assign value of type '" +
                               rhsType->toDisplayString() + "' to field '" +
                               memberAssign.getMemberName() + "' of type '" +
                               fieldType->toDisplayString() + "'",
                           memberAssign.getLocation());
        }
      }
    }
  }

  memberAssign.setResolvedType(Types::Void());
}

void BodyAnalyzer::analyzeIndexedAssignment(
    sun::ast::IndexedAssignmentAST& assignment) {
  sema_.analyzeExpr(const_cast<ExprAST&>(*assignment.getTarget()));
  sema_.expressions().requireMutablePlace(*assignment.getTarget(),
                                          "assign to an element of",
                                          assignment.getLocation());
  sema_.analyzeExpr(const_cast<ExprAST&>(*assignment.getValue()));
  sema_.expressions().checkMoveSource(*assignment.getValue(),
                                      assignment.getLocation());

  // Retain the setter selected for a class indexed assignment.
  if (assignment.getTarget()->getType() == ASTNodeType::INDEX) {
    const auto& idx =
        static_cast<const sun::ast::IndexAST&>(*assignment.getTarget());
    TypePtr objType = unwrapRef(idx.getTarget()->getResolvedType());
    if (objType && objType->isClass()) {
      const auto* method =
          ctx_.accessibleMethod(static_cast<const ClassType&>(*objType),
                                "__setindex__", assignment.getLocation());
      if (!method)
        logAndThrowError("Class does not implement __setindex__ for assignment",
                         assignment.getLocation());
      assignment.setTargetDeclarationId(method->declarationId);
      sema_.expressions().checkUnsafeCall(method->isUnsafe, "__setindex__",
                                          assignment.getLocation());
    }
  }

  // Get the element type from the target (what we're assigning to)
  TypePtr elementType = assignment.getTarget()->getResolvedType();
  ExprAST* valueExpr = const_cast<ExprAST*>(assignment.getValue());

  // Try to coerce integer literal to target type (throws if doesn't fit)
  tryCoerceIntegerLiteral(valueExpr, elementType, /*throwOnFail=*/true);

  assignment.setResolvedType(
      sema_.expressions().requireResolvedType(*assignment.getValue()));
}

}  // namespace sun::semantic_analysis
