// analysis_statements.cpp — Statements that bind or write a place:
// declarations, assignments and borrows
//
// One handler per AST node kind, called from the dispatcher in
// analysis.cpp.

#include <algorithm>
#include <functional>

#include "codegen/abi/c_abi_types.h"
#include "semantic_analysis/expression_properties.h"
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

void SemanticAnalyzer::analyzeVariableCreation(
    sun::ast::VariableCreationAST& varCreate) {
  if (isTrackedGlobal(varCreate)) {
    // An earlier use may already have analyzed this global, which gave it
    // its type
    if (!varCreate.getResolvedType())
      analyzeGlobal(varCreate, ctx_.currentScope());
    return;
  }
  analyzeVariableDeclaration(varCreate);
}

bool SemanticAnalyzer::isTrackedGlobal(
    const sun::ast::VariableCreationAST& varCreate) {
  return ctx_.isAtModuleLevel() && varCreate.hasQualifiedName() &&
         varCreate.getDeclarationId() &&
         ctx_.declarationTable().findGlobal(varCreate.getQualifiedName()) ==
             varCreate.getDeclarationId();
}

void SemanticAnalyzer::analyzeGlobal(sun::ast::VariableCreationAST& global,
                                     SemanticScope& scope) {
  const std::string& name = global.getName();
  auto repeated =
      std::find(globalsInProgress_.begin(), globalsInProgress_.end(), &global);
  if (repeated != globalsInProgress_.end()) {
    std::string cycle;
    for (auto step = repeated; step != globalsInProgress_.end(); ++step)
      cycle += (*step)->getName() + " -> ";
    logAndThrowError(
        "Global variable '" + name + "' depends on itself: " + cycle + name,
        global.getLocation());
  }

  // A class field or a function signature asks for its array sizes while
  // declarations are still being collected, when no function can be called
  // yet: functions are registered after the types their signatures mention.
  if (ctx_.isCollectingDeclarations() && global.getValue()) {
    std::function<bool(const ExprAST&)> callsAFunction =
        [&](const ExprAST& expr) {
          if (expr.getType() == ASTNodeType::CALL) return true;
          bool found = false;
          const_cast<ExprAST&>(expr).forEachChildSlot(
              [&](std::unique_ptr<ExprAST>& child) {
                found = found || (child && callsAFunction(*child));
              });
          return found;
        };
    if (callsAFunction(*global.getValue()))
      logAndThrowError(
          "'" + name +
              "' is needed by a class field or a function signature, so its "
              "value cannot come from calling a function; write the value "
              "out, or compute it from other constants",
          global.getLocation());
  }

  // The initializer belongs to the scope and file of the declaration, and to
  // no class or function, wherever the use that asked for it happens to be.
  SemanticContext::ScopeSwitchGuard scopeSwitch(ctx_, &scope);
  SemanticContext::SourceFileGuard sourceFile(ctx_, global.getSourceFileId());
  auto savedClass = ctx_.getCurrentClass();
  auto savedLifetimes = std::move(activeLifetimeNames_);
  bool savedAllowThis = allowThisLifetime_;
  ctx_.setCurrentClass(nullptr);
  activeLifetimeNames_.clear();
  allowThisLifetime_ = false;
  globalsInProgress_.push_back(&global);
  /** Puts back the analysis state of the code that used the global. */
  struct Restore {
    SemanticAnalyzer& sema;
    std::shared_ptr<sun::types::ClassType> currentClass;
    std::vector<std::string> lifetimes;
    bool allowThis;
    /** Restores the saved state, including when an error unwinds. */
    ~Restore() {
      sema.ctx_.setCurrentClass(currentClass);
      sema.activeLifetimeNames_ = std::move(lifetimes);
      sema.allowThisLifetime_ = allowThis;
      sema.globalsInProgress_.pop_back();
    }
  } restore{*this, std::move(savedClass), std::move(savedLifetimes),
            savedAllowThis};

  analyzeVariableDeclaration(global);
  // Inside a module it is also reachable as `module.name`. A bundle's hash
  // scope is not a module the program can name.
  if (scope.getType() == ScopeType::Module && !isLibraryScope(scope.scopeName))
    registerModuleVariable(global);
}

void SemanticAnalyzer::registerModuleVariable(
    sun::ast::VariableCreationAST& varCreate) {
  if (auto type = varCreate.getResolvedType()) {
    ctx_.currentScope().declareModuleVariable(
        varCreate.getQualifiedName(), type, varCreate.getVisibility(),
        varCreate.isConst(), varCreate.isCExtern(),
        varCreate.getDeclarationId());
  }
}

