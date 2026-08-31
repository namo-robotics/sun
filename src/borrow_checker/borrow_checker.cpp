// src/borrow_checker/borrow_checker.cpp
// Implementation of the main borrow checker

#include "borrow_checker/borrow_checker.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <optional>

#include "ast/control_flow.h"
#include "support/config.h"
#include "support/error.h"

namespace sun {

BorrowChecker::BorrowChecker() {}

std::vector<BorrowError> BorrowChecker::check(const BlockExprAST& program) {
  errors_.clear();
  state_.clear();
  currentScope_ = 0;
  currentFunction_.clear();
  refVariables_.clear();
  movedVariables_.clear();
  frameBoundVars_.clear();
  frameSourcedLambdas_.clear();
  frameLocalNames_.clear();
  declDepths_.clear();
  refHolderBounds_.clear();
  refTypedParams_.clear();
  rawPointerLocals_.clear();
  functionScopeDepth_ = 0;
  currentFunctionReturnsRef_ = false;
  paramLifetimes_.clear();
  nextLifetimeId_ = 0;
  classesWithRefFields_.clear();

  // Check all statements
  for (const auto& stmt : program.getBody()) {
    checkExpr(*stmt);
  }

  return errors_;
}

void BorrowChecker::checkExpr(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::VARIABLE_CREATION:
      checkVariableCreation(static_cast<const VariableCreationAST&>(expr));
      break;

    case ASTNodeType::REFERENCE_CREATION:
      checkReferenceCreation(static_cast<const ReferenceCreationAST&>(expr));
      break;

    case ASTNodeType::VARIABLE_ASSIGNMENT:
      checkVariableAssignment(static_cast<const VariableAssignmentAST&>(expr));
      break;

    case ASTNodeType::VARIABLE_REFERENCE:
      checkVariableReference(static_cast<const VariableReferenceAST&>(expr));
      break;

    case ASTNodeType::BINARY:
      checkBinaryExpr(static_cast<const BinaryExprAST&>(expr));
      break;

    case ASTNodeType::CALL:
      checkCallExpr(static_cast<const CallExprAST&>(expr));
      break;

    case ASTNodeType::IF:
      checkIfExpr(static_cast<const IfExprAST&>(expr));
      break;

    case ASTNodeType::TERNARY:
      checkTernaryExpr(static_cast<const TernaryExprAST&>(expr));
      break;

    case ASTNodeType::MATCH:
      checkMatchExpr(static_cast<const MatchExprAST&>(expr));
      break;

    case ASTNodeType::WHILE_LOOP:
      checkWhileExpr(static_cast<const WhileExprAST&>(expr));
      break;

    case ASTNodeType::FOR_LOOP:
      checkForExpr(static_cast<const ForExprAST&>(expr));
      break;

    case ASTNodeType::FOR_IN_LOOP:
      checkForInExpr(static_cast<const ForInExprAST&>(expr));
      break;

    case ASTNodeType::BLOCK:
      checkBlockExpr(static_cast<const BlockExprAST&>(expr));
      break;

    case ASTNodeType::RETURN:
      checkReturnStmt(static_cast<const ReturnExprAST&>(expr));
      break;

    case ASTNodeType::FUNCTION:
      checkFunctionDef(static_cast<const FunctionAST&>(expr));
      break;

    case ASTNodeType::LAMBDA:
      checkLambdaDef(static_cast<const LambdaAST&>(expr));
      break;

    case ASTNodeType::CLASS_DEFINITION:
      checkClassDef(static_cast<const ClassDefinitionAST&>(expr));
      break;

    case ASTNodeType::MEMBER_ACCESS:
      checkMemberAccess(static_cast<const MemberAccessAST&>(expr));
      break;

    case ASTNodeType::MEMBER_ASSIGNMENT:
      checkMemberAssignment(static_cast<const MemberAssignmentAST&>(expr));
      break;

    case ASTNodeType::INDEXED_ASSIGNMENT:
      checkIndexedAssignment(static_cast<const IndexedAssignmentAST&>(expr));
      break;

    case ASTNodeType::COMPOUND_ASSIGNMENT:
      checkCompoundAssignment(static_cast<const CompoundAssignmentAST&>(expr));
      break;

    case ASTNodeType::TRY_CATCH:
      checkTryCatch(static_cast<const TryCatchExprAST&>(expr));
      break;

    case ASTNodeType::UNSAFE_BLOCK:
      checkUnsafeBlock(static_cast<const UnsafeBlockAST&>(expr));
      break;

    case ASTNodeType::MODULE:
      checkBlockExpr(static_cast<const ModuleAST&>(expr).getBody());
      break;

    case ASTNodeType::MOON_SCOPE:
      checkBlockExpr(static_cast<const MoonScopeAST&>(expr).getBody());
      break;

    case ASTNodeType::UNARY:
      checkExpr(*static_cast<const UnaryExprAST&>(expr).getOperand());
      break;

    case ASTNodeType::GENERIC_CALL: {
      // Intrinsics and generic functions: arguments still use variables
      const auto& gcall = static_cast<const GenericCallAST&>(expr);
      bool hasFrameSourcedLambda = false;
      for (const auto& arg : gcall.getArgs()) {
        if (arg) {
          checkExpr(*arg);
          // Parameter kinds are not resolved here, so a frame-bound value
          // is rejected outright rather than only for by-value parameters
          if (isFrameBoundExpr(*arg)) {
            reportFrameBoundEscapeThroughCall(arg->getLocation());
          }
          if (isFrameSourcedLambdaExpr(*arg)) hasFrameSourcedLambda = true;
        }
      }
      // Same conservatism for a frame-sourced lambda: with parameter kinds
      // unknown, every compound argument is a place the callee could keep
      // it, so each must die with this frame
      if (hasFrameSourcedLambda) {
        size_t envDepth = functionScopeDepth_;
        for (const auto& arg : gcall.getArgs()) {
          if (arg && isFrameSourcedLambdaExpr(*arg)) {
            envDepth = std::max(envDepth, inferEnvDepth(*arg));
          }
        }
        for (const auto& arg : gcall.getArgs()) {
          if (!arg || isFrameSourcedLambdaExpr(*arg)) continue;
          auto argType = arg->getResolvedType();
          if (!argType || !argType->isCompound()) continue;
          const std::string* base = getBaseVariableName(*arg);
          if (base) {
            noteFrameSourcedLambdaStore(*base, envDepth, arg->getLocation());
          }
        }
      }
      break;
    }

    // These don't need borrow checking
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
    case ASTNodeType::CHAR_LITERAL:
    case ASTNodeType::BOOL_LITERAL:
    case ASTNodeType::NULL_LITERAL:
    case ASTNodeType::STRUCT_LITERAL:
    case ASTNodeType::ARRAY_LITERAL:
    case ASTNodeType::INDEX:
    case ASTNodeType::THIS:
    case ASTNodeType::PROTOTYPE:
    case ASTNodeType::IMPORT:
    case ASTNodeType::DECLARE_TYPE:
    case ASTNodeType::USING:
    case ASTNodeType::MANIFEST:
    case ASTNodeType::QUALIFIED_NAME:
    case ASTNodeType::INTERFACE_DEFINITION:
    case ASTNodeType::ENUM_DEFINITION:
    case ASTNodeType::PACK_EXPANSION:  // only in unexpanded generic templates
    case ASTNodeType::THROW:
    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
      break;

    default:
      throw SunError(SunError::Kind::Compile,
                     "BorrowChecker: unhandled AST node type: " +
                         std::to_string(static_cast<int>(expr.getType())));
  }
}

void BorrowChecker::checkVariableCreation(const VariableCreationAST& var) {
  // Check the initializer expression
  if (var.getValue()) {
    checkExpr(*var.getValue());
  }

  // A fresh declaration (e.g. the next loop iteration, or a same-named
  // variable in a sibling scope) starts un-moved
  movedVariables_.erase(var.getName());
  clearFieldPaths(var.getName());
  noteLoopLocal(var.getName());
  frameLocalNames_.insert(var.getName());
  declDepths_[var.getName()] = currentScope_;

  // The new name holds a frame-bound value (see frameBoundVars_) when its
  // initializer is one: a spawn over a capture-list lambda, or another
  // frame-bound local moved in. A fresh declaration is always at least as
  // deep as its initializer's environment, so no depth check is needed.
  if (var.getValue() && isFrameBoundExpr(*var.getValue())) {
    frameBoundVars_[var.getName()] = inferEnvDepth(*var.getValue());
  } else {
    frameBoundVars_.erase(var.getName());
  }

  // A lambda-typed local sourced from this frame (see frameSourcedLambdas_)
  if (var.getValue() && isFrameSourcedLambdaExpr(*var.getValue())) {
    frameSourcedLambdas_[var.getName()] = inferEnvDepth(*var.getValue());
  } else {
    frameSourcedLambdas_.erase(var.getName());
  }

  // A ref-storing class value landing in a fresh name records the deepest
  // declaration it borrows, so later moves toward outer scopes are checked.
  refHolderBounds_.erase(var.getName());
  if (var.getValue()) {
    trackRefHolderStore(var.getName(), currentScope_, *var.getValue(),
                        var.getLocation());
  }

  // Remember raw_ptr<T> locals; see rawPointerLocals_. The annotation is read
  // rather than the resolved type, which is only known per specialization.
  const auto& annotation = var.getTypeAnnotation();
  bool isRawPointer = annotation && annotation->baseName == "raw_ptr";
  if (!isRawPointer && var.getValue()) {
    TypePtr valueType = var.getValue()->getResolvedType();
    isRawPointer = valueType && valueType->isRawPointer();
  }
  if (isRawPointer) {
    rawPointerLocals_.insert(var.getName());
  } else {
    rawPointerLocals_.erase(var.getName());
  }

  // `var r: ref T = <lvalue>` binds a borrow, not a value: nothing moves out
  // of the target, and the loan is tracked like `ref r = <lvalue>`.
  TypePtr declaredType = var.getResolvedType();
  if (var.getValue() && declaredType && declaredType->isReference()) {
    auto valueType = var.getValue()->getResolvedType();
    if (valueType && !valueType->isReference()) {
      checkBorrowBinding(var.getName(), *var.getValue(),
                         isMutableRef(declaredType), var.getLocation());
      return;
    }
    // The initializer is itself a ref: a call handing back a borrow of its
    // inputs, or another ref variable being aliased. Track what the new name
    // could point into so writes to those variables are rejected while it
    // lives.
    const ExprAST* init = var.getValue();
    while (init->getType() == ASTNodeType::PAREN_EXPR) {
      init = static_cast<const ParenExprAST&>(*init).getInner();
    }
    if (init->getType() == ASTNodeType::CALL) {
      borrowRefCallInputs(var.getName(),
                          static_cast<const CallExprAST&>(*init),
                          isMutableRef(declaredType), var.getLocation());
    } else if (init->getType() == ASTNodeType::VARIABLE_REFERENCE) {
      checkBorrowBinding(var.getName(), *init, isMutableRef(declaredType),
                         var.getLocation());
      return;
    }
  }

  // Move semantics for temporaries and compound types
  if (var.getValue()) {
    auto srcType = var.getValue()->getResolvedType();

    // Mark ALL temporaries as moved when assigned to a variable.
    // The variable takes ownership, so the temporary's deinit must be skipped.
    if (var.getValue()->isTemporary()) {
      var.getValue()->setMoved(true);
    }
    // For variable references of compound types, mark as moved
    else if (srcType && srcType->isCompound() &&
             var.getValue()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
      const auto& srcRef =
          static_cast<const VariableReferenceAST&>(*var.getValue());
      if (checkMoveAllowed(srcRef.getName(), srcRef.getLocation()) &&
          checkFieldsIntact(srcRef.getName(), srcRef.getLocation())) {
        recordMove(srcRef.getName(), srcRef.getLocation());
        clearFieldPaths(srcRef.getName());
      }
    }
    // A field read moves the field out of its object
    else {
      noteFieldMove(*var.getValue());
    }
  }

  // Check for storing a reference with local lifetime (from call with temp
  // args) This catches: var r = foo(ref Temp()); where foo returns ref
  if (var.getValue()) {
    TypePtr varType = var.getValue()->getResolvedType();
    if (varType && varType->isReference()) {
      // Variable is being initialized with a reference type
      // A ref into a named local (e.g. `items.borrow(i)` on a local Vec) is
      // fine: it lives as long as that variable. Only temporaries die here.
      Lifetime lt = inferExprLifetime(*var.getValue());
      if (lt.isLocal() && lt.getName().rfind("$temp", 0) == 0) {
        const auto& pos = var.getLocation();
        reportError("cannot store reference with local lifetime in variable '" +
                        var.getName() +
                        "' - the referenced value will be destroyed after this "
                        "statement", pos);
      }
    }
  }
}

void BorrowChecker::checkReferenceCreation(const ReferenceCreationAST& ref) {
  checkBorrowBinding(ref.getName(), *ref.getTarget(), ref.isMutable(),
                     ref.getLocation());
}

// Borrowing a place that was moved out of: the field itself, or the whole
// object while one of its fields is gone
void BorrowChecker::checkBorrowTargetIntact(const ExprAST& target,
                                            const Position& refPos) {
  const std::string* targetVarName = getBaseVariableName(target);
  if (!targetVarName) return;
  std::string targetPath = fieldPath(target);
  if (!targetPath.empty() && movedVariables_.count(targetPath)) {
    reportError("cannot borrow moved field '" + targetPath +
                    "'. It was moved out of its object; assign a value back "
                    "into it first",
                refPos);
  } else if (targetPath == *targetVarName) {
    checkFieldsIntact(*targetVarName, refPos);
  }
}

