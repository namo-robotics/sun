/** Checks bindings and resolves global initializer dependencies. */
#include <algorithm>

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

using sun::semantic_analysis::containsCall;
using sun::semantic_analysis::isBorrowableLvalue;
using sun::semantic_analysis::type_analysis::isAssignableTo;
using sun::semantic_analysis::type_analysis::tryCoerceIntegerLiteral;
using sun::types::unwrapRef;

void DeclarationAnalyzer::analyzeVariableCreation(
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

bool DeclarationAnalyzer::isTrackedGlobal(
    const sun::ast::VariableCreationAST& varCreate) {
  return ctx_.isAtModuleLevel() && varCreate.hasQualifiedName() &&
         varCreate.getDeclarationId() &&
         ctx_.declarationTable().findGlobal(varCreate.getQualifiedName()) ==
             varCreate.getDeclarationId();
}

void DeclarationAnalyzer::verifyGlobalVarIsNotRepeated(
    const sun::ast::VariableCreationAST& global) const {
  auto repeated =
      std::find(globalsInProgress_.begin(), globalsInProgress_.end(), &global);
  if (repeated == globalsInProgress_.end()) return;
  const std::string& name = global.getName();
  std::string cycle;
  for (auto step = repeated; step != globalsInProgress_.end(); ++step)
    cycle += (*step)->getName() + " -> ";
  logAndThrowError(
      "Global variable '" + name + "' depends on itself: " + cycle + name,
      global.getLocation());
}

void DeclarationAnalyzer::analyzeGlobal(sun::ast::VariableCreationAST& global,
                                        SemanticScope& scope) {
  const std::string& name = global.getName();
  verifyGlobalVarIsNotRepeated(global);

  // A class field or a function signature asks for its array sizes while
  // declarations are still being collected, when no function can be called
  // yet: functions are registered after the types their signatures mention.
  if (ctx_.isCollectingDeclarations() && global.getValue()) {
    if (containsCall(*global.getValue()))
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
  SemanticContext::LifetimeScopeGuard lifetimes(ctx_, false, true);
  ctx_.setCurrentClass(nullptr);
  globalsInProgress_.push_back(&global);
  /** Puts back the analysis state of the code that used the global. */
  struct Restore {
    DeclarationAnalyzer& sema;
    std::shared_ptr<sun::types::ClassType> currentClass;
    /** Restores the saved state, including when an error unwinds. */
    ~Restore() {
      sema.ctx_.setCurrentClass(currentClass);
      sema.globalsInProgress_.pop_back();
    }
  } restore{*this, std::move(savedClass)};

  analyzeVariableDeclaration(global);
  // Inside a module it is also reachable as `module.name`. A bundle's hash
  // scope is not a module the program can name.
  if (scope.getType() == ScopeType::Module && !isLibraryScope(scope.scopeName))
    registerModuleVariable(global);
}

void DeclarationAnalyzer::registerModuleVariable(
    sun::ast::VariableCreationAST& varCreate) {
  if (auto type = varCreate.getResolvedType()) {
    ctx_.currentScope().declareModuleVariable(
        varCreate.getQualifiedName(), type, varCreate.getVisibility(),
        varCreate.isConst(), varCreate.isCExtern(),
        varCreate.getDeclarationId());
  }
}

void DeclarationAnalyzer::ensureGlobalAnalyzed(const std::string& name) {
  if (UnanalyzedGlobal global = ctx_.currentScope().findUnanalyzedGlobal(
          name, ctx_.declarationTable()))
    analyzeGlobal(*global.node, *global.scope);
}

void DeclarationAnalyzer::ensureModuleGlobalAnalyzed(
    const std::string& modulePath, const std::string& name) {
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

void DeclarationAnalyzer::analyzeVariableDeclaration(
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
      type = sema_.typeResolver().typeAnnotationToType(
          *varCreate.getTypeAnnotation());
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
    declaredType = sema_.typeResolver().typeAnnotationToType(
        *varCreate.getTypeAnnotation());
  }

  // Analyze the value expression, passing declared type as expected type
  sema_.analyzeExpr(const_cast<ExprAST&>(*varCreate.getValue()), declaredType);
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
    if (referenced->isInterface() ? isAssignableTo(rhsType, declaredType)
                                  : isAssignableTo(rhsType, referenced)) {
      if (!isBorrowableLvalue(*varCreate.getValue())) {
        logAndThrowError("Cannot bind reference '" + varCreate.getName() +
                             "' to a temporary - a reference must bind a "
                             "variable, field, or array element",
                         varCreate.getLocation());
      }
      bindsBorrow = true;
      sema_.expressions().validateBorrowTarget(*varCreate.getValue(),
                                               varCreate.getLocation());
      if (sun::types::isMutableRef(declaredType)) {
        sema_.expressions().requireMutablePlace(*varCreate.getValue(),
                                                "take a mutable reference to",
                                                varCreate.getLocation());
      }
    }
  }
  // Taking the value moves it: a field cannot leave an immutable object
  if (!bindsBorrow) {
    sema_.expressions().checkMoveSource(*varCreate.getValue(),
                                        varCreate.getLocation());
  }

  TypePtr type;
  if (declaredType) {
    // Check type compatibility: RHS must be assignable to declared type
    // Interface views borrow an existing concrete value.
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

  if (type && type->isInterface()) {
    // A call on an unbound generic receiver has only its return contract here.
    // Specialization checks the concrete return type before generating storage.
    bool deferredContract = false;
    if (!declaredType && varCreate.getValue()->getType() == ASTNodeType::CALL) {
      const auto& call =
          static_cast<const sun::ast::CallExprAST&>(*varCreate.getValue());
      if (call.getCallee()->getType() == ASTNodeType::MEMBER_ACCESS) {
        const auto& member =
            static_cast<const sun::ast::MemberAccessAST&>(*call.getCallee());
        auto receiver =
            sun::types::unwrapRef(member.getObject()->getResolvedType());
        deferredContract = receiver && receiver->isTypeParameter();
      }
    }
    if (!deferredContract)
      logAndThrowError("Interface '" + type->toDisplayString() +
                           "' cannot own a value; use ref or const ref, or own "
                           "a concrete type",
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

void DeclarationAnalyzer::analyzeReferenceCreation(
    sun::ast::ReferenceCreationAST& refCreate) {
  // Analyze the target expression
  sema_.analyzeExpr(const_cast<ExprAST&>(*refCreate.getTarget()));

  sema_.expressions().validateBorrowTarget(*refCreate.getTarget(),
                                           refCreate.getLocation());
  // A mutable borrow needs a place that may be changed
  if (refCreate.isMutable()) {
    sema_.expressions().requireMutablePlace(*refCreate.getTarget(),
                                            "take a mutable reference to",
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
