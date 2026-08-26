// analysis_statements.cpp — Statements that bind or write a place:
// declarations, assignments and borrows
//
// One handler per AST node kind, called from the dispatcher in
// analysis.cpp.

#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/type_rules.h"
#include "support/error.h"

using sun::unwrapRef;
using sun::rules::isAssignableTo;
using sun::rules::isBorrowableLvalue;
using sun::rules::tryCoerceIntegerLiteral;

void SemanticAnalyzer::analyzeVariableCreation(VariableCreationAST& varCreate) {
  auto varName = varCreate.getName();
  // Determine type first (before analyzing value, for array literals)
  sun::TypePtr declaredType;
  if (varCreate.hasTypeAnnotation()) {
    declaredType = types_.typeAnnotationToType(*varCreate.getTypeAnnotation());
    // For array literals with explicit type annotation, set the type before
    // analysis
    if (varCreate.getValue()->getType() == ASTNodeType::ARRAY_LITERAL) {
      const_cast<ExprAST&>(*varCreate.getValue()).setResolvedType(declaredType);
    }
  }

  // Named functions cannot be assigned to variables - only lambdas
  if (varCreate.getValue()->isFunction()) {
    logAndThrowError("Cannot assign a named function to variable '" +
                         varCreate.getName() + "'. Use a lambda instead.",
                     varCreate.getLocation());
  }

  // Analyze the value expression, passing declared type as expected type
  analyzeExpr(const_cast<ExprAST&>(*varCreate.getValue()), declaredType);
  sun::TypePtr rhsType = varCreate.getValue()->getResolvedType();

  // Determine the final variable type
  // `var r: ref T = <lvalue>` borrows the lvalue's storage - the same
  // implicit borrow a `ref T` parameter takes at a call site. An RHS that
  // is already a reference (a call returning `ref T`) goes through
  // isAssignableTo instead.
  bool bindsBorrow = false;
  if (declaredType && declaredType->isReference() && rhsType &&
      !rhsType->isReference()) {
    auto referenced = static_cast<sun::ReferenceType*>(declaredType.get())
                          ->getReferencedType();
    if (isAssignableTo(rhsType, referenced)) {
      if (!isBorrowableLvalue(*varCreate.getValue())) {
        logAndThrowError("Cannot bind reference '" + varCreate.getName() +
                             "' to a temporary - a reference must bind a "
                             "variable, field, or array element",
                         varCreate.getLocation());
      }
      bindsBorrow = true;
      validateBorrowTarget(*varCreate.getValue(), varCreate.getLocation());
      if (sun::isMutableRef(declaredType)) {
        requireMutablePlace(*varCreate.getValue(),
                            "take a mutable reference to",
                            varCreate.getLocation());
      }
    }
  }
  // Taking the value moves it: a field cannot leave an immutable object
  if (!bindsBorrow) {
    checkMoveSource(*varCreate.getValue(), varCreate.getLocation());
  }

  sun::TypePtr type;
  if (declaredType) {
    // Check type compatibility: RHS must be assignable to declared type
    // This enables interface polymorphism: var s: IShape = Circle(...)
    if (!bindsBorrow && rhsType && !isAssignableTo(rhsType, declaredType)) {
      // Allow integer literal coercion as a fallback
      if (!tryCoerceIntegerLiteral(const_cast<ExprAST*>(varCreate.getValue()),
                                   declaredType, false)) {
        logAndThrowError("Cannot assign value of type '" +
                             rhsType->toDisplayString() + "' to variable '" +
                             varCreate.getName() + "' of type '" +
                             declaredType->toDisplayString() + "'",
                         varCreate.getLocation());
      }
    }
    type = declaredType;
  } else {
    type = rhsType;
  }

  // Nothing can be stored in a variable of type void, and an inferred
  // `var` has nothing to infer from a call that returns nothing.
  if (type && sun::unwrapRef(type)->isVoid()) {
    logAndThrowError(declaredType
                         ? "Variable '" + varName + "' cannot have type 'void'"
                         : "Cannot infer a type for variable '" + varName +
                               "': the value assigned to it produces no result",
                     varCreate.getLocation());
  }

  validateTypeParameter(type, varCreate);

  // Note: Move semantics tracking is handled by the borrow checker
  ctx_.declareVariable(varCreate.getName(), type, /*isParam=*/false,
                       varCreate.isConst());
  // Set the resolved type on the variable creation node itself
  varCreate.setResolvedType(type);
}