void BorrowChecker::checkBorrowBinding(const std::string& refName,
                                       const ExprAST& targetExpr,
                                       bool isMutable, const Position& refPos) {
  // A conditional borrow binds whichever branch runs, so both are borrowed
  // for the ref's lifetime. Branches of the same object share one loan -
  // borrows are tracked per base variable, so a second one would collide.
  if (targetExpr.getType() == ASTNodeType::TERNARY) {
    const auto& ternary = static_cast<const TernaryExprAST&>(targetExpr);
    const std::string* thenBase = getBaseVariableName(*ternary.getThen());
    const std::string* elseBase = getBaseVariableName(*ternary.getElse());
    checkBorrowBinding(refName, *ternary.getThen(), isMutable, refPos);
    if (!thenBase || !elseBase || *thenBase != *elseBase) {
      checkBorrowBinding(refName, *ternary.getElse(), isMutable, refPos);
    } else {
      checkBorrowTargetIntact(*ternary.getElse(), refPos);
    }
    return;
  }

  // The borrow is tracked against the base variable of the target lvalue
  // (ref r = obj.field borrows obj as a whole - conservative but sound)
  const std::string* targetVarName = getBaseVariableName(targetExpr);
  if (!targetVarName) {
    reportError("reference must be bound to a variable, not a temporary",
                refPos);
    return;
  }

  checkBorrowTargetIntact(targetExpr, refPos);

  // Resolve the actual target and check rebinding rules
  auto targetInfo = resolveRefTarget(*targetVarName);

  BorrowKind kind = isMutable ? BorrowKind::Mutable : BorrowKind::Shared;

  // Rebinding rules: mutable -> immutable is OK, immutable -> mutable is NOT
  if (targetInfo.isRebind) {
    if (targetInfo.sourceBorrowKind == BorrowKind::Shared &&
        kind == BorrowKind::Mutable) {
      reportError("cannot create mutable reference '" + refName +
                      "' from immutable reference '" + *targetVarName + "'", refPos);
      return;
    }
    // Downgrade: if source is immutable, new ref must also be immutable
    if (targetInfo.sourceBorrowKind == BorrowKind::Shared) {
      kind = BorrowKind::Shared;
    }

    // Rebinding just creates an alias - no new loan needed
    // The original loan controls the borrow lifetime
    refVariables_[refName] = {targetInfo.actualTarget, kind};
    return;
  }

  // Attempt to create the borrow
  auto result = state_.addBorrow(targetInfo.actualTarget, refName, kind,
                                 currentScope_, refPos);

  if (!result.allowed) {
    reportConflict(result.errorMessage, refPos,
                   result.conflictingLoan);
  } else {
    // Track this reference with its borrow kind
    refVariables_[refName] = {targetInfo.actualTarget, kind};
  }
}

// A call that returns a borrow (`longest(a, b)`, `vec.get(i)`) hands back a
// reference into one of its by-ref inputs. Which one is not knowable here,
// so binding the result to a name conservatively borrows every named
// variable passed by ref - the receiver of a method call included. An input
// that is already a ref (a ref variable or ref parameter) is a reborrow: the
// loan it made governs the storage, so no new loan is taken.
void BorrowChecker::borrowRefCallInputs(const std::string& refName,
                                        const CallExprAST& call,
                                        bool isMutable, const Position& refPos) {
  BorrowKind kind = isMutable ? BorrowKind::Mutable : BorrowKind::Shared;
  bool tracked = false;

  auto borrowInput = [&](const ExprAST& input) {
    const std::string* base = getBaseVariableName(input);
    if (!base) return;
    // Raw pointers are the unsafe escape hatch: storage the checker cannot see
    if (rawPointerLocals_.count(*base)) return;
    TypePtr inputType = input.getResolvedType();
    if (inputType && inputType->isRawPointer()) return;

    auto targetInfo = resolveRefTarget(*base);
    if (!targetInfo.isRebind) {
      auto result = state_.addBorrow(targetInfo.actualTarget, refName, kind,
                                     currentScope_, refPos);
      if (!result.allowed) {
        reportConflict(result.errorMessage, refPos,
                       result.conflictingLoan);
        return;
      }
    }
    // The name is a live ref either way; writes through it and rebinding
    // consult this entry. One target suffices - the loans carry the rest.
    if (!tracked) {
      refVariables_[refName] = {targetInfo.actualTarget, kind};
      tracked = true;
    }
  };

  // Method receiver: `obj.method(...)` returning ref borrows from obj
  const ExprAST* callee = call.getCallee();
  if (callee && callee->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto& access = static_cast<const MemberAccessAST&>(*callee);
    if (access.getObject()) borrowInput(*access.getObject());
  }

  // Arguments bound to ref parameters
  std::vector<TypePtr> paramTypes;
  if (TypePtr calleeType = callee ? callee->getResolvedType() : nullptr) {
    if (auto* lambdaType = dynamic_cast<const LambdaType*>(calleeType.get())) {
      paramTypes = lambdaType->getParamTypes();
    } else if (auto* funcType =
                   dynamic_cast<const FunctionType*>(calleeType.get())) {
      paramTypes = funcType->getParamTypes();
    }
  }
  const auto& args = call.getArgs();
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    if (!args[i] || !paramTypes[i] || !paramTypes[i]->isReference()) continue;
    borrowInput(*args[i]);
  }
}

// Shared write-side check for assigning to a named variable (plain or
// compound assignment). valueType is the type of the value being written.
void BorrowChecker::checkVariableWrite(const std::string& varName,
                                       const TypePtr& valueType,
                                       const Position& pos) {
  // Check if this is a ref or a regular variable
  auto refIt = refVariables_.find(varName);
  if (refIt != refVariables_.end()) {
    // Assigning through a reference
    auto result = state_.canMutateThroughRef(varName);
    if (!result.allowed) {
      reportConflict(result.errorMessage, pos,
                     result.conflictingLoan);
    }
  } else if (refTypedParams_.count(varName)) {
    // This is a reference parameter - assigning through it
    // For ref params, we track them differently since they don't have
    // a local borrow entry but do allow mutation
    // TODO: More sophisticated tracking for ref params
  } else {
    // Direct write to a variable that may be borrowed. Overwriting a
    // compound value drops the storage every live borrow points into, so it
    // is rejected while any borrow is active. A scalar write leaves the
    // storage in place - live borrows simply observe the new value - so it
    // stays legal (ownership.mdx documents both).
    if constexpr (Config::STRICT_MUTATION_CHECKING) {
      if (valueType && valueType->isCompound()) {
        auto result = state_.canMutateDirectly(varName);
        if (!result.allowed) {
          reportConflict(result.errorMessage, pos,
                         result.conflictingLoan);
        }
      }
    }
  }
}

void BorrowChecker::checkVariableAssignment(
    const VariableAssignmentAST& assign) {
  const std::string& varName = assign.getName();

  // Check the value expression first
  if (assign.getValue()) {
    checkExpr(*assign.getValue());
    if (isFrameBoundExpr(*assign.getValue())) {
      size_t envDepth = inferEnvDepth(*assign.getValue());
      // A module-level global (marked with a qualified name by sema)
      // outlives this frame, so a frame-bound value cannot be stored there
      if (!assign.getQualifiedName().empty() || nameOutlivesFrame(varName)) {
        reportError(
            "cannot assign this value to '" + varName +
                "' - it holds a lambda whose captured environment lives in "
                "this frame, and '" + varName + "' outlives it. Give the "
                "lambda its data as arguments instead of captures",
            assign.getLocation());
      } else if (checkFrameStoreDepth(varName, envDepth,
                                      assign.getLocation())) {
        // A frame-bound value (see frameBoundVars_) keeps its restriction
        // when reassigned to another local
        auto [it, inserted] = frameBoundVars_.try_emplace(varName, envDepth);
        if (!inserted) it->second = std::max(it->second, envDepth);
      }
    }
    // A frame-sourced lambda keeps its restriction through reassignment
    // too. A destination that outlives the frame, or that was declared in
    // an outer scope than the environment, would dangle.
    if (isFrameSourcedLambdaExpr(*assign.getValue())) {
      size_t envDepth = inferEnvDepth(*assign.getValue());
      if (nameOutlivesFrame(varName)) {
        reportError(
            "cannot store this lambda in '" + varName +
                "' - its captured environment lives in this frame, and '" +
                varName + "' outlives it. Give the lambda its data as "
                "arguments instead of captures",
            assign.getLocation());
      } else if (checkFrameStoreDepth(varName, envDepth,
                                      assign.getLocation())) {
        auto [it, inserted] =
            frameSourcedLambdas_.try_emplace(varName, envDepth);
        if (!inserted) it->second = std::max(it->second, envDepth);
      }
    }
    // A value whose type names a lifetime, written through a name that
    // outlives the frame (a ref parameter): the destination must carry
    // the same name - see the field-store rule in checkMemberAssignment
    if (!isFrameSourcedLambdaExpr(*assign.getValue())) {
      LifetimeValue env = inferEnvLifetimeValue(*assign.getValue());
      if (env.kind == LifetimeValue::Kind::Symbolic &&
          nameOutlivesFrame(varName)) {
        LifetimeValue dst = destLifetimeValueForName(varName);
        if (!lifetimeValueOutlives(env, dst)) {
          reportError(
              "cannot store this value in '" + varName +
                  "' - its type ties it to lifetime '" + env.name +
                  ", and nothing says '" + varName + "' dies first. Give "
                  "the destination the same lifetime",
              assign.getLocation());
        }
      }
    }

    // A ref-storing class value must not land in a holder declared in an
    // outer scope than what it borrows (issue #178, Rule 5's gap).
    trackRefHolderStore(
        varName,
        nameOutlivesFrame(varName) ? 0 : lookupDeclDepth(varName),
        *assign.getValue(), assign.getLocation());
  }

  // Move semantics: if assigning from a compound-typed variable, mark source as
  // moved. Compound types (classes, interfaces) get moved, not copied.
  if (assign.getValue() &&
      assign.getValue()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& srcRef =
        static_cast<const VariableReferenceAST&>(*assign.getValue());
    auto srcType = assign.getValue()->getResolvedType();
    if (srcType && srcType->isCompound() &&
        checkMoveAllowed(srcRef.getName(), srcRef.getLocation()) &&
        checkFieldsIntact(srcRef.getName(), srcRef.getLocation())) {
      recordMove(srcRef.getName(), srcRef.getLocation());
      clearFieldPaths(srcRef.getName());
      assign.getValue()->setMoved(true);  // Mark for codegen to skip deinit
    }
  } else if (assign.getValue()) {
    noteFieldMove(*assign.getValue());
  }

  // Overwriting a frozen discriminant or a match binding is rejected the
  // same way as moving it (the arms borrow the payload in place)
  checkMoveAllowed(varName, assign.getLocation());

  // The variable holds a new value: whatever was moved out of the old one is
  // no longer missing
  movedVariables_.erase(varName);
  clearFieldPaths(varName);

  checkVariableWrite(varName,
                     assign.getValue() ? assign.getValue()->getResolvedType()
                                       : nullptr,
                     assign.getLocation());
}

void BorrowChecker::checkCompoundAssignment(
    const CompoundAssignmentAST& assign) {
  // The value is only read - compound ops work on scalars, so no move
  // semantics apply (`x += y` must not mark y as moved)
  if (assign.getValue()) {
    checkExpr(*assign.getValue());
  }

  // The target is read (use-after-move, read-through-ref checks) ...
  checkExpr(*assign.getTarget());

  // ... and written
  if (assign.getTarget()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    checkVariableWrite(
        static_cast<const VariableReferenceAST&>(*assign.getTarget())
            .getName(),
        assign.getTarget()->getResolvedType(),
        assign.getTarget()->getLocation());
  }
  // Member/indexed targets have no write-side checks today (parity with
  // checkMemberAssignment/checkIndexedAssignment)
}

void BorrowChecker::checkVariableReference(const VariableReferenceAST& varRef) {
  const std::string& name = varRef.getName();

  // Check for use-after-move
  if (movedVariables_.count(name)) {
    const auto& pos = varRef.getLocation();
    reportError("use of moved variable '" + name +
                    "'. Ownership was transferred in a previous assignment.", pos);
  }

  // Using the object as a whole (a by-value move, a borrow, a method call)
  // while one of its fields is moved out. Reaching a sibling field is fine,
  // and those reads run with fieldBaseDepth_ raised.
  if (fieldBaseDepth_ == 0) {
    checkFieldsIntact(name, varRef.getLocation());
  }

  // Check if reading through a reference
  auto refIt = refVariables_.find(name);
  if (refIt != refVariables_.end()) {
    auto result = state_.canRead(refIt->second.first, name);
    if (!result.allowed) {
      reportError(result.errorMessage, varRef.getLocation());
    }
  }
  // Reading a non-ref variable is always allowed
}

void BorrowChecker::checkBinaryExpr(const BinaryExprAST& binary) {
  if (binary.getLHS()) {
    checkExpr(*binary.getLHS());
  }
  if (binary.getRHS()) {
    checkExpr(*binary.getRHS());
  }
}

// The one message for a frame-bound value handed to a call by value
void BorrowChecker::reportFrameBoundEscapeThroughCall(const Position& pos) {
  reportError(
      "cannot pass this value to a call by value - it holds a lambda whose "
      "captured environment lives in this frame, and the callee could keep "
      "it beyond the frame's lifetime. Pass it by ref, or give the lambda "
      "its data as arguments instead of captures",
      pos);
}

// A frame-bound compound value (a Thread handle from spawn, or a local one
// was moved into) must not cross a call boundary by value: nothing in the
// parameter's type says the value is frame-bound, so the callee could keep
// it - in a field, a container element, a global - past this frame's death.
// A '[ref]' lambda itself may cross: its parameter type carries the frame
// binding, and the callee is checked under the same rules. An argument with
// no resolved parameter type is treated as by-value, conservatively.
void BorrowChecker::forbidFrameBoundByValueArgs(
    const CallExprAST& call, const std::vector<TypePtr>& paramTypes) {
  const auto& args = call.getArgs();
  for (size_t i = 0; i < args.size(); ++i) {
    if (!args[i]) continue;
    if (i < paramTypes.size() && paramTypes[i] &&
        paramTypes[i]->isReference()) {
      continue;  // a borrow cannot be kept by the callee
    }
    if (isFrameBoundExpr(*args[i])) {
      reportFrameBoundEscapeThroughCall(args[i]->getLocation());
    }
  }
}

// A frame-sourced lambda passed by value may be kept by the callee - but
// only somewhere the callee can reach: its receiver, or an argument passed
// by mutable ref. Each such destination must die with this frame (else the
// lambda would dangle), and becomes frame-bound when it does. This is what
// keeps a callee from storing OUR locals' environment up into a caller's
// object; storing a caller-sourced callback stays legal because parameters
// are never frame-sourced.
void BorrowChecker::checkFrameSourcedLambdaArgs(
    const CallExprAST& call, const std::vector<TypePtr>& paramTypes) {
  const auto& args = call.getArgs();
  bool hasFrameSourced = false;
  size_t envDepth = functionScopeDepth_;
  for (size_t i = 0; i < args.size(); ++i) {
    if (!args[i]) continue;
    if (i < paramTypes.size() && paramTypes[i]) {
      if (paramTypes[i]->isReference()) continue;
      // A parameter with a NAMED lifetime says exactly where its value may
      // go; checkNamedLifetimesAtCall enforces that, so the blanket rule
      // stands down for it
      if (auto* lt = tryGetType<LambdaType>(paramTypes[i])) {
        if (!lt->getLifetimeName().empty()) continue;
      }
    }
    if (isFrameSourcedLambdaExpr(*args[i])) {
      hasFrameSourced = true;
      envDepth = std::max(envDepth, inferEnvDepth(*args[i]));
    }
  }
  if (!hasFrameSourced) return;

  // The receiver. A receiver with no named base is a temporary that dies
  // with the statement, which cannot outlive the lambda's environment.
  const ExprAST* callee = call.getCallee();
  if (callee && callee->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto& access = static_cast<const MemberAccessAST&>(*callee);
    const std::string* base = access.getObject()
                                  ? getBaseVariableName(*access.getObject())
                                  : nullptr;
    if (base) noteFrameSourcedLambdaStore(*base, envDepth, call.getLocation());
  }

  // Arguments passed by mutable ref (a const ref cannot be written)
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    if (!args[i] || !paramTypes[i] || !paramTypes[i]->isReference()) continue;
    if (!isMutableRef(paramTypes[i])) continue;
    const std::string* base = getBaseVariableName(*args[i]);
    if (base) {
      noteFrameSourcedLambdaStore(*base, envDepth, args[i]->getLocation());
    }
  }
}