void SemanticAnalyzer::ensureGlobalAnalyzed(const std::string& name) {
  if (UnanalyzedGlobal global = ctx_.currentScope().findUnanalyzedGlobal(
          name, ctx_.declarationTable()))
    analyzeGlobal(*global.node, *global.scope);
}

void SemanticAnalyzer::ensureModuleGlobalAnalyzed(const std::string& modulePath,
                                                  const std::string& name) {
  auto* moduleScope = ctx_.lookupModuleScope(modulePath);
  if (!moduleScope) return;
  const auto& declarations = ctx_.declarationTable();
  const auto* global = findVariableNode(
      declarations,
      declarations.findGlobal(QualifiedName(moduleScope->scopePath, name)));
  if (global && !global->getResolvedType())
    analyzeGlobal(const_cast<sun::ast::VariableCreationAST&>(*global),
                  *moduleScope);
}

void SemanticAnalyzer::analyzeVariableDeclaration(
    sun::ast::VariableCreationAST& varCreate) {
  if (varCreate.isCExtern()) {
    if (!ctx_.isAtModuleLevel()) {
      logAndThrowError("Extern variables are only allowed at module scope",
                       varCreate.getLocation());
    }
    if (!varCreate.hasTypeAnnotation()) {
      logAndThrowError("Extern variable '" + varCreate.getName() +
                           "' requires an explicit type",
                       varCreate.getLocation());
    }
    if (varCreate.hasValue()) {
      logAndThrowError("Extern variable '" + varCreate.getName() +
                           "' cannot have an initializer",
                       varCreate.getLocation());
    }
    TypePtr type = varCreate.getResolvedType();
    if (!type) {
      type = resolver_.typeAnnotationToType(*varCreate.getTypeAnnotation());
      varCreate.setResolvedType(type);
    }
    if (!sun::codegen::abi::isValue(type)) {
      logAndThrowError("Extern variable '" + varCreate.getName() +
                           "' has type '" + type->toDisplayString() +
                           "', which has no C equivalent",
                       varCreate.getLocation());
    }
    return;
  }

  auto varName = varCreate.getName();
  // Determine type first (before analyzing value, for array literals)
  TypePtr declaredType;
  if (varCreate.hasTypeAnnotation()) {
    checkAnnotationLifetimes(*varCreate.getTypeAnnotation(),
                             varCreate.getLocation());
    declaredType =
        resolver_.typeAnnotationToType(*varCreate.getTypeAnnotation());
  }

  // Analyze the value expression, passing declared type as expected type
  analyzeExpr(const_cast<ExprAST&>(*varCreate.getValue()), declaredType);
  TypePtr rhsType = varCreate.getValue()->getResolvedType();

  // A block that always leaves the function — `var x = unsafe { return 0; };`
  // — never produces a value, so there is nothing to bind and the binding
  // itself would be dead code. GNU C draws the same line for its statement
  // expressions.
  if (varCreate.getValue()->getType() == ASTNodeType::UNSAFE_BLOCK &&
      sun::semantic_analysis::alwaysExits(*varCreate.getValue())) {
    logAndThrowError(
        "Cannot bind '" + varCreate.getName() +
            "' to this unsafe block: it always leaves the function through a "
            "`return` or `throw`, so it never produces a value. Move the "
            "`return` out of the block.",
        varCreate.getLocation());
  }

  // Determine the final variable type
  // `var r: ref T = <lvalue>` borrows the lvalue's storage - the same
  // implicit borrow a `ref T` parameter takes at a call site. An RHS that
  // is already a reference (a call returning `ref T`) goes through
  // isAssignableTo instead.
  bool bindsBorrow = false;
  if (declaredType && declaredType->isReference() && rhsType &&
      !rhsType->isReference()) {
    auto referenced =
        static_cast<sun::types::ReferenceType*>(declaredType.get())
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
      if (sun::types::isMutableRef(declaredType)) {
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

  TypePtr type;
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
  if (type && sun::types::unwrapRef(type)->isVoid()) {
    logAndThrowError(declaredType
                         ? "Variable '" + varName + "' cannot have type 'void'"
                         : "Cannot infer a type for variable '" + varName +
                               "': the value assigned to it produces no result",
                     varCreate.getLocation());
  }

  validateTypeParameter(type, varCreate);

  // A global outlives every stack frame, so it cannot have a type that may
  // point into one: a '<'_>' lambda, or anything that transitively holds
  // one (a class, enum, container instantiation or array of them).
  if (ctx_.isAtModuleLevel() && sun::types::typeIsFrameCarrying(type)) {
    logAndThrowError(
        "a module-level variable cannot have the frame-carrying type '" +
            type->toDisplayString() +
            "' - it can hold a lambda whose captured environment lives in a "
            "stack frame, and the global would outlive that frame",
        varCreate.getLocation());
  }

  // Note: Move semantics tracking is handled by the borrow checker
  ctx_.currentScope().declareVariable(varCreate.getName(), type,
                                      /*isParam=*/false, varCreate.isConst(),
                                      varCreate.getDeclarationId());
  // Set the resolved type on the variable creation node itself
  varCreate.setResolvedType(type);
}

void SemanticAnalyzer::analyzeVariableAssignment(
    sun::ast::VariableAssignmentAST& varAssign) {
  // Look up the variable's type first for expected type propagation
  ensureGlobalAnalyzed(varAssign.getName());
  VariableInfo* varInfo =
      ctx_.currentScope().lookupVariable(varAssign.getName());
  if (varInfo) varAssign.setTargetDeclarationId(varInfo->declarationId);
  // A module-level global is emitted using its declaration ID; record it so
  // codegen can find the symbol (locals keep the name as written).
  if (varInfo && varInfo->isGlobal) {
    varAssign.setQualifiedName(ctx_.resolveNameWithUsings(varAssign.getName()));
  }
  if (varInfo) {
    checkExternVariableAccessAllowed(*varInfo, varAssign.getName(),
                                     varAssign.getLocation());
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
    }
  }

  // Analyze the value expression with expected type
  analyzeExpr(const_cast<ExprAST&>(*varAssign.getValue()), expectedTargetType);
  TypePtr rhsType = varAssign.getValue()->getResolvedType();
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
    varAssign.setResolvedType(requireResolvedType(*varAssign.getValue()));
  }
}