void SemanticAnalyzer::analyzeVariableAssignment(
    VariableAssignmentAST& varAssign) {
  // Named functions cannot be assigned to variables - only lambdas
  if (varAssign.getValue()->isFunction()) {
    logAndThrowError("Cannot assign a named function to variable '" +
                         varAssign.getName() + "'. Use a lambda instead.",
                     varAssign.getLocation());
  }

  // Look up the variable's type first for expected type propagation
  VariableInfo* varInfo = ctx_.lookupVariable(varAssign.getName());
  // A module-level global is emitted under its mangled name; record it so
  // codegen can find the symbol (locals keep the name as written).
  if (varInfo && varInfo->isGlobal) {
    varAssign.setQualifiedName(ctx_.resolveNameWithUsings(varAssign.getName()));
  }
  if (varInfo && varInfo->isConst) {
    logAndThrowError("Cannot assign to constant '" + varAssign.getName() +
                         "'; declare it with 'var' if it must change",
                     varAssign.getLocation());
  }
  if (varInfo && sun::isConstRef(varInfo->type)) {
    logAndThrowError(
        "Cannot assign through const reference '" + varAssign.getName() + "'",
        varAssign.getLocation());
  }
  if (varInfo && varInfo->captureKind == CaptureKind::ByValue) {
    logAndThrowError("Cannot mutate by-value captured variable '" +
                         varAssign.getName() +
                         "': capture it by reference with 'lambda [ref " +
                         varAssign.getName() + "]'",
                     varAssign.getLocation());
  }
  sun::TypePtr expectedTargetType = nullptr;
  if (varInfo) {
    expectedTargetType = varInfo->type;
    // For reference types, the target is the referenced type
    if (expectedTargetType && expectedTargetType->isReference()) {
      auto* refType =
          static_cast<sun::ReferenceType*>(expectedTargetType.get());
      expectedTargetType = refType->getReferencedType();
    }
  }

  // Analyze the value expression with expected type
  analyzeExpr(const_cast<ExprAST&>(*varAssign.getValue()), expectedTargetType);
  sun::TypePtr rhsType = varAssign.getValue()->getResolvedType();
  checkMoveSource(*varAssign.getValue(), varAssign.getLocation());

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
    varAssign.setResolvedType(types_.inferType(varAssign));
  }
}

void SemanticAnalyzer::analyzeCompoundAssignment(
    CompoundAssignmentAST& compound) {
  // By-value captures are immutable (mirror VARIABLE_ASSIGNMENT)
  if (compound.getTarget()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*compound.getTarget());
    VariableInfo* varInfo = ctx_.lookupVariable(varRef.getName());
    if (varInfo && varInfo->captureKind == CaptureKind::ByValue) {
      logAndThrowError("Cannot mutate by-value captured variable '" +
                           varRef.getName() +
                           "': capture it by reference with 'lambda [ref " +
                           varRef.getName() + "]'",
                       compound.getLocation());
    }
  }

  // Analyze the target as a read: gives the whole target subtree
  // resolved types (codegen signedness depends on them)
  analyzeExpr(const_cast<ExprAST&>(*compound.getTarget()));
  requireMutablePlace(*compound.getTarget(), "assign to",
                      compound.getLocation());
  sun::TypePtr targetType =
      sun::unwrapRef(compound.getTarget()->getResolvedType());

  // Analyze the value with the target's type as expected
  analyzeExpr(const_cast<ExprAST&>(*compound.getValue()), targetType);
  sun::TypePtr rhsType = compound.getValue()->getResolvedType();

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

  // Compound assignment is a statement; codegen returns the stored value
  compound.setResolvedType(sun::Types::Void());
}