void BorrowChecker::checkCallExpr(const CallExprAST& call) {
  // Check callee
  if (call.getCallee()) {
    checkExpr(*call.getCallee());
  }

  // Check arguments
  for (const auto& arg : call.getArgs()) {
    if (arg) {
      checkExpr(*arg);
    }
  }

  // Move semantics: when passing a compound-typed variable BY VALUE to a
  // function, the source variable is moved and cannot be used afterward.
  // Get the callee's function type to check if params are by-value or by-ref.
  const ExprAST* callee = call.getCallee();
  if (!callee) return;

  TypePtr calleeType = callee->getResolvedType();
  if (!calleeType) return;

  const auto& args = call.getArgs();

  // Enum variant construction (EnumName.Variant(args...)): sema resolves the
  // callee to the enum type. Every compound payload argument MOVES into the
  // enum — Sun never implicitly copies compound values.
  if (calleeType->isEnum()) {
    // Every payload parameter is by-value; a frame-bound payload would let
    // the enum value smuggle it onward
    forbidFrameBoundByValueArgs(call, {});
    for (const auto& arg : args) {
      if (!arg) continue;
      TypePtr argType = arg->getResolvedType();
      if (!argType || !argType->isCompound()) continue;
      if (arg->getType() != ASTNodeType::VARIABLE_REFERENCE) {
        noteFieldMove(*arg);
        continue;
      }
      const auto& varRef = static_cast<const VariableReferenceAST&>(*arg);
      if (checkMoveAllowed(varRef.getName(), varRef.getLocation()) &&
          checkFieldsIntact(varRef.getName(), varRef.getLocation())) {
        recordMove(varRef.getName(), varRef.getLocation());
        clearFieldPaths(varRef.getName());
        arg->setMoved(true);
      }
    }
    return;
  }

  // Constructor call (ClassName(args...)): resolve the init overload and move
  // compound arguments bound to by-value parameters.
  std::vector<TypePtr> paramTypes;
  if (calleeType->isClass()) {
    std::vector<TypePtr> argTypes;
    for (const auto& arg : args) {
      argTypes.push_back(arg ? arg->getResolvedType() : nullptr);
    }
    const ClassMethod* init = static_cast<const ClassType&>(*calleeType)
                                  .getMethodForArgs("init", argTypes);
    if (!init) return;
    paramTypes = init->paramTypes;

    // A class that stores references keeps pointing into its by-ref
    // constructor arguments after the call returns, so the constructed value
    // holds a loan on each of them - conservatively until scope exit, since
    // the object can live that long. The loan is what rejects a later move
    // or replacement of the borrowed variable, and recordMove/
    // canMutateDirectly report it with this construction site as the note.
    if (classStoresRefs(calleeType)) {
      for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
        if (!args[i] || !paramTypes[i] || !paramTypes[i]->isReference()) {
          continue;
        }
        const std::string* base = getBaseVariableName(*args[i]);
        if (!base || rawPointerLocals_.count(*base)) continue;
        auto targetInfo = resolveRefTarget(*base);
        if (targetInfo.isRebind) continue;  // reborrow: original loan governs
        const auto& argPos = args[i]->getLocation();
        std::string holder = "$refholder@" + std::to_string(argPos.line) +
                             ":" + std::to_string(argPos.column);
        auto result = state_.addBorrow(
            targetInfo.actualTarget, holder,
            isMutableRef(paramTypes[i]) ? BorrowKind::Mutable
                                        : BorrowKind::Shared,
            currentScope_, argPos);
        if (!result.allowed) {
          reportConflict(result.errorMessage, argPos,
                         result.conflictingLoan);
        }
      }
    }
  } else if (auto* funcType =
                 dynamic_cast<const FunctionType*>(calleeType.get())) {
    // Direct calls and method calls carry a FunctionType
    paramTypes = funcType->getParamTypes();
  } else if (auto* lambdaCallee =
                 dynamic_cast<const LambdaType*>(calleeType.get())) {
    // A call through a lambda value: its parameter types serve the escape
    // checks. (Move semantics for lambda-callee arguments are a
    // pre-existing gap, left as they are.)
    forbidFrameBoundByValueArgs(call, lambdaCallee->getParamTypes());
    checkFrameSourcedLambdaArgs(call, lambdaCallee->getParamTypes());
    checkNamedLifetimesAtCall(call, lambdaCallee->getParamTypes());
    return;
  } else {
    return;
  }

  forbidFrameBoundByValueArgs(call, paramTypes);
  checkFrameSourcedLambdaArgs(call, paramTypes);
  checkNamedLifetimesAtCall(call, paramTypes);

  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    const auto& arg = args[i];
    const auto& paramType = paramTypes[i];

    if (!arg) continue;

    // A field handed to a by-value parameter moves out of its object
    if (paramType && !paramType->isReference()) {
      noteFieldMove(*arg);
    }

    // Only check variable references (not complex expressions)
    if (arg->getType() != ASTNodeType::VARIABLE_REFERENCE) continue;

    const auto& varRef = static_cast<const VariableReferenceAST&>(*arg);
    TypePtr argType = arg->getResolvedType();

    // If argument is compound type AND parameter is NOT a reference (by-value)
    // then we move the argument
    if (argType && argType->isCompound() && !paramType->isReference() &&
        checkMoveAllowed(varRef.getName(), varRef.getLocation()) &&
        checkFieldsIntact(varRef.getName(), varRef.getLocation())) {
      recordMove(varRef.getName(), varRef.getLocation());
      clearFieldPaths(varRef.getName());
      arg->setMoved(true);  // Mark for codegen to skip deinit
    }
  }
}

void BorrowChecker::checkIfExpr(const IfExprAST& ifExpr) {
  // Check condition
  if (ifExpr.getCond()) {
    checkExpr(*ifExpr.getCond());
  }

  // Branches are alternatives: each is checked against the pre-if move
  // state, and only moves on branches that fall through are unioned into
  // the state after the if. A branch that returns/throws cannot poison the
  // code that follows.
  auto movedBefore = movedVariables_;
  auto movedAfter = movedVariables_;

  // Check then branch in its own scope
  enterScope();
  if (ifExpr.getThen()) {
    checkExpr(*ifExpr.getThen());
    if (!exprDiverges(*ifExpr.getThen())) {
      movedAfter.insert(movedVariables_.begin(), movedVariables_.end());
    }
  }
  exitScope();

  // Check else branch in its own scope
  if (ifExpr.getElse()) {
    movedVariables_ = movedBefore;
    enterScope();
    checkExpr(*ifExpr.getElse());
    if (!exprDiverges(*ifExpr.getElse())) {
      movedAfter.insert(movedVariables_.begin(), movedVariables_.end());
    }
    exitScope();
  }

  movedVariables_ = std::move(movedAfter);
}

void BorrowChecker::checkTernaryExpr(const TernaryExprAST& ternary) {
  checkExpr(*ternary.getCond());

  // Check each branch in its own scope
  enterScope();
  checkExpr(*ternary.getThen());
  exitScope();

  enterScope();
  checkExpr(*ternary.getElse());
  exitScope();
}

void BorrowChecker::checkMatchExpr(const MatchExprAST& matchExpr) {
  // Check discriminant expression
  if (matchExpr.getDiscriminant()) {
    checkExpr(*matchExpr.getDiscriminant());
  }

  // A named discriminant is frozen for the whole match: its payloads may be
  // borrowed by arm bindings, so it must not be reassigned or moved.
  const std::string* discName = nullptr;
  if (matchExpr.getDiscriminant()) {
    discName = getBaseVariableName(*matchExpr.getDiscriminant());
  }
  bool discNewlyFrozen = discName && !frozenDiscriminants_.count(*discName);
  if (discNewlyFrozen) frozenDiscriminants_.insert(*discName);

  // Each arm is checked in its own scope against the SAME pre-match move
  // state (arms are alternatives, not a sequence); moves performed inside
  // any arm are unioned afterwards, conservatively.
  auto movedBefore = movedVariables_;
  auto movedAfter = movedVariables_;
  for (const auto& arm : matchExpr.getArms()) {
    movedVariables_ = movedBefore;
    enterScope();
    if (arm.pattern) {
      checkExpr(*arm.pattern);
    }
    // Compound payload bindings borrow the payload slot in place and can
    // never be moved out; scalar bindings are plain copies. A borrowed
    // binding lives as long as the matched value does, so a ref derived
    // from it (e.g. `items.borrow(i)`) may be returned when the
    // discriminant is a parameter or `this`.
    std::vector<std::string> armBorrows;
    std::vector<std::pair<std::string, std::optional<Lifetime>>> savedLifetimes;
    for (const auto& binding : arm.bindings) {
      if (binding.isWildcard) continue;
      // A reference payload is an address the enum borrowed from elsewhere,
      // so the binding does not live and die with the matched value.
      if (binding.resolvedType && binding.resolvedType->isReference()) {
        savedLifetimes.emplace_back(binding.name,
                                    state_.getLifetime(binding.name));
        state_.setLifetime(binding.name, Lifetime::static_());
        continue;
      }
      if (binding.resolvedType && binding.resolvedType->isCompound()) {
        if (matchBorrowedBindings_.insert(binding.name).second) {
          armBorrows.push_back(binding.name);
        }
        if (matchExpr.getDiscriminant()) {
          savedLifetimes.emplace_back(binding.name,
                                      state_.getLifetime(binding.name));
          state_.setLifetime(binding.name,
                             inferExprLifetime(*matchExpr.getDiscriminant()));
        }
      }
    }
    if (arm.body) {
      checkExpr(*arm.body);
    }
    for (const auto& name : armBorrows) matchBorrowedBindings_.erase(name);
    for (const auto& [name, previous] : savedLifetimes) {
      if (previous) {
        state_.setLifetime(name, *previous);
      } else {
        state_.clearLifetime(name);
      }
    }
    exitScope();
    // An arm that returns or throws never reaches the code after the match,
    // so its moves must not leak into the post-match state.
    if (!arm.body || !exprDiverges(*arm.body)) {
      movedAfter.insert(movedVariables_.begin(), movedVariables_.end());
    }
  }
  movedVariables_ = std::move(movedAfter);

  if (discNewlyFrozen) frozenDiscriminants_.erase(*discName);
}

// Moving out of a match binding would take ownership of a payload the enum
// still owns (double drop); moving/reassigning a frozen discriminant would
// invalidate borrows held by its arms.
bool BorrowChecker::checkMoveAllowed(const std::string& name,
                                     const Position& pos) {
  if (matchBorrowedBindings_.count(name)) {
    reportError("cannot move out of match binding '" + name +
                    "' — it borrows the matched value's payload; use it in "
                    "place or pass it by ref",
                pos);
    return false;
  }
  if (frozenDiscriminants_.count(name)) {
    reportError("cannot move '" + name +
                    "' while it is being matched — its payloads are borrowed "
                    "by the match arms", pos);
    return false;
  }
  return true;
}

std::string BorrowChecker::fieldPath(const ExprAST& expr) const {
  if (expr.getType() == ASTNodeType::VARIABLE_REFERENCE) {
    return static_cast<const VariableReferenceAST&>(expr).getName();
  }
  // A method's receiver: its fields are tracked like any other object's
  if (expr.getType() == ASTNodeType::THIS) return "this";
  if (expr.getType() != ASTNodeType::MEMBER_ACCESS) return "";

  const auto& access = static_cast<const MemberAccessAST&>(expr);
  if (access.isBoundMethodRef() || access.hasQualifiedName()) return "";

  const ExprAST* object = access.getObject();
  if (!object) return "";

  // Only a field of a class value: an enum constant (Option.None) or a module
  // member is not a place that can be moved out of.
  TypePtr objectType = object->getResolvedType();
  if (objectType && objectType->isReference()) {
    objectType =
        static_cast<const ReferenceType&>(*objectType).getReferencedType();
  }
  if (objectType && objectType->isClass()) {
    if (!static_cast<const ClassType&>(*objectType)
             .getField(access.getMemberName())) {
      return "";
    }
  } else if (object->getType() != ASTNodeType::THIS) {
    // `this` carries no resolved type on the node; anything else without a
    // class type is not a field of an object
    return "";
  }

  std::string base = fieldPath(*object);
  if (base.empty()) return "";
  return base + "." + access.getMemberName();
}

void BorrowChecker::noteFieldMove(const ExprAST& value) {
  TypePtr type = value.getResolvedType();
  if (!type || !type->isCompound()) return;

  std::string path = fieldPath(value);
  // A plain variable move is recorded by the caller; only field paths here
  if (path.empty() || path.find('.') == std::string::npos) return;

  recordMove(path, value.getLocation());
  clearFieldPaths(path);
}

void BorrowChecker::clearFieldPaths(const std::string& base) {
  const std::string prefix = base + ".";
  for (auto it = movedVariables_.begin(); it != movedVariables_.end();) {
    if (it->rfind(prefix, 0) == 0) {
      it = movedVariables_.erase(it);
    } else {
      ++it;
    }
  }
}

const std::string* BorrowChecker::movedFieldOf(const std::string& name) const {
  const std::string prefix = name + ".";
  for (const auto& moved : movedVariables_) {
    if (moved.rfind(prefix, 0) == 0) return &moved;
  }
  return nullptr;
}

// Does this type point into storage it does not own? Either a class holding
// a reference anywhere in its fields, or any type that can carry a '[ref]'
// lambda's captured environment (a [ref] field, a container instantiated
// over a [ref] lambda type, a payload enum holding one). Such a value is
// itself borrow-like and must not leave the frame.
bool BorrowChecker::classStoresRefs(const TypePtr& type) const {
  if (sun::typeIsFrameCarrying(type)) return true;
  std::unordered_set<const Type*> visited;
  return classStoresRefsWalk(type, visited);
}