void SemanticAnalyzer::analyzeCompoundAssignment(
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
  analyzeExpr(const_cast<ExprAST&>(*compound.getTarget()));
  requireMutablePlace(*compound.getTarget(), "assign to",
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
      checkUnsafeCall(method->isUnsafe, "__setindex__", compound.getLocation());
      compound.setTargetDeclarationId(method->declarationId);
    }
  }

  // Analyze the value with the target's type as expected
  analyzeExpr(const_cast<ExprAST&>(*compound.getValue()), targetType);
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

  // Compound assignment is a statement; codegen returns the stored value
  compound.setResolvedType(Types::Void());
}

void SemanticAnalyzer::analyzeMemberAssignment(
    sun::ast::MemberAssignmentAST& memberAssign) {
  // Analyze the object first so the field type can flow into the value
  // as the expected type (e.g. `this.value = Option.None;`)
  analyzeExpr(const_cast<ExprAST&>(*memberAssign.getObject()));
  requireMutablePlace(
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
  analyzeExpr(const_cast<ExprAST&>(*memberAssign.getValue()),
              expectedFieldType);
  checkMoveSource(*memberAssign.getValue(), memberAssign.getLocation());

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

void SemanticAnalyzer::analyzeIndexedAssignment(
    sun::ast::IndexedAssignmentAST& assignment) {
  analyzeExpr(const_cast<ExprAST&>(*assignment.getTarget()));
  requireMutablePlace(*assignment.getTarget(), "assign to an element of",
                      assignment.getLocation());
  analyzeExpr(const_cast<ExprAST&>(*assignment.getValue()));
  checkMoveSource(*assignment.getValue(), assignment.getLocation());

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
      checkUnsafeCall(method->isUnsafe, "__setindex__",
                      assignment.getLocation());
    }
  }

  // Get the element type from the target (what we're assigning to)
  TypePtr elementType = assignment.getTarget()->getResolvedType();
  ExprAST* valueExpr = const_cast<ExprAST*>(assignment.getValue());

  // Try to coerce integer literal to target type (throws if doesn't fit)
  tryCoerceIntegerLiteral(valueExpr, elementType, /*throwOnFail=*/true);

  assignment.setResolvedType(requireResolvedType(*assignment.getValue()));
}

void SemanticAnalyzer::analyzeReferenceCreation(
    sun::ast::ReferenceCreationAST& refCreate) {
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
  TypePtr targetType = unwrapRef(requireResolvedType(*refCreate.getTarget()));
  // Create reference type: ref(T) or const ref(T)
  TypePtr refType = requireInferredType(
      sun::semantic_analysis::type_analysis::TypeInferer::reference(
          targetType, refCreate.isMutable()),
      refCreate.getLocation(), "Cannot determine reference target type");
  // Declare the reference variable
  ctx_.currentScope().declareVariable(refCreate.getName(), refType, false,
                                      false, refCreate.getDeclarationId());
  // Set the resolved type
  refCreate.setResolvedType(refType);
}

}  // namespace sun::semantic_analysis