void SemanticAnalyzer::analyzeMemberAssignment(
    MemberAssignmentAST& memberAssign) {
  // Analyze the object first so the field type can flow into the value
  // as the expected type (e.g. `this.value = Option.None;`)
  analyzeExpr(const_cast<ExprAST&>(*memberAssign.getObject()));
  requireMutablePlace(
      *memberAssign.getObject(),
      "assign to field '" + memberAssign.getMemberName() + "' of",
      memberAssign.getLocation());

  sun::TypePtr objectType = memberAssign.getObject()->getResolvedType();
  objectType = unwrapRef(objectType);

  // mod.global = value: a write to a module-level variable, not a field
  if (objectType && objectType->isModule()) {
    analyzeModuleGlobalAssignment(memberAssign, *objectType);
    memberAssign.setResolvedType(sun::Types::Void());
    return;
  }

  sun::TypePtr expectedFieldType;
  if (objectType && objectType->isClass()) {
    auto* classType = static_cast<sun::ClassType*>(objectType.get());
    if (const sun::ClassField* field =
            ctx_.accessibleField(*classType, memberAssign.getMemberName(),
                                 memberAssign.getLocation())) {
      expectedFieldType = field->type;
    }
  }
  analyzeExpr(const_cast<ExprAST&>(*memberAssign.getValue()),
              expectedFieldType);
  checkMoveSource(*memberAssign.getValue(), memberAssign.getLocation());

  if (objectType && objectType->isClass()) {
    auto* classType = static_cast<sun::ClassType*>(objectType.get());
    const sun::ClassField* field =
        classType->getField(memberAssign.getMemberName());
    if (field) {
      sun::TypePtr rhsType = memberAssign.getValue()->getResolvedType();
      sun::TypePtr fieldType = field->type;

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

  memberAssign.setResolvedType(sun::Types::Void());
}

void SemanticAnalyzer::analyzeIndexedAssignment(
    IndexedAssignmentAST& assignment) {
  analyzeExpr(const_cast<ExprAST&>(*assignment.getTarget()));
  requireMutablePlace(*assignment.getTarget(), "assign to an element of",
                      assignment.getLocation());
  analyzeExpr(const_cast<ExprAST&>(*assignment.getValue()));
  checkMoveSource(*assignment.getValue(), assignment.getLocation());

  // `obj[i] = v` on a class dispatches to __setindex__ (resolved in
  // codegen); it must be accessible from here like any other member
  if (assignment.getTarget()->getType() == ASTNodeType::INDEX) {
    const auto& idx = static_cast<const IndexAST&>(*assignment.getTarget());
    sun::TypePtr objType = unwrapRef(idx.getTarget()->getResolvedType());
    if (objType && objType->isClass()) {
      ctx_.accessibleMethod(static_cast<const sun::ClassType&>(*objType),
                            "__setindex__", assignment.getLocation());
    }
  }

  // Get the element type from the target (what we're assigning to)
  sun::TypePtr elementType = assignment.getTarget()->getResolvedType();
  ExprAST* valueExpr = const_cast<ExprAST*>(assignment.getValue());

  // Try to coerce integer literal to target type (throws if doesn't fit)
  tryCoerceIntegerLiteral(valueExpr, elementType, /*throwOnFail=*/true);

  assignment.setResolvedType(types_.inferType(assignment));
}

void SemanticAnalyzer::analyzeReferenceCreation(
    ReferenceCreationAST& refCreate) {
  // Analyze the target expression
  analyzeExpr(const_cast<ExprAST&>(*refCreate.getTarget()));

  validateBorrowTarget(*refCreate.getTarget(), refCreate.getLocation());
  // A mutable borrow needs a place that may be changed
  if (refCreate.isMutable()) {
    requireMutablePlace(*refCreate.getTarget(), "take a mutable reference to",
                        refCreate.getLocation());
  }
  // Determine the type of the referenced expression. Rebinding through
  // another reference borrows the same referent, not the reference.
  sun::TypePtr targetType = unwrapRef(types_.inferType(*refCreate.getTarget()));
  // Create reference type: ref(T) or const ref(T)
  sun::TypePtr refType =
      sun::Types::Reference(targetType, refCreate.isMutable());
  // Declare the reference variable
  ctx_.declareVariable(refCreate.getName(), refType);
  // Set the resolved type
  refCreate.setResolvedType(refType);
}