bool BorrowChecker::classStoresRefsWalk(
    const TypePtr& type, std::unordered_set<const Type*>& visited) const {
  const auto* classType = tryGetType<ClassType>(type);
  if (!classType || !visited.insert(classType).second) return false;
  for (const auto& field : classType->getFields()) {
    if (!field.type) continue;
    if (field.type->isReference()) return true;
    if (classStoresRefsWalk(field.type, visited)) return true;
  }
  return false;
}

// Every move funnels through here. Moving a borrowed place would leave the
// live borrows reading the zeroed husk the move leaves behind, so it is
// rejected; a loan on the base variable covers its fields too.
void BorrowChecker::recordMove(const std::string& place, const Position& pos) {
  std::string base = place.substr(0, place.find('.'));
  auto loans = state_.getActiveLoans(base);
  if (!loans.empty()) {
    std::string why = base == place ? "it is borrowed"
                                    : "'" + base + "' is borrowed";
    reportConflict("cannot move out of '" + place + "' because " + why, pos, loans.front());
    return;  // not marked moved: keep follow-on errors focused on this one
  }
  movedVariables_.insert(place);
  moveLocations_[place] = pos;
}

void BorrowChecker::noteLoopLocal(const std::string& name) {
  for (auto& frame : loopLocals_) frame.insert(name);
}

void BorrowChecker::checkLoopBody(const ExprAST* body,
                                  const std::vector<std::string>& loopVars) {
  if (!body) return;

  auto movedBefore = movedVariables_;
  loopLocals_.emplace_back();
  for (const auto& v : loopVars) {
    loopLocals_.back().insert(v);
    frameLocalNames_.insert(v);
    declDepths_[v] = currentScope_;
  }

  checkExpr(*body);

  auto declared = std::move(loopLocals_.back());
  loopLocals_.pop_back();

  // A body that always returns or throws runs at most once
  if (exprDiverges(*body)) return;

  // Whatever is still moved when the body ends was moved out of something
  // that outlives the iteration: the next pass would move it again.
  std::vector<std::string> carried;
  for (const auto& place : movedVariables_) {
    if (movedBefore.count(place)) continue;
    std::string base = place.substr(0, place.find('.'));
    if (declared.count(base)) continue;
    carried.push_back(place);
  }

  for (const auto& place : carried) {
    auto loc = moveLocations_.find(place);
    Position pos = loc != moveLocations_.end() ? loc->second : Position{};
    reportError("'" + place +
                    "' is moved inside a loop, so the next iteration would "
                    "use what is already gone. Move it once outside the loop, "
                    "borrow it with 'ref', copy it with clone(), or assign a "
                    "value back into it before the iteration ends", pos);
    // Reported once: let the rest of the function check against a whole value
    movedVariables_.erase(place);
    moveLocations_.erase(place);
  }
}

bool BorrowChecker::checkFieldsIntact(const std::string& name,
                                      const Position& pos) {
  const std::string* moved = movedFieldOf(name);
  if (!moved) return true;
  reportError("cannot use '" + name + "' as a whole: its field '" + *moved +
                  "' was moved out. Assign a value back into " + *moved +
                  " first, or borrow the field with 'ref' instead of moving "
                  "it", pos);
  return false;
}

void BorrowChecker::checkWhileExpr(const WhileExprAST& whileExpr) {
  // Check condition
  if (whileExpr.getCondition()) {
    checkExpr(*whileExpr.getCondition());
  }

  // Check body in its own scope
  enterScope();
  checkLoopBody(whileExpr.getBody());
  exitScope();
}

void BorrowChecker::checkForExpr(const ForExprAST& forExpr) {
  // Enter scope for loop variables
  enterScope();

  // Check initialization, condition, and increment expressions
  if (forExpr.getInit()) {
    checkExpr(*forExpr.getInit());
  }
  if (forExpr.getCondition()) {
    checkExpr(*forExpr.getCondition());
  }
  if (forExpr.getIncrement()) {
    checkExpr(*forExpr.getIncrement());
  }

  // Check body
  checkLoopBody(forExpr.getBody());
  exitScope();
}

void BorrowChecker::checkForInExpr(const ForInExprAST& forInExpr) {
  // Check the iterable expression first (outside the loop scope)
  if (forInExpr.getIterable()) {
    checkExpr(*forInExpr.getIterable());
  }

  // Enter scope for loop variable
  enterScope();

  // Check body; the loop variable is bound afresh on every iteration
  checkLoopBody(forInExpr.getBody(), {forInExpr.getLoopVar()});
  exitScope();
}

void BorrowChecker::checkBlockExpr(const BlockExprAST& block) {
  enterScope();
  for (const auto& stmt : block.getBody()) {
    if (stmt) {
      checkExpr(*stmt);
    }
  }
  exitScope();
}

void BorrowChecker::checkReturnStmt(const ReturnExprAST& ret) {
  const ExprAST* value = ret.getValue();
  if (!value) return;

  // Check the return value expression
  checkExpr(*value);

  // A '<'a>' return type unpins the ban: a value whose environment is
  // symbolically that same lifetime is exactly what the signature promises
  bool returnMatchesNamedLifetime = false;
  if (!returnLifetimeName_.empty()) {
    LifetimeValue env = inferEnvLifetimeValue(*value);
    returnMatchesNamedLifetime =
        env.kind == LifetimeValue::Kind::Symbolic &&
        env.name == returnLifetimeName_;
  }

  // A lambda whose environment is bound to this frame must not escape through
  // return — whether it borrows from the frame or owns a value the frame drops
  if (!returnMatchesNamedLifetime && isRefCapturingLambdaExpr(*value)) {
    const auto& pos = ret.getLocation();
    reportError(
        "cannot return a lambda with a capture list - its captured "
        "environment lives in this frame and dies when the function returns", pos);
  }

  // The same escape one step removed: a value such a lambda was moved into
  // (a Thread handle from spawn) is bound to this frame just as the lambda
  // is - what it captured dies when the function returns
  if (!returnMatchesNamedLifetime && isFrameBoundExpr(*value)) {
    reportError(
        "cannot return this value - it holds a lambda whose captured "
        "environment lives in this frame and dies when the function returns. "
        "Use it in this scope, or give the lambda its data as arguments "
        "instead of captures", ret.getLocation());
  }

  // Check lifetime safety for reference returns
  checkReturnLifetime(ret);

  // Move semantics for return values: when returning a compound type (class)
  // by value, the ownership transfers to the caller. Mark the return value
  // as moved so its deinit is skipped in the callee. Returning a `ref` only
  // lends the value out, so nothing moves.
  auto retType = value->getResolvedType();

  // A value that stores references, or carries a '[ref]' lambda's captured
  // environment, must not escape by value: what it points into lives in
  // this frame and dies when the function returns
  if (!returnMatchesNamedLifetime && retType && !retType->isReference() &&
      classStoresRefs(retType)) {
    const auto& pos = ret.getLocation();
    reportError(
        "cannot return a value that stores references or a '[ref]' lambda - "
        "what it points into lives in this frame and dies when the function "
        "returns", pos);
  }
  if (retType && retType->isCompound() && !currentFunctionReturnsRef_) {
    // Mark temporaries as moved (ownership transferred to caller)
    if (value->isTemporary()) {
      const_cast<ExprAST*>(value)->setMoved(true);
    }
    // For variable references, mark the variable as moved
    else if (value->getType() == ASTNodeType::VARIABLE_REFERENCE) {
      const auto& srcRef = static_cast<const VariableReferenceAST&>(*value);
      if (checkMoveAllowed(srcRef.getName(), srcRef.getLocation()) &&
          checkFieldsIntact(srcRef.getName(), srcRef.getLocation())) {
        recordMove(srcRef.getName(), srcRef.getLocation());
        clearFieldPaths(srcRef.getName());
        const_cast<ExprAST*>(value)->setMoved(true);  // codegen skips deinit
      }
    }
    // Returning a field moves it out of its object
    else {
      noteFieldMove(*value);
    }
  }
}

void BorrowChecker::checkFunctionDef(const FunctionAST& func) {
  const auto& proto = func.getProto();
  const std::string& funcName = proto.getName();

  // Templates are analyzed with unbound type parameters; codegen only emits
  // their specializations (analyzed clones with concrete types), so those are
  // what must be checked (move marks must land on the emitted AST).
  // A pack template is skipped even with no specializations: its body holds an
  // unexpanded `args...` that only a specialization gives meaning to.
  if (proto.isTemplate() &&
      (!func.getSpecializations().empty() || proto.hasVariadicParam())) {
    for (const auto& [name, specialized] : func.getSpecializations()) {
      if (specialized) checkFunctionDef(*specialized);
    }
    return;
  }

  // Check if function returns a reference type
  bool returnsRef = false;
  if (proto.hasReturnType()) {
    const auto& retType = *proto.getReturnType();
    returnsRef = retType.isReference();

    // Rule: Return type cannot be a reference (when config enabled)
    if constexpr (Config::FORBID_REF_RETURNS) {
      if (returnsRef) {
        reportError(
            "function '" + funcName + "' cannot return a reference type",
            func.getLocation());
      }
    }
  }

  // Enter function scope
  enterFunctionScope(funcName);
  currentFunctionReturnsRef_ = returnsRef;

  // Track reference parameters and their lifetimes. A by-value parameter
  // is storage this frame owns; a ref parameter's referent outlives it.
  for (const auto& [argName, argType] : proto.getArgs()) {
    if (argType.isReference()) {
      refTypedParams_[argName] = !argType.constRef;
      // Assign param lifetime - outlives the function body
      Lifetime paramLt = Lifetime::param(argName);
      paramLifetimes_[argName] = paramLt;
      state_.setLifetime(argName, paramLt);
    } else {
      frameLocalNames_.insert(argName);
      declDepths_[argName] = currentScope_;
    }
    // Record the signature's named lifetimes for call-site, store and
    // return checks
    if (argType.isLambda() && argType.refEnv && !argType.lifetimeName.empty()) {
      paramEnvNames_[argName] = argType.lifetimeName;
    }
    if (argType.isReference() && !argType.lifetimeName.empty()) {
      refParamNames_[argName] = argType.lifetimeName;
    }
    if (argType.isReference() && argType.elementType &&
        !argType.elementType->lifetimeArguments.empty()) {
      refParamClassBindings_[argName] =
          argType.elementType->lifetimeArguments;
    }
  }
  if (proto.hasReturnType() && proto.getReturnType()->isLambda() &&
      proto.getReturnType()->refEnv) {
    returnLifetimeName_ = proto.getReturnType()->lifetimeName;
  }

  // Check the function body (skip for extern declarations)
  if (func.hasBody()) {
    checkBlockExpr(func.getBody());
  }

  // Exit function scope - clears all borrows
  exitFunctionScope();
}

// True when the expression is (or holds) a lambda with [ref x] captures -
// such a lambda carries pointers into the enclosing frame
bool BorrowChecker::isRefCapturingLambdaExpr(const ExprAST& expr) const {
  if (expr.getType() == ASTNodeType::LAMBDA) {
    return static_cast<const LambdaAST&>(expr).getProto().hasRefCaptures();
  }
  auto type = expr.getResolvedType();
  if (type && type->isLambda()) {
    return static_cast<const sun::LambdaType*>(type.get())->hasRefCaptures();
  }
  return false;
}

// True when the expression produces (or names) a value bound to this frame
// because a capture-list lambda is buried inside it. A call that takes such
// a lambda by value may keep it — spawn stores it in the Thread handle it
// returns — so a compound call result built from one must not outlive the
// frame the lambda's environment lives in.
bool BorrowChecker::isFrameBoundExpr(const ExprAST& expr) const {
  const ExprAST* e = &expr;
  while (e->getType() == ASTNodeType::PAREN_EXPR) {
    e = static_cast<const ParenExprAST&>(*e).getInner();
  }
  if (e->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    return frameBoundVars_.count(
               static_cast<const VariableReferenceAST&>(*e).getName()) > 0;
  }
  // A literal holding a frame-bound element is frame-bound as a whole
  // (checkExpr never descends into literals, so recurse here)
  if (e->getType() == ASTNodeType::ARRAY_LITERAL) {
    for (const auto& elem : static_cast<const ArrayLiteralAST&>(*e)
                                .getElements()) {
      if (elem && isFrameBoundExpr(*elem)) return true;
    }
    return false;
  }
  if (e->getType() == ASTNodeType::STRUCT_LITERAL) {
    for (const auto& field :
         static_cast<const StructLiteralAST&>(*e).getFields()) {
      if (field.value && isFrameBoundExpr(*field.value)) return true;
    }
    return false;
  }
  // An explicitly instantiated call (Box<[ref]() -> i32>(...)) is the same
  // shape as a plain call for this purpose
  if (e->getType() == ASTNodeType::GENERIC_CALL) {
    const auto& gcall = static_cast<const GenericCallAST&>(*e);
    TypePtr resultType = gcall.getResolvedType();
    if (!resultType || !resultType->isCompound()) return false;
    for (const auto& arg : gcall.getArgs()) {
      if (arg && isRefCapturingLambdaExpr(*arg)) return true;
    }
    return false;
  }
  if (e->getType() != ASTNodeType::CALL) return false;
  const auto& call = static_cast<const CallExprAST&>(*e);
  TypePtr resultType = call.getResolvedType();
  if (!resultType || !resultType->isCompound()) return false;
  for (const auto& arg : call.getArgs()) {
    if (arg && isRefCapturingLambdaExpr(*arg)) return true;
  }
  return false;
}

// A lambda whose captured environment provably lives in THIS frame: a
// capture-list literal (its environment is built here), a bound method of a
// frame-local receiver (the receiver dies with this frame), or a local one
// of those was assigned to. A '[ref]' value received as a parameter is NOT
// frame-sourced: its environment lives in an ancestor frame that outlives
// this one, which is what lets a callee store a caller's callback.
bool BorrowChecker::isFrameSourcedLambdaExpr(const ExprAST& expr) const {
  const ExprAST* e = &expr;
  while (e->getType() == ASTNodeType::PAREN_EXPR) {
    e = static_cast<const ParenExprAST&>(*e).getInner();
  }
  if (e->getType() == ASTNodeType::LAMBDA) {
    return static_cast<const LambdaAST&>(*e).getProto().hasRefCaptures();
  }
  if (e->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    return frameSourcedLambdas_.count(
               static_cast<const VariableReferenceAST&>(*e).getName()) > 0;
  }
  if (e->getType() == ASTNodeType::MEMBER_ACCESS) {
    auto type = e->getResolvedType();
    if (type && type->isLambda() &&
        static_cast<const sun::LambdaType*>(type.get())->hasRefCaptures()) {
      const auto& access = static_cast<const MemberAccessAST&>(*e);
      const std::string* base =
          access.getObject() ? getBaseVariableName(*access.getObject())
                             : nullptr;
      return base && !nameOutlivesFrame(*base);
    }
  }
  // A call returning a named '<'a>' lambda is frame-sourced exactly
  // when the arguments bound to 'a pin it to this frame
  if (e->getType() == ASTNodeType::CALL) {
    auto type = e->getResolvedType();
    if (type && type->isLambda() &&
        static_cast<const sun::LambdaType*>(type.get())->hasRefCaptures()) {
      return inferEnvLifetimeValue(*e).kind == LifetimeValue::Kind::Concrete;
    }
  }
  return false;
}

// Does the named place outlive this frame? `this` and ref parameters point
// at storage owned elsewhere, and a name this frame never declared is a
// global. Everything else is a local or by-value parameter that dies here.
bool BorrowChecker::nameOutlivesFrame(const std::string& base) const {
  if (base == "this") return true;
  const std::string& target = resolveRefTarget(base).actualTarget;
  if (target == "this") return true;
  if (refTypedParams_.count(target)) return true;
  return frameLocalNames_.count(target) == 0;
}

// The declaration depth of the named storage. Ref aliases resolve to their
// ultimate target; anything that outlives the frame ranks as 0.
size_t BorrowChecker::lookupDeclDepth(const std::string& name) const {
  if (name == "this") return 0;
  auto info = resolveRefTarget(name);
  if (info.isRefParam || info.actualTarget == "this") return 0;
  auto it = declDepths_.find(info.actualTarget);
  return it != declDepths_.end() ? it->second : 0;
}

// The scope depth a frame-sourced value's environment is pinned to. A
// capture-list literal is pinned to the deepest declaration among its
// borrowed variables (owned captures move in and add no dependency); a
// bound method to its receiver's declaration; a tracked local to its
// recorded bound. functionScopeDepth_ means "valid anywhere in the frame".
size_t BorrowChecker::inferEnvDepth(const ExprAST& expr) const {
  const ExprAST* e = &expr;
  while (e->getType() == ASTNodeType::PAREN_EXPR) {
    e = static_cast<const ParenExprAST&>(*e).getInner();
  }
  switch (e->getType()) {
    case ASTNodeType::LAMBDA: {
      size_t depth = functionScopeDepth_;
      for (const auto& cap :
           static_cast<const LambdaAST&>(*e).getProto().getCaptures()) {
        if (cap.kind != CaptureKind::Borrow) continue;
        depth = std::max(depth, lookupDeclDepth(cap.name));
      }
      return depth;
    }
    case ASTNodeType::VARIABLE_REFERENCE: {
      const std::string& name =
          static_cast<const VariableReferenceAST&>(*e).getName();
      if (auto it = frameSourcedLambdas_.find(name);
          it != frameSourcedLambdas_.end()) {
        return std::max(functionScopeDepth_, it->second);
      }
      if (auto it = frameBoundVars_.find(name); it != frameBoundVars_.end()) {
        return std::max(functionScopeDepth_, it->second);
      }
      return functionScopeDepth_;
    }
    case ASTNodeType::MEMBER_ACCESS: {
      // A bound method's environment is its receiver
      const auto& access = static_cast<const MemberAccessAST&>(*e);
      const std::string* base = access.getObject()
                                    ? getBaseVariableName(*access.getObject())
                                    : nullptr;
      return base ? std::max(functionScopeDepth_, lookupDeclDepth(*base))
                  : functionScopeDepth_;
    }
    case ASTNodeType::ARRAY_LITERAL: {
      size_t depth = functionScopeDepth_;
      for (const auto& elem :
           static_cast<const ArrayLiteralAST&>(*e).getElements()) {
        if (!elem) continue;
        if (isFrameBoundExpr(*elem) || isFrameSourcedLambdaExpr(*elem)) {
          depth = std::max(depth, inferEnvDepth(*elem));
        }
      }
      return depth;
    }
    case ASTNodeType::STRUCT_LITERAL: {
      size_t depth = functionScopeDepth_;
      for (const auto& field :
           static_cast<const StructLiteralAST&>(*e).getFields()) {
        if (!field.value) continue;
        if (isFrameBoundExpr(*field.value) ||
            isFrameSourcedLambdaExpr(*field.value)) {
          depth = std::max(depth, inferEnvDepth(*field.value));
        }
      }
      return depth;
    }
    case ASTNodeType::CALL: {
      // A call returning a named '<'a>' lambda: the environment's
      // bound was computed from the arguments tied to 'a
      TypePtr resultType = e->getResolvedType();
      if (resultType && resultType->isLambda()) {
        LifetimeValue env = inferEnvLifetimeValue(*e);
        return env.kind == LifetimeValue::Kind::Concrete
                   ? std::max(functionScopeDepth_, env.depth)
                   : functionScopeDepth_;
      }
      // A compound result built from a capture-list lambda (spawn):
      // pinned wherever the buried environments are
      size_t depth = functionScopeDepth_;
      for (const auto& arg : static_cast<const CallExprAST&>(*e).getArgs()) {
        if (arg && isRefCapturingLambdaExpr(*arg)) {
          depth = std::max(depth, inferEnvDepth(*arg));
        }
      }
      return depth;
    }
    case ASTNodeType::GENERIC_CALL: {
      size_t depth = functionScopeDepth_;
      for (const auto& arg :
           static_cast<const GenericCallAST&>(*e).getArgs()) {
        if (arg && isRefCapturingLambdaExpr(*arg)) {
          depth = std::max(depth, inferEnvDepth(*arg));
        }
      }
      return depth;
    }
    default:
      return functionScopeDepth_;
  }
}

// A frame-sourced environment pinned at envDepth is entering the named
// frame-local destination. A destination declared in an outer scope
// outlives the environment, so the store would dangle once the inner scope
// ends - the sub-frame hole of issue #178.
bool BorrowChecker::checkFrameStoreDepth(const std::string& destBase,
                                         size_t envDepth,
                                         const Position& pos) {
  size_t destDepth = lookupDeclDepth(destBase);
  if (destDepth == 0) return true;  // outlives the frame: callers reject it
  if (destDepth < envDepth) {
    reportError(
        "cannot store this lambda in '" + destBase + "' - '" + destBase +
            "' is declared in an outer scope and outlives the lambda's "
            "captured environment. Declare '" + destBase +
            "' alongside what the lambda captures, or give the lambda its "
            "data as arguments instead of captures",
        pos);
    return false;
  }
  return true;
}

// A frame-sourced lambda is entering the named destination. A destination
// that outlives the frame - or that is declared in an outer scope than the
// lambda's environment - would let the lambda dangle, so it is rejected; an
// accepted one becomes frame-bound itself, so the carrier object cannot
// cross a call boundary and smuggle the lambda onward.
void BorrowChecker::noteFrameSourcedLambdaStore(const std::string& destBase,
                                                size_t envDepth,
                                                const Position& pos) {
  if (nameOutlivesFrame(destBase)) {
    reportError(
        "cannot store this lambda in '" + destBase +
            "' - its captured environment lives in this frame, and '" +
            destBase + "' outlives it. Give the lambda its data as "
            "arguments instead of captures",
        pos);
    return;
  }
  if (!checkFrameStoreDepth(destBase, envDepth, pos)) return;
  const std::string target = resolveRefTarget(destBase).actualTarget;
  auto [it, inserted] = frameBoundVars_.try_emplace(target, envDepth);
  if (!inserted) it->second = std::max(it->second, envDepth);
}

// A source value provably lives at least as long as the destination when
// it outlives the frame entirely, when both carry the same signature
// lifetime, or when both are pinned to scopes of this frame and the
// source's scope encloses the destination's. A symbolic source outlives
// any frame-local destination (signature lifetimes name ancestor frames),
// but never an unrelated symbolic or elided destination.
bool BorrowChecker::lifetimeValueOutlives(const LifetimeValue& src,
                                          const LifetimeValue& dst) {
  if (src.kind == LifetimeValue::Kind::Outlives) return true;
  if (dst.kind == LifetimeValue::Kind::Concrete) {
    return src.kind == LifetimeValue::Kind::Symbolic || src.depth <= dst.depth;
  }
  if (dst.kind == LifetimeValue::Kind::Symbolic) {
    return src.kind == LifetimeValue::Kind::Symbolic && src.name == dst.name;
  }
  return false;  // dst outlives the frame; src is frame-pinned or unrelated
}

// The lifetime of the captured environment an expression's value carries.
BorrowChecker::LifetimeValue BorrowChecker::inferEnvLifetimeValue(
    const ExprAST& expr) const {
  const ExprAST* e = &expr;
  while (e->getType() == ASTNodeType::PAREN_EXPR) {
    e = static_cast<const ParenExprAST&>(*e).getInner();
  }

  if (e->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const std::string& name =
        static_cast<const VariableReferenceAST&>(*e).getName();
    if (frameSourcedLambdas_.count(name) || frameBoundVars_.count(name)) {
      return {LifetimeValue::Kind::Concrete, inferEnvDepth(*e), "", name};
    }
    // A value that carries a named lifetime in its type - a parameter, or
    // a local it was copied into - stays symbolic
    TypePtr type = e->getResolvedType();
    if (auto* lt = tryGetType<LambdaType>(type)) {
      if (lt->hasRefCaptures() && !lt->getLifetimeName().empty()) {
        return {LifetimeValue::Kind::Symbolic, 0, lt->getLifetimeName(), name};
      }
      if (lt->hasRefCaptures()) {
        return {LifetimeValue::Kind::Outlives, 0, "", name};
      }
    }
    return {LifetimeValue::Kind::Outlives, 0, "", name};
  }

  if (e->getType() == ASTNodeType::LAMBDA) {
    return {LifetimeValue::Kind::Concrete, inferEnvDepth(*e), "", "lambda"};
  }

  if (e->getType() == ASTNodeType::MEMBER_ACCESS) {
    // A bound method's environment is its receiver
    TypePtr type = e->getResolvedType();
    auto* lt = tryGetType<LambdaType>(type);
    if (!lt || !lt->hasRefCaptures()) {
      return {LifetimeValue::Kind::Outlives, 0, "", ""};
    }
    const auto& access = static_cast<const MemberAccessAST&>(*e);
    const std::string* base = access.getObject()
                                  ? getBaseVariableName(*access.getObject())
                                  : nullptr;
    if (!base) return {LifetimeValue::Kind::Outlives, 0, "", ""};
    if (*base == "this" || resolveRefTarget(*base).actualTarget == "this") {
      return {LifetimeValue::Kind::Symbolic, 0, "this", *base};
    }
    auto info = resolveRefTarget(*base);
    if (info.isRefParam) {
      std::string paramName = info.actualTarget.rfind("param:", 0) == 0
                                  ? info.actualTarget.substr(6)
                                  : *base;
      auto it = refParamNames_.find(paramName);
      if (it != refParamNames_.end()) {
        return {LifetimeValue::Kind::Symbolic, 0, it->second, *base};
      }
      return {LifetimeValue::Kind::Outlives, 0, "", *base};
    }
    if (declDepths_.count(info.actualTarget)) {
      return {LifetimeValue::Kind::Concrete, inferEnvDepth(*e), "", *base};
    }
    return {LifetimeValue::Kind::Outlives, 0, "", *base};
  }

  if (e->getType() == ASTNodeType::CALL) {
    // A call returning a named '<'a>' lambda takes the shortest
    // lifetime among the arguments the signature ties to 'a
    const auto& call = static_cast<const CallExprAST&>(*e);
    TypePtr resultType = call.getResolvedType();
    auto* resultLambda = tryGetType<LambdaType>(resultType);
    if (!resultLambda || !resultLambda->hasRefCaptures()) {
      return {LifetimeValue::Kind::Outlives, 0, "", ""};
    }
    const std::string& retName = resultLambda->getLifetimeName();
    if (retName.empty()) {
      return {LifetimeValue::Kind::Outlives, 0, "", ""};
    }
    std::vector<TypePtr> paramTypes;
    TypePtr calleeType =
        call.getCallee() ? call.getCallee()->getResolvedType() : nullptr;
    if (auto* ft = tryGetType<FunctionType>(calleeType)) {
      paramTypes = ft->getParamTypes();
    } else if (auto* clt = tryGetType<LambdaType>(calleeType)) {
      paramTypes = clt->getParamTypes();
    }
    const auto& args = call.getArgs();
    bool haveConcrete = false, haveSymbolic = false, mixedSymbolic = false;
    size_t maxDepth = functionScopeDepth_;
    std::string symbolicName;
    auto addContribution = [&](const LifetimeValue& v) {
      if (v.kind == LifetimeValue::Kind::Concrete) {
        haveConcrete = true;
        maxDepth = std::max(maxDepth, v.depth);
      } else if (v.kind == LifetimeValue::Kind::Symbolic) {
        if (haveSymbolic && symbolicName != v.name) mixedSymbolic = true;
        haveSymbolic = true;
        symbolicName = v.name;
      }
    };
    for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
      if (!args[i] || !paramTypes[i]) continue;
      if (auto* plt = tryGetType<LambdaType>(paramTypes[i])) {
        if (plt->getLifetimeName() == retName) {
          addContribution(inferEnvLifetimeValue(*args[i]));
        }
      } else if (auto* prt = tryGetType<ReferenceType>(paramTypes[i])) {
        if (prt->getLifetimeName() == retName) {
          addContribution(inferDestLifetimeValue(*args[i]));
        }
      }
    }
    if (haveConcrete) {
      return {LifetimeValue::Kind::Concrete, maxDepth, "", "call result"};
    }
    if (haveSymbolic && !mixedSymbolic) {
      return {LifetimeValue::Kind::Symbolic, 0, symbolicName, "call result"};
    }
    if (haveSymbolic) {
      // Unrelated signature lifetimes met: restrict to the current scope
      return {LifetimeValue::Kind::Concrete, currentScope_, "", "call result"};
    }
    return {LifetimeValue::Kind::Outlives, 0, "", "call result"};
  }

  if (isFrameSourcedLambdaExpr(*e)) {
    return {LifetimeValue::Kind::Concrete, inferEnvDepth(*e), "", ""};
  }
  return {LifetimeValue::Kind::Outlives, 0, "", ""};
}

// The lifetime of the storage behind a plain name the callee (or this
// frame) may write into.
BorrowChecker::LifetimeValue BorrowChecker::destLifetimeValueForName(
    const std::string& base) const {
  if (base == "this") {
    return {LifetimeValue::Kind::Symbolic, 0, "this", base};
  }
  if (rawPointerLocals_.count(base)) {
    // Raw pointers are the unsafe escape hatch: nothing is rejected
    return {LifetimeValue::Kind::Concrete, static_cast<size_t>(-1), "", base};
  }
  auto info = resolveRefTarget(base);
  if (info.actualTarget == "this") {
    return {LifetimeValue::Kind::Symbolic, 0, "this", base};
  }
  if (info.isRefParam) {
    std::string paramName = info.actualTarget.rfind("param:", 0) == 0
                                ? info.actualTarget.substr(6)
                                : base;
    auto it = refParamNames_.find(paramName);
    if (it != refParamNames_.end()) {
      return {LifetimeValue::Kind::Symbolic, 0, it->second, base};
    }
    return {LifetimeValue::Kind::Outlives, 0, "", base};
  }
  auto it = declDepths_.find(info.actualTarget);
  if (it != declDepths_.end()) {
    return {LifetimeValue::Kind::Concrete, it->second, "", base};
  }
  return {LifetimeValue::Kind::Outlives, 0, "", base};  // a global
}

// The lifetime of the storage a call argument lets the callee write into.
BorrowChecker::LifetimeValue BorrowChecker::inferDestLifetimeValue(
    const ExprAST& arg) const {
  const std::string* base = getBaseVariableName(arg);
  if (!base) {
    // A temporary dies with the statement; anything outlives it
    return {LifetimeValue::Kind::Concrete, currentScope_, "", "a temporary"};
  }
  return destLifetimeValueForName(*base);
}

// Enforce a callee's named-lifetime relations at one call. Each name binds
// the arguments the signature tags with it: '<'a>' lambda parameters
// contribute environments (sources), 'ref 'a T' parameters contribute the
// storage the callee may write into (destinations), and the receiver joins
// the builtin 'this. Every source must outlive every destination sharing
// its name - this is what closes issue #178's cross-function hole: the
// entanglement inside the callee is visible here as the shared name.
void BorrowChecker::checkNamedLifetimesAtCall(
    const CallExprAST& call, const std::vector<TypePtr>& paramTypes) {
  const auto& args = call.getArgs();
  struct Entry {
    LifetimeValue value;
    Position pos;
    size_t argIndex;
  };
  std::map<std::string, std::vector<Entry>> sources, dests;
  bool usesThis = false;

  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    if (!args[i] || !paramTypes[i]) continue;
    if (auto* lt = tryGetType<LambdaType>(paramTypes[i])) {
      if (!lt->hasRefCaptures() || lt->getLifetimeName().empty()) continue;
      const std::string& name = lt->getLifetimeName();
      sources[name].push_back(
          {inferEnvLifetimeValue(*args[i]), args[i]->getLocation(), i});
      if (name == "this") usesThis = true;
    } else if (auto* rt = tryGetType<ReferenceType>(paramTypes[i])) {
      // Names this argument binds: the ref position's own name
      // ('dst: ref 'a Holder') and each class-application binding
      // ('bus: ref Bus<'this>' binds Bus's slot to 'this)
      if (rt->getLifetimeName().empty() &&
          rt->getClassLifetimeArgs().empty()) {
        continue;
      }
      const std::string* base = getBaseVariableName(*args[i]);
      // When the argument is our own ref parameter carrying slot bindings
      // ('bus: ref Bus<'a>'), the callee's slot k stands for OUR name for
      // slot k, not for the referent's frame
      const std::vector<std::string>* argBindings = nullptr;
      if (base) {
        auto info = resolveRefTarget(*base);
        std::string paramName = info.actualTarget.rfind("param:", 0) == 0
                                    ? info.actualTarget.substr(6)
                                    : *base;
        auto bindIt = refParamClassBindings_.find(paramName);
        if (info.isRefParam && bindIt != refParamClassBindings_.end()) {
          argBindings = &bindIt->second;
        }
      }
      auto addCarriedSource = [&](const std::string& name) {
        // The referent may already carry a frame environment: that carried
        // bound is a source the other destinations must accommodate
        if (!base) return;
        auto bit = frameBoundVars_.find(resolveRefTarget(*base).actualTarget);
        if (bit != frameBoundVars_.end()) {
          sources[name].push_back(
              {{LifetimeValue::Kind::Concrete, bit->second, "", *base},
               args[i]->getLocation(),
               i});
        }
      };
      if (!rt->getLifetimeName().empty()) {
        const std::string& name = rt->getLifetimeName();
        dests[name].push_back(
            {inferDestLifetimeValue(*args[i]), args[i]->getLocation(), i});
        addCarriedSource(name);
        if (name == "this") usesThis = true;
      }
      const auto& slotNames = rt->getClassLifetimeArgs();
      for (size_t k = 0; k < slotNames.size(); ++k) {
        const std::string& name = slotNames[k];
        LifetimeValue dest = inferDestLifetimeValue(*args[i]);
        if (argBindings && k < argBindings->size() && base) {
          dest = {LifetimeValue::Kind::Symbolic, 0, (*argBindings)[k], *base};
        }
        dests[name].push_back({dest, args[i]->getLocation(), i});
        addCarriedSource(name);
        if (name == "this") usesThis = true;
      }
    }
  }
  if (sources.empty() && dests.empty()) return;

  // The receiver joins the builtin 'this, and every name its CLASS
  // declares: a 'class Bus<'a>' object is a carrier for values bound to
  // 'a (its fields may store them), so an 'a-named argument must outlive
  // the receiver too.
  const ExprAST* callee = call.getCallee();
  const ExprAST* receiver = nullptr;
  if (callee && callee->getType() == ASTNodeType::MEMBER_ACCESS) {
    receiver = static_cast<const MemberAccessAST&>(*callee).getObject();
  }
  if (receiver) {
    std::vector<std::string> receiverNames;
    if (usesThis) receiverNames.push_back("this");
    TypePtr receiverType = receiver->getResolvedType();
    if (auto* rrt = tryGetType<ReferenceType>(receiverType)) {
      receiverType = rrt->getReferencedType();
    }
    if (auto* rct = tryGetType<ClassType>(receiverType)) {
      for (const auto& className : rct->getLifetimeParams()) {
        if (sources.count(className) || dests.count(className)) {
          receiverNames.push_back(className);
        }
      }
    }
    if (auto* rit = tryGetType<InterfaceType>(receiverType)) {
      for (const auto& ifaceName : rit->getLifetimeParams()) {
        if (sources.count(ifaceName) || dests.count(ifaceName)) {
          receiverNames.push_back(ifaceName);
        }
      }
    }
    const std::string* receiverBase = getBaseVariableName(*receiver);
    // A receiver that is our own ref parameter with class-slot bindings
    // ('bus: ref Bus<'this>') stands for those bindings: storing under the
    // class's 'a means storing under what WE bound it to
    const std::vector<std::string>* receiverBindings = nullptr;
    std::vector<std::string> receiverClassNames;
    if (receiverBase) {
      auto info = resolveRefTarget(*receiverBase);
      std::string paramName = info.actualTarget.rfind("param:", 0) == 0
                                  ? info.actualTarget.substr(6)
                                  : *receiverBase;
      auto bindIt = refParamClassBindings_.find(paramName);
      if (info.isRefParam && bindIt != refParamClassBindings_.end()) {
        receiverBindings = &bindIt->second;
        TypePtr receiverType = receiver->getResolvedType();
        if (auto* rrt = tryGetType<ReferenceType>(receiverType)) {
          receiverType = rrt->getReferencedType();
        }
        if (auto* rct = tryGetType<ClassType>(receiverType)) {
          receiverClassNames = rct->getLifetimeParams();
        }
      }
    }
    for (const auto& name : receiverNames) {
      LifetimeValue dest = inferDestLifetimeValue(*receiver);
      if (receiverBindings && name != "this") {
        for (size_t k = 0; k < receiverClassNames.size() &&
                           k < receiverBindings->size();
             ++k) {
          if (receiverClassNames[k] == name) {
            dest = {LifetimeValue::Kind::Symbolic, 0, (*receiverBindings)[k],
                    *receiverBase};
          }
        }
      }
      dests[name].push_back(
          {dest, call.getLocation(), static_cast<size_t>(-1)});
      if (receiverBase) {
        auto bit =
            frameBoundVars_.find(resolveRefTarget(*receiverBase).actualTarget);
        if (bit != frameBoundVars_.end()) {
          sources[name].push_back(
              {{LifetimeValue::Kind::Concrete, bit->second, "", *receiverBase},
               call.getLocation(),
               static_cast<size_t>(-1)});
        }
      }
    }
  }

  // Wherever 'this is a destination, the receiver itself is a potential
  // source: the callee can form a bound method of `this` (environment =
  // the receiver) and store it there
  if (receiver && dests.count("this")) {
    sources["this"].push_back({inferDestLifetimeValue(*receiver),
                               call.getLocation(), static_cast<size_t>(-1)});
  }

  for (const auto& [name, srcEntries] : sources) {
    auto dit = dests.find(name);
    if (dit == dests.end()) continue;
    for (const auto& src : srcEntries) {
      for (const auto& dst : dit->second) {
        if (src.argIndex == dst.argIndex) continue;
        if (lifetimeValueOutlives(src.value, dst.value)) continue;
        std::string srcName = src.value.described.empty()
                                  ? "this argument"
                                  : "'" + src.value.described + "'";
        std::string dstName = dst.value.described.empty()
                                  ? "the destination"
                                  : "'" + dst.value.described + "'";
        reportError(
            "cannot make this call: the signature ties " + srcName + " and " +
                dstName + " together through lifetime '" + name + ", but " +
                srcName + "'s captured environment does not provably "
                "outlive " + dstName + ". Declare them in the same scope, "
                "or give the lambda its data as arguments instead of "
                "captures",
            src.pos);
      }
    }
  }

  // A frame-local destination that received a frame-pinned environment is
  // frame-bound from here on, at the environment's depth. A destination's
  // own contribution (the same argument as source and destination - the
  // receiver's implicit 'this source, a carried bound) marks nothing: a
  // value cannot pin itself.
  for (const auto& [name, dstEntries] : dests) {
    auto sit = sources.find(name);
    if (sit == sources.end()) continue;
    for (const auto& dst : dstEntries) {
      if (dst.value.kind != LifetimeValue::Kind::Concrete) continue;
      if (dst.value.described.empty()) continue;
      size_t maxSourceDepth = 0;
      bool haveConcreteSource = false;
      for (const auto& src : sit->second) {
        if (src.argIndex == dst.argIndex) continue;
        if (src.value.kind == LifetimeValue::Kind::Concrete) {
          haveConcreteSource = true;
          maxSourceDepth = std::max(maxSourceDepth, src.value.depth);
        }
      }
      if (!haveConcreteSource) continue;
      const std::string target =
          resolveRefTarget(dst.value.described).actualTarget;
      // Temporaries and raw pointers resolve to nothing frame-local and
      // fall out here
      if (!frameLocalNames_.count(target)) continue;
      auto [it, inserted] = frameBoundVars_.try_emplace(target, maxSourceDepth);
      if (!inserted) it->second = std::max(it->second, maxSourceDepth);
    }
  }
}

// A ref-storing class value is landing in the named destination: a fresh
// construction records the deepest declaration among the variables it
// borrows, and a holder local moving between names carries its bound along.
// A destination declared in an outer scope than the bound would keep the
// borrowed storage's address past its death, so it is rejected. destDepth
// of 0 means the destination outlives the frame.
void BorrowChecker::trackRefHolderStore(const std::string& destName,
                                        size_t destDepth, const ExprAST& value,
                                        const Position& pos) {
  const ExprAST* e = &value;
  while (e->getType() == ASTNodeType::PAREN_EXPR) {
    e = static_cast<const ParenExprAST&>(*e).getInner();
  }

  // A holder local moved into another name: the bound travels with it
  if (e->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    TypePtr valueType = e->getResolvedType();
    if (!valueType || !valueType->isCompound() || valueType->isReference()) {
      return;  // a borrow or a scalar read relocates nothing
    }
    auto it = refHolderBounds_.find(
        static_cast<const VariableReferenceAST&>(*e).getName());
    if (it == refHolderBounds_.end()) return;
    size_t bound = it->second;
    if (destDepth < bound) {
      reportError(
          "cannot store this value in '" + destName + "' - it borrows a "
          "variable declared in an inner scope, which dies before '" +
              destName + "' does. Declare '" + destName +
              "' alongside what the value borrows",
          pos);
      return;
    }
    auto [dit, inserted] = refHolderBounds_.try_emplace(destName, bound);
    if (!inserted) dit->second = std::max(dit->second, bound);
    return;
  }

  // A fresh construction of a class that stores references
  if (e->getType() != ASTNodeType::CALL) return;
  const auto& call = static_cast<const CallExprAST&>(*e);
  TypePtr calleeType =
      call.getCallee() ? call.getCallee()->getResolvedType() : nullptr;
  if (!calleeType || !calleeType->isClass() || !classStoresRefs(calleeType)) {
    return;
  }
  const auto& args = call.getArgs();
  std::vector<TypePtr> argTypes;
  for (const auto& arg : args) {
    argTypes.push_back(arg ? arg->getResolvedType() : nullptr);
  }
  const ClassMethod* init = static_cast<const ClassType&>(*calleeType)
                                .getMethodForArgs("init", argTypes);
  if (!init) return;
  const auto& paramTypes = init->paramTypes;

  size_t bound = functionScopeDepth_;
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    if (!args[i] || !paramTypes[i] || !paramTypes[i]->isReference()) continue;
    const std::string* base = getBaseVariableName(*args[i]);
    if (!base || rawPointerLocals_.count(*base)) continue;
    size_t targetDepth = lookupDeclDepth(*base);
    if (targetDepth > destDepth) {
      reportError(
          "cannot store this value in '" + destName + "' - it borrows '" +
              *base + "', which is declared in an inner scope and dies "
              "before '" + destName + "' does. Declare '" + destName +
              "' alongside '" + *base + "'",
          pos);
      continue;
    }
    bound = std::max(bound, targetDepth);
  }
  auto [dit, inserted] = refHolderBounds_.try_emplace(destName, bound);
  if (!inserted) dit->second = std::max(dit->second, bound);
}

void BorrowChecker::checkLambdaDef(const LambdaAST& lambda) {
  const auto& proto = lambda.getProto();

  // Check if lambda returns a reference type
  bool returnsRef = false;
  if (proto.hasReturnType()) {
    const auto& retType = *proto.getReturnType();
    returnsRef = retType.isReference();

    // Rule: Return type cannot be a reference (when config enabled)
    if constexpr (Config::FORBID_REF_RETURNS) {
      if (returnsRef) {
        reportError("lambda cannot return a reference type",
                    lambda.getLocation());
      }
    }
  }

  // Register a borrow for every [ref x] capture in the ENCLOSING scope,
  // before entering the lambda's own scope. Scope-depth expiry gives the loan
  // exactly the enclosing block's lifetime, so conflicting refs to captured
  // variables are rejected while the lambda value is live. A `[const ref x]`
  // capture reads and never writes, so it takes a shared loan: several
  // lambdas — and so several threads — may hold one at the same time.
  const auto& pos = lambda.getLocation();

  // An owned capture takes no loan — it takes the value. A compound moves
  // into the closure's environment, so the name it came from is gone from
  // here on and the enclosing scope no longer drops it.
  for (const auto& cap : proto.getCaptures()) {
    if (cap.kind != CaptureKind::Owned || !cap.type || !cap.type->isCompound())
      continue;
    if (!state_.getActiveLoans(cap.name).empty()) {
      reportError(
          "cannot move '" + cap.name + "' into the lambda while it is borrowed", pos);
      continue;
    }
    if (!checkMoveAllowed(cap.name, pos)) continue;
    if (movedVariables_.count(cap.name)) {
      reportError("use of moved variable '" + cap.name +
                      "'. Ownership was transferred in a previous assignment.", pos);
      continue;
    }
    recordMove(cap.name, pos);
  }

  for (const auto& cap : proto.getCaptures()) {
    if (cap.kind != CaptureKind::Borrow) continue;
    // A by-ref capture of an enclosing lambda's by-ref capture aliases the
    // existing loan rather than creating a new one
    bool aliasesEnclosingCapture = false;
    for (const auto* enclosing : lambdaProtoStack_) {
      for (const auto& enclosingCap : enclosing->getCaptures()) {
        if (enclosingCap.name == cap.name &&
            enclosingCap.kind == CaptureKind::Borrow) {
          aliasesEnclosingCapture = true;
          break;
        }
      }
      if (aliasesEnclosingCapture) break;
    }
    if (aliasesEnclosingCapture) continue;

    auto targetInfo = resolveRefTarget(cap.name);
    std::string refName = "$capture:" + cap.name + "@" +
                          std::to_string(pos.line) + ":" +
                          std::to_string(pos.column);
    auto result =
        state_.addBorrow(targetInfo.actualTarget, refName,
                         cap.isConst ? BorrowKind::Shared : BorrowKind::Mutable,
                         currentScope_, pos);
    if (!result.allowed) {
      reportConflict(result.errorMessage, pos,
                     result.conflictingLoan);
    }
  }

  // Save the enclosing function's checking state: exitFunctionScope()'s
  // blanket clear would otherwise wipe it for every lambda literal analyzed
  // mid-function
  auto savedRefVariables = refVariables_;
  auto savedMovedVariables = movedVariables_;
  auto savedFrameBoundVars = frameBoundVars_;
  frameBoundVars_.clear();  // the lambda body is another frame
  auto savedFrameSourcedLambdas = frameSourcedLambdas_;
  frameSourcedLambdas_.clear();
  auto savedFrameLocalNames = frameLocalNames_;
  frameLocalNames_.clear();
  auto savedDeclDepths = declDepths_;
  declDepths_.clear();
  auto savedRefHolderBounds = refHolderBounds_;
  refHolderBounds_.clear();
  auto savedRefTypedParams = refTypedParams_;
  auto savedParamEnvNames = paramEnvNames_;
  auto savedRefParamNames = refParamNames_;
  auto savedRefParamClassBindings = refParamClassBindings_;
  auto savedReturnLifetimeName = returnLifetimeName_;
  auto savedParamLifetimes = paramLifetimes_;
  auto savedFunctionScopeDepth = functionScopeDepth_;
  auto savedReturnsRef = currentFunctionReturnsRef_;
  auto savedFunction = currentFunction_;

  // Enter function scope (anonymous)
  enterFunctionScope("<lambda>");
  currentFunctionReturnsRef_ = returnsRef;

  // The name is gone from the enclosing scope, but inside the body it is the
  // closure's own value again — that is the point of moving it in.
  for (const auto& cap : proto.getCaptures()) {
    if (cap.kind == CaptureKind::Owned) movedVariables_.erase(cap.name);
  }

  // Inside the body a by-ref captured name IS the borrow: writing to it
  // writes through the capture's loan, exactly like a ref parameter.
  for (const auto& cap : proto.getCaptures()) {
    if (cap.kind == CaptureKind::Borrow) {
      refTypedParams_[cap.name] = !cap.isConst;
    }
  }

  // Track reference parameters and their lifetimes. A by-value parameter
  // is storage the lambda's activation owns; captures are not listed here -
  // the closure environment outlives any one activation.
  for (const auto& [argName, argType] : proto.getArgs()) {
    if (argType.isReference()) {
      refTypedParams_[argName] = !argType.constRef;
      // Assign param lifetime - outlives the lambda body
      Lifetime paramLt = Lifetime::param(argName);
      paramLifetimes_[argName] = paramLt;
      state_.setLifetime(argName, paramLt);
    } else {
      frameLocalNames_.insert(argName);
      declDepths_[argName] = currentScope_;
    }
  }

  // Check the lambda body (lambdas always have a body)
  lambdaProtoStack_.push_back(&proto);
  if (lambda.hasBody()) {
    checkBlockExpr(lambda.getBody());
  }
  lambdaProtoStack_.pop_back();

  // Exit function scope - clears the lambda's borrows
  exitFunctionScope();

  // Restore the enclosing function's state
  refVariables_ = std::move(savedRefVariables);
  movedVariables_ = std::move(savedMovedVariables);
  frameBoundVars_ = std::move(savedFrameBoundVars);
  frameSourcedLambdas_ = std::move(savedFrameSourcedLambdas);
  frameLocalNames_ = std::move(savedFrameLocalNames);
  declDepths_ = std::move(savedDeclDepths);
  refHolderBounds_ = std::move(savedRefHolderBounds);
  refTypedParams_ = std::move(savedRefTypedParams);
  paramEnvNames_ = std::move(savedParamEnvNames);
  refParamNames_ = std::move(savedRefParamNames);
  refParamClassBindings_ = std::move(savedRefParamClassBindings);
  returnLifetimeName_ = std::move(savedReturnLifetimeName);
  paramLifetimes_ = std::move(savedParamLifetimes);
  functionScopeDepth_ = savedFunctionScopeDepth;
  currentFunctionReturnsRef_ = savedReturnsRef;
  currentFunction_ = savedFunction;
}

void BorrowChecker::checkClassDef(const ClassDefinitionAST& classDef) {
  // The class's declared lifetimes govern what its methods may store into
  // 'this' fields (see checkMemberAssignment)
  auto savedClassLifetimes = activeClassLifetimes_;
  activeClassLifetimes_.clear();
  for (const auto& lp : classDef.getLifetimeParameters()) {
    activeClassLifetimes_.push_back(lp.name);
  }

  // Check for reference-type fields
  bool hasRefFields = false;
  for (const auto& field : classDef.getFields()) {
    if (field.type.isReference()) {
      hasRefFields = true;

      // Rule: Classes cannot have reference-type fields (when config enabled)
      if constexpr (Config::FORBID_REF_FIELDS_IN_CLASSES) {
        reportError("class '" + classDef.getName() +
                        "' cannot have reference field '" + field.name +
                        "' - references cannot be stored in classes; store "
                        "the value, or pass the reference to the methods "
                        "that need it",
                    field.location);
      }
    }
  }

  // Track classes with ref fields for lifetime checking
  if (hasRefFields) {
    classesWithRefFields_.insert(classDef.getName());
  }

  // Check methods
  for (const auto& method : classDef.getMethods()) {
    if (method.function) {
      checkFunctionDef(*method.function);
    }
  }

  // Check specializations (for generic classes)
  for (const auto& [name, specializedClass] : classDef.getSpecializations()) {
    checkClassDef(*specializedClass);
  }

  activeClassLifetimes_ = std::move(savedClassLifetimes);
}

void BorrowChecker::checkMemberAccess(const MemberAccessAST& access) {
  std::string path = fieldPath(access);

  if (access.getObject()) {
    // Reaching through an object to one of its fields is a use of that field,
    // not of the object as a whole
    if (!path.empty()) fieldBaseDepth_++;
    checkExpr(*access.getObject());
    if (!path.empty()) fieldBaseDepth_--;
  }

  if (!path.empty() && movedVariables_.count(path)) {
    const auto& pos = access.getLocation();
    reportError("use of moved field '" + path +
                    "'. It was moved out of its object; assign a value back "
                    "into it before reading it again",
                pos);
  }
}

void BorrowChecker::checkMemberAssignment(const MemberAssignmentAST& assign) {
  if (assign.getObject()) {
    // Storing into a field is not a use of the object as a whole: a field of
    // a partially moved object can still be filled back in
    fieldBaseDepth_++;
    checkExpr(*assign.getObject());
    fieldBaseDepth_--;

    // The field owns a value again
    std::string base = fieldPath(*assign.getObject());
    if (!base.empty()) {
      std::string path = base + "." + assign.getMemberName();
      movedVariables_.erase(path);
      clearFieldPaths(path);
    }
  }
  if (assign.getValue()) {
    checkExpr(*assign.getValue());

    // A field lets the value outlive this frame with its object, so a
    // value-tracked frame-bound compound is rejected: a Thread handle from
    // spawn, or a local one was moved into. This is what keeps
    // `this.t = spawn(lambda [ref x] ...)` from outliving x. A '[ref]'
    // lambda itself is type-tracked instead: sema only lets it into a
    // '[ref]' field, which makes the whole class frame-carrying
    // (classStoresRefs), so the object cannot leave the frame either.
    if (isFrameBoundExpr(*assign.getValue())) {
      reportError(
          "cannot store this value in field '" + assign.getMemberName() +
              "' - it holds a lambda whose captured environment lives in "
              "this frame, and the object may outlive it. Give the lambda "
              "its data as arguments instead of captures",
          assign.getLocation());
    }

    // A frame-sourced '[ref]' lambda may enter a field only when the object
    // dies with this frame; the object then carries the frame binding. A
    // field of `this` or of an object behind a ref parameter outlives it.
    if (isFrameSourcedLambdaExpr(*assign.getValue()) && assign.getObject()) {
      const std::string* base = getBaseVariableName(*assign.getObject());
      if (base) {
        noteFrameSourcedLambdaStore(*base, inferEnvDepth(*assign.getValue()),
                                    assign.getLocation());
      }
    }

    // A value whose type NAMES a lifetime ('cb: <'a>(...)') may enter
    // a destination that outlives the frame only when the destination's
    // own lifetime is the same name - 'this-named values into the
    // receiver, 'a-named values into a 'ref 'a' parameter's referent.
    // Cross-name stores are how a callee would launder one caller
    // lifetime into another, so they are rejected here, in the callee.
    if (assign.getObject() &&
        !isFrameSourcedLambdaExpr(*assign.getValue())) {
      LifetimeValue env = inferEnvLifetimeValue(*assign.getValue());
      if (env.kind == LifetimeValue::Kind::Symbolic) {
        const std::string* base = getBaseVariableName(*assign.getObject());
        // A name the enclosing class declares may enter the receiver's own
        // fields: the class's contract is that such values outlive its
        // objects, and call sites enforce it via the receiver's class.
        bool classOwnedName =
            std::find(activeClassLifetimes_.begin(),
                      activeClassLifetimes_.end(),
                      env.name) != activeClassLifetimes_.end();
        bool storesIntoThis =
            base && (*base == "this" ||
                     resolveRefTarget(*base).actualTarget == "this");
        if (base && nameOutlivesFrame(*base) &&
            !(classOwnedName && storesIntoThis)) {
          LifetimeValue dst = inferDestLifetimeValue(*assign.getObject());
          if (!lifetimeValueOutlives(env, dst)) {
            reportError(
                "cannot store this value in a field of '" + *base +
                    "' - its type ties it to lifetime '" + env.name +
                    ", and nothing says '" + *base + "' dies first. Give "
                    "the destination the same lifetime, or use 'this",
                assign.getLocation());
          }
        }
      }
    }

    auto srcType = assign.getValue()->getResolvedType();

    // Mark ALL temporaries as moved when assigned to a field.
    // The field takes ownership, so the temporary's deinit must be skipped.
    if (assign.getValue()->isTemporary()) {
      assign.getValue()->setMoved(true);
    }
    // For variable references of compound types, mark as moved
    else if (srcType && srcType->isCompound() &&
             assign.getValue()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
      const auto& srcRef =
          static_cast<const VariableReferenceAST&>(*assign.getValue());
      if (checkMoveAllowed(srcRef.getName(), srcRef.getLocation()) &&
          checkFieldsIntact(srcRef.getName(), srcRef.getLocation())) {
        recordMove(srcRef.getName(), srcRef.getLocation());
        clearFieldPaths(srcRef.getName());
        assign.getValue()->setMoved(true);  // Mark for codegen to skip deinit
      }
    } else {
      noteFieldMove(*assign.getValue());
    }
  }
}

void BorrowChecker::checkIndexedAssignment(const IndexedAssignmentAST& assign) {
  if (assign.getTarget()) {
    checkExpr(*assign.getTarget());
  }
  if (assign.getValue()) {
    checkExpr(*assign.getValue());

    // An indexed slot is a container element: like a field, it can carry
    // the value past this frame's death, so a frame-bound value (a Thread
    // handle from spawn) is rejected the same way
    if (isFrameBoundExpr(*assign.getValue())) {
      reportError(
          "cannot store this value in an indexed slot - it holds a lambda "
          "whose captured environment lives in this frame, and the "
          "container may outlive it. Give the lambda its data as arguments "
          "instead of captures",
          assign.getLocation());
    }

    // A frame-sourced '[ref]' lambda entering an element: same rule as a
    // field store - the container must die with this frame
    if (isFrameSourcedLambdaExpr(*assign.getValue()) && assign.getTarget()) {
      const std::string* base = getBaseVariableName(*assign.getTarget());
      if (base) {
        noteFrameSourcedLambdaStore(*base, inferEnvDepth(*assign.getValue()),
                                    assign.getLocation());
      }
    }

    noteFieldMove(*assign.getValue());
  }
}

void BorrowChecker::checkTryCatch(const TryCatchExprAST& tryCatch) {
  enterScope();
  checkBlockExpr(tryCatch.getTryBlock());
  exitScope();

  // Catch clauses are alternatives to one another, each entered from the try
  // block's state. As with if/else, a clause that returns or throws cannot
  // poison the code that follows the try/catch — only clauses that fall
  // through contribute their moves.
  auto movedBefore = movedVariables_;
  auto movedAfter = movedVariables_;

  for (const auto& clause : tryCatch.getCatchClauses()) {
    movedVariables_ = movedBefore;
    enterScope();
    if (clause.body) {
      checkBlockExpr(*clause.body);
      if (!exprDiverges(*clause.body)) {
        movedAfter.insert(movedVariables_.begin(), movedVariables_.end());
      }
    }
    exitScope();
  }

  movedVariables_ = std::move(movedAfter);
}

void BorrowChecker::checkUnsafeBlock(const UnsafeBlockAST& unsafeBlock) {
  // Unsafe blocks are just scoped blocks - check the body normally
  enterScope();
  checkBlockExpr(unsafeBlock.getBody());
  exitScope();
}

void BorrowChecker::enterScope() { currentScope_++; }

void BorrowChecker::exitScope() {
  state_.exitScope(currentScope_);
  currentScope_--;

  // Clean up ref tracking for this scope
  // Note: This is simplified - a full implementation would track scope per ref
}

void BorrowChecker::enterFunctionScope(const std::string& funcName) {
  currentFunction_ = funcName;
  enterScope();
  functionScopeDepth_ = currentScope_;  // Record where function body starts
  refTypedParams_.clear();
  rawPointerLocals_.clear();
  paramLifetimes_.clear();
  paramEnvNames_.clear();
  refParamNames_.clear();
  refParamClassBindings_.clear();
  returnLifetimeName_.clear();
}

void BorrowChecker::exitFunctionScope() {
  exitScope();
  refVariables_.clear();
  movedVariables_.clear();
  frameBoundVars_.clear();
  frameSourcedLambdas_.clear();
  frameLocalNames_.clear();
  declDepths_.clear();
  refHolderBounds_.clear();
  refTypedParams_.clear();
  rawPointerLocals_.clear();
  paramLifetimes_.clear();
  paramEnvNames_.clear();
  refParamNames_.clear();
  refParamClassBindings_.clear();
  returnLifetimeName_.clear();
  currentFunction_.clear();
  functionScopeDepth_ = 0;
  currentFunctionReturnsRef_ = false;
}

void BorrowChecker::reportError(const std::string& msg, const Position& pos) {
  BorrowError err;
  err.message = msg;
  err.location = pos;
  errors_.push_back(std::move(err));
}

void BorrowChecker::reportConflict(const std::string& msg, const Position& pos,
                                   const Loan& conflict) {
  BorrowError err;
  err.message = msg;
  err.location = pos;
  if (!conflict.borrowedVar.empty()) {
    err.relatedLocations.push_back(conflict.location);
  }
  errors_.push_back(std::move(err));
}

bool BorrowChecker::isReferenceType(const TypePtr& type) const {
  return type && type->isReference();
}

bool BorrowChecker::typeContainsReference(const TypePtr& type) const {
  if (!type) return false;
  if (type->isReference()) return true;
  // Could recursively check struct/class fields here
  return false;
}

void BorrowChecker::trackRef(const std::string& refName,
                             const std::string& targetVar, BorrowKind kind) {
  refVariables_[refName] = {targetVar, kind};
}

bool BorrowChecker::isRefExpr(const ExprAST& expr) const {
  if (expr.getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef = static_cast<const VariableReferenceAST&>(expr);
    return refVariables_.count(varRef.getName()) > 0 ||
           refTypedParams_.count(varRef.getName()) > 0;
  }
  return false;
}

const std::string* BorrowChecker::getVariableName(const ExprAST& expr) const {
  if (expr.getType() == ASTNodeType::VARIABLE_REFERENCE) {
    return &static_cast<const VariableReferenceAST&>(expr).getName();
  }
  return nullptr;
}

// Walk member/index chains down to the base variable (e.g. obj.a.b -> obj,
// arr[i] -> arr, this.x -> this). Returns nullptr when the base is not a
// named variable (a temporary).
const std::string* BorrowChecker::getBaseVariableName(
    const ExprAST& expr) const {
  static const std::string kThis = "this";
  const ExprAST* e = &expr;
  while (true) {
    switch (e->getType()) {
      case ASTNodeType::VARIABLE_REFERENCE:
        return &static_cast<const VariableReferenceAST&>(*e).getName();
      case ASTNodeType::MEMBER_ACCESS:
        e = static_cast<const MemberAccessAST&>(*e).getObject();
        continue;
      case ASTNodeType::INDEX:
        e = static_cast<const IndexAST&>(*e).getTarget();
        continue;
      case ASTNodeType::THIS:
        return &kThis;
      default:
        return nullptr;
    }
  }
}

BorrowChecker::RefTargetInfo BorrowChecker::resolveRefTarget(
    const std::string& targetVarName) const {
  RefTargetInfo info;

  // Check if target is itself a local reference - if so, we're rebinding
  auto refIt = refVariables_.find(targetVarName);
  if (refIt != refVariables_.end()) {
    // Rebinding through another reference - borrow the original variable
    info.actualTarget = refIt->second.first;
    info.isRebind = true;
    info.sourceBorrowKind = refIt->second.second;
    return info;
  }

  // Check if target is a ref-typed function parameter
  auto paramIt = refTypedParams_.find(targetVarName);
  if (paramIt != refTypedParams_.end()) {
    // Treat the param as a virtual borrow target since we don't know
    // what it references outside our scope
    info.actualTarget = "param:" + targetVarName;
    info.isRefParam = true;
    info.sourceBorrowKind =
        paramIt->second ? BorrowKind::Mutable : BorrowKind::Shared;
    info.isRebind = true;  // Treat as rebind for rule checking
    return info;
  }

  // Direct variable reference
  info.actualTarget = targetVarName;
  return info;
}

// ============================================================================
// Lifetime Inference
// ============================================================================

Lifetime BorrowChecker::inferExprLifetime(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::VARIABLE_REFERENCE: {
      const auto& varRef = static_cast<const VariableReferenceAST&>(expr);
      const std::string& name = varRef.getName();

      // Check if it's a reference variable - return the lifetime of its target
      auto refIt = refVariables_.find(name);
      if (refIt != refVariables_.end()) {
        // Get the lifetime of what it points to
        const std::string& target = refIt->second.first;
        auto targetLt = state_.getLifetime(target);
        if (targetLt) {
          return *targetLt;
        }
        // Fall back to inferring from the target name
        if (target.substr(0, 6) == "param:") {
          return Lifetime::param(target.substr(6));
        }
        auto declIt = declDepths_.find(target);
        return Lifetime::local(
            target, declIt != declDepths_.end() ? declIt->second
                                                : currentScope_);
      }

      // Check if it's a ref-typed function parameter
      auto paramIt = paramLifetimes_.find(name);
      if (paramIt != paramLifetimes_.end()) {
        return paramIt->second;
      }

      // Check stored lifetime
      auto storedLt = state_.getLifetime(name);
      if (storedLt) {
        return *storedLt;
      }

      // Local variable - bound to the scope it was declared in
      auto declIt = declDepths_.find(name);
      return Lifetime::local(name, declIt != declDepths_.end()
                                       ? declIt->second
                                       : currentScope_);
    }

    case ASTNodeType::MEMBER_ACCESS: {
      // Field access inherits the object's lifetime
      const auto& access = static_cast<const MemberAccessAST&>(expr);
      if (access.getObject()) {
        return inferExprLifetime(*access.getObject());
      }
      break;
    }

    case ASTNodeType::INDEX: {
      // Array index inherits the array's lifetime
      const auto& idx = static_cast<const IndexAST&>(expr);
      if (idx.getTarget()) {
        return inferExprLifetime(*idx.getTarget());
      }
      break;
    }

    case ASTNodeType::THIS: {
      // 'this' is implicitly a parameter - has param lifetime
      return Lifetime::param("this");
    }

    case ASTNodeType::CALL: {
      // Call expression: use specialized logic that considers
      // whether temporaries were passed to ref parameters
      const auto& call = static_cast<const CallExprAST&>(expr);
      return inferCallReturnLifetime(call);
    }

    case ASTNodeType::GENERIC_CALL: {
      // For _to_ref<T>(ptr), the returned ref has the lifetime of the ptr
      // arg when that is a named variable. Any other raw pointer expression
      // (pointer arithmetic, a method returning raw_ptr) points at storage
      // the borrow checker cannot see: raw pointers are the unsafe escape
      // hatch, so the resulting ref is unrestricted rather than a temporary.
      const auto& genericCall = static_cast<const GenericCallAST&>(expr);
      const std::string& funcName = genericCall.getFunctionName();
      if (funcName == "_to_ref" && !genericCall.getArgs().empty()) {
        const ExprAST& ptrArg = *genericCall.getArgs()[0];
        if (ptrArg.getType() == ASTNodeType::VARIABLE_REFERENCE) {
          return inferExprLifetime(ptrArg);
        }
        return Lifetime::static_();
      }
      // Other generic calls: treat as temporary
      break;
    }

    case ASTNodeType::UNSAFE_BLOCK: {
      // Unsafe block: return the lifetime of the inner expression
      const auto& unsafeBlock = static_cast<const UnsafeBlockAST&>(expr);
      const auto& body = unsafeBlock.getBody().getBody();
      if (!body.empty()) {
        return inferExprLifetime(*body.back());
      }
      break;
    }

    default:
      break;
  }

  // Default: temporary at current scope
  return Lifetime::local("$temp", currentScope_);
}

void BorrowChecker::checkReturnLifetime(const ReturnExprAST& ret) {
  const ExprAST* value = ret.getValue();
  if (!value) return;

  // Only check lifetime if the function is declared to return a reference type.
  // If the function returns a value type (e.g., i32), returning through a ref
  // variable is fine because the value gets copied/dereferenced.
  if (!currentFunctionReturnsRef_) return;

  // Infer the lifetime of the returned expression
  Lifetime exprLifetime = inferExprLifetime(*value);

  // A reference return is safe if:
  // 1. It's a static lifetime (string literals, globals)
  // 2. It's a parameter lifetime (caller owns the data)
  // 3. It's derived from a parameter (member access on param, etc.)
  //
  // It's NOT safe if:
  // - It's a local lifetime with scope >= functionScopeDepth_
  //   (local to this function - would dangle after return)

  if (exprLifetime.isLocal()) {
    // Check if the local is within this function (would dangle)
    if (exprLifetime.getScopeDepth() >= functionScopeDepth_) {
      const auto& pos = ret.getLocation();
      reportDanglingRef(exprLifetime.getName(), pos);
    }
  }
}

void BorrowChecker::reportDanglingRef(const std::string& varName,
                                      const Position& pos) {
  std::string msg;
  if (varName == "$temp") {
    msg =
        "cannot return reference to temporary value - it would be a "
        "dangling reference";
  } else {
    msg = "cannot return reference to local variable '" + varName +
          "' - it would be a dangling reference after the function returns";
  }
  reportError(msg, pos);
}

// ============================================================================
// Temporary Ownership Tracking
// ============================================================================

Lifetime BorrowChecker::inferCallReturnLifetime(const CallExprAST& call) {
  const ExprAST* callee = call.getCallee();
  if (!callee) {
    return Lifetime::local("$temp", currentScope_);
  }

  // A method call cannot outlive its receiver, so `obj.method(...)` gets the
  // receiver's lifetime whenever the signature is not visible here — inside a
  // generic body the callee's type is only known per specialization. A call
  // through a raw pointer is unrestricted for the same reason `_to_ref` is:
  // raw pointers are the unsafe escape hatch and name storage the checker
  // cannot see.
  const MemberAccessAST* methodAccess =
      callee->getType() == ASTNodeType::MEMBER_ACCESS
          ? static_cast<const MemberAccessAST*>(callee)
          : nullptr;
  auto receiverLifetime = [&]() -> std::optional<Lifetime> {
    if (!methodAccess || !methodAccess->getObject()) return std::nullopt;
    const ExprAST& object = *methodAccess->getObject();
    TypePtr objectType = object.getResolvedType();
    if (objectType && objectType->isRawPointer()) return Lifetime::static_();
    if (object.getType() == ASTNodeType::VARIABLE_REFERENCE &&
        rawPointerLocals_.count(
            static_cast<const VariableReferenceAST&>(object).getName())) {
      return Lifetime::static_();
    }
    return inferExprLifetime(object);
  };

  TypePtr calleeType = callee->getResolvedType();
  if (!calleeType) {
    if (auto lt = receiverLifetime()) return *lt;
    return Lifetime::local("$temp", currentScope_);
  }

  // Method call (`obj.method(...)`): the callee is a lambda-typed bound
  // method. A ref-returning method borrows from its receiver, so the result
  // has the receiver's lifetime (unless a temporary was passed by ref).
  if (auto* lambdaType = dynamic_cast<const LambdaType*>(calleeType.get())) {
    TypePtr returnType = lambdaType->getReturnType();
    if (!returnType || !returnType->isReference()) {
      return Lifetime::local("$temp", currentScope_);
    }
    const auto& paramTypes = lambdaType->getParamTypes();
    const auto& args = call.getArgs();
    for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
      if (args[i] && paramTypes[i] && paramTypes[i]->isReference() &&
          args[i]->isTemporary()) {
        return Lifetime::local("$temp.from_call", currentScope_);
      }
    }
    if (auto lt = receiverLifetime()) return *lt;
    return Lifetime::param("$call_return");
  }

  // Check if function returns a reference type
  auto* funcType = dynamic_cast<const FunctionType*>(calleeType.get());
  if (!funcType) {
    if (auto lt = receiverLifetime()) return *lt;
    return Lifetime::local("$temp", currentScope_);
  }

  TypePtr returnType = funcType->getReturnType();
  if (!returnType || !returnType->isReference()) {
    // Non-ref return: always local (value is copied)
    return Lifetime::local("$temp", currentScope_);
  }

  // Function returns a reference. The lifetime depends on what was passed
  // to ref parameters. If ANY ref param received a temporary, the return
  // has local lifetime (can't outlive the call expression).

  const auto& paramTypes = funcType->getParamTypes();
  const auto& args = call.getArgs();

  // Check each ref parameter to see if it received a class temporary
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    const auto& arg = args[i];
    const auto& paramType = paramTypes[i];

    if (!arg || !paramType->isReference()) continue;

    // If this ref param received a temporary, return has local lifetime
    // The temporary dies at end of statement, so any ref to it would dangle
    if (arg->isTemporary()) {
      return Lifetime::local("$temp.from_call", currentScope_);
    }
  }

  // A ref-returning method borrows from its receiver: `obj.method(...)`
  // has obj's lifetime
  if (auto lt = receiverLifetime()) return *lt;

  // No temporaries passed to ref params - return lifetime is param lifetime
  // (tied to the arguments' lifetimes, which the caller controls)
  // For simplicity, we treat it as having param lifetime
  return Lifetime::param("$call_return");
}

}  // namespace sun
