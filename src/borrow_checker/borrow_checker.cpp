// src/borrow_checker/borrow_checker.cpp
// Implementation of the main borrow checker

#include "borrow_checker/borrow_checker.h"

#include <cassert>
#include <optional>

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

    case ASTNodeType::GENERIC_CALL:
      // Intrinsics and generic functions: arguments still use variables
      for (const auto& arg : static_cast<const GenericCallAST&>(expr).getArgs()) {
        if (arg) checkExpr(*arg);
      }
      break;

    // These don't need borrow checking
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
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

    case ASTNodeType::SPAWN: {
      const auto& spawnExpr = static_cast<const SpawnExprAST&>(expr);
      checkExpr(spawnExpr.getLambda());
      // A spawned thread may outlive the enclosing frame; pointers into it
      // (by-ref captures) would dangle and race
      if (isRefCapturingLambdaExpr(spawnExpr.getLambda())) {
        const auto& pos = spawnExpr.getLocation();
        reportError(
            "cannot spawn a lambda that captures variables by reference - "
            "the thread may outlive the captured variables",
            pos.line, pos.column);
      }
      break;
    }

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
                        "statement",
                    pos.line, pos.column);
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
                refPos.line, refPos.column);
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
    reportError("reference must be bound to a variable, not a temporary", 0, 0);
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
                      "' from immutable reference '" + *targetVarName + "'",
                  refPos.line, refPos.column);
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
  SourceLoc loc{refPos.line, refPos.column, ""};
  auto result = state_.addBorrow(targetInfo.actualTarget, refName, kind,
                                 currentScope_, loc);

  if (!result.allowed) {
    reportConflict(result.errorMessage, refPos.line, refPos.column,
                   result.conflictingLoan);
  } else {
    // Track this reference with its borrow kind
    refVariables_[refName] = {targetInfo.actualTarget, kind};
  }
}

// Shared write-side check for assigning to a named variable (plain or
// compound assignment)
void BorrowChecker::checkVariableWrite(const std::string& varName) {
  // Check if this is a ref or a regular variable
  auto refIt = refVariables_.find(varName);
  if (refIt != refVariables_.end()) {
    // Assigning through a reference
    auto result = state_.canMutateThroughRef(varName);
    if (!result.allowed) {
      reportConflict(result.errorMessage, 0, 0, result.conflictingLoan);
    }
  } else if (refTypedParams_.count(varName)) {
    // This is a reference parameter - assigning through it
    // For ref params, we track them differently since they don't have
    // a local borrow entry but do allow mutation
    // TODO: More sophisticated tracking for ref params
  } else {
    // Direct mutation of a variable
    // Only check if strict mutation checking is enabled
    if constexpr (Config::STRICT_MUTATION_CHECKING) {
      auto result = state_.canMutateDirectly(varName);
      if (!result.allowed) {
        reportConflict(result.errorMessage, 0, 0, result.conflictingLoan);
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

  checkVariableWrite(varName);
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
            .getName());
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
                    "'. Ownership was transferred in a previous assignment.",
                pos.line, pos.column);
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
      reportError(result.errorMessage, 0, 0);
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
    const ClassMethod* init =
        static_cast<const ClassType&>(*calleeType)
            .getMethodForArgs("init", argTypes);
    if (!init) return;
    paramTypes = init->paramTypes;
  } else {
    // Handle FunctionType (direct calls)
    auto* funcType = dynamic_cast<const FunctionType*>(calleeType.get());
    if (!funcType) return;
    paramTypes = funcType->getParamTypes();
  }

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

bool BorrowChecker::exprDiverges(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::RETURN:
    case ASTNodeType::THROW:
      return true;
    case ASTNodeType::BLOCK: {
      const auto& block = static_cast<const BlockExprAST&>(expr);
      for (const auto& stmt : block.getBody()) {
        if (stmt && exprDiverges(*stmt)) return true;
      }
      return false;
    }
    case ASTNodeType::IF: {
      const auto& ifExpr = static_cast<const IfExprAST&>(expr);
      return ifExpr.getThen() && ifExpr.getElse() &&
             exprDiverges(*ifExpr.getThen()) && exprDiverges(*ifExpr.getElse());
    }
    default:
      return false;
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
                pos.line, pos.column);
    return false;
  }
  if (frozenDiscriminants_.count(name)) {
    reportError("cannot move '" + name +
                    "' while it is being matched — its payloads are borrowed "
                    "by the match arms",
                pos.line, pos.column);
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
  if (access.isBoundMethodRef() || access.hasResolvedQualifiedName()) return "";

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

void BorrowChecker::recordMove(const std::string& place, const Position& pos) {
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
  for (const auto& v : loopVars) loopLocals_.back().insert(v);

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
                    "value back into it before the iteration ends",
                pos.line, pos.column);
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
                  "it",
              pos.line, pos.column);
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

  // A lambda holding pointers into this frame must not escape through return
  if (isRefCapturingLambdaExpr(*value)) {
    const auto& pos = ret.getLocation();
    reportError(
        "cannot return a lambda that captures variables by reference - the "
        "captured variables die when this function returns",
        pos.line, pos.column);
  }

  // Check lifetime safety for reference returns
  checkReturnLifetime(ret);

  // Move semantics for return values: when returning a compound type (class)
  // by value, the ownership transfers to the caller. Mark the return value
  // as moved so its deinit is skipped in the callee. Returning a `ref` only
  // lends the value out, so nothing moves.
  auto retType = value->getResolvedType();
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

  // Generic templates are analyzed with unbound type parameters; codegen only
  // emits their specializations (analyzed clones with concrete types), so those
  // are what must be checked (move marks must land on the emitted AST).
  if (proto.isGeneric() && !func.getSpecializations().empty()) {
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
            "function '" + funcName + "' cannot return a reference type", 0, 0);
      }
    }
  }

  // Enter function scope
  enterFunctionScope(funcName);
  currentFunctionReturnsRef_ = returnsRef;

  // Track reference parameters and their lifetimes
  for (const auto& [argName, argType] : proto.getArgs()) {
    if (argType.isReference()) {
      refTypedParams_[argName] = !argType.constRef;
      // Assign param lifetime - outlives the function body
      Lifetime paramLt = Lifetime::param(argName);
      paramLifetimes_[argName] = paramLt;
      state_.setLifetime(argName, paramLt);
    }
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
        reportError("lambda cannot return a reference type", 0, 0);
      }
    }
  }

  // Register a mutable borrow for every [ref x] capture in the ENCLOSING
  // scope, before entering the lambda's own scope. Scope-depth expiry gives
  // the loan exactly the enclosing block's lifetime, so conflicting refs to
  // captured variables are rejected while the lambda value is live.
  const auto& pos = lambda.getLocation();
  for (const auto& cap : proto.getCaptures()) {
    if (!cap.byRef) continue;
    // A by-ref capture of an enclosing lambda's by-ref capture aliases the
    // existing loan rather than creating a new one
    bool aliasesEnclosingCapture = false;
    for (const auto* enclosing : lambdaProtoStack_) {
      for (const auto& enclosingCap : enclosing->getCaptures()) {
        if (enclosingCap.name == cap.name && enclosingCap.byRef) {
          aliasesEnclosingCapture = true;
          break;
        }
      }
      if (aliasesEnclosingCapture) break;
    }
    if (aliasesEnclosingCapture) continue;

    auto targetInfo = resolveRefTarget(cap.name);
    SourceLoc loc{pos.line, pos.column, ""};
    std::string refName = "$capture:" + cap.name + "@" +
                          std::to_string(pos.line) + ":" +
                          std::to_string(pos.column);
    auto result = state_.addBorrow(targetInfo.actualTarget, refName,
                                   BorrowKind::Mutable, currentScope_, loc);
    if (!result.allowed) {
      reportConflict(result.errorMessage, pos.line, pos.column,
                     result.conflictingLoan);
    }
  }

  // Save the enclosing function's checking state: exitFunctionScope()'s
  // blanket clear would otherwise wipe it for every lambda literal analyzed
  // mid-function
  auto savedRefVariables = refVariables_;
  auto savedMovedVariables = movedVariables_;
  auto savedRefTypedParams = refTypedParams_;
  auto savedParamLifetimes = paramLifetimes_;
  auto savedFunctionScopeDepth = functionScopeDepth_;
  auto savedReturnsRef = currentFunctionReturnsRef_;
  auto savedFunction = currentFunction_;

  // Enter function scope (anonymous)
  enterFunctionScope("<lambda>");
  currentFunctionReturnsRef_ = returnsRef;

  // Track reference parameters and their lifetimes
  for (const auto& [argName, argType] : proto.getArgs()) {
    if (argType.isReference()) {
      refTypedParams_[argName] = !argType.constRef;
      // Assign param lifetime - outlives the lambda body
      Lifetime paramLt = Lifetime::param(argName);
      paramLifetimes_[argName] = paramLt;
      state_.setLifetime(argName, paramLt);
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
  refTypedParams_ = std::move(savedRefTypedParams);
  paramLifetimes_ = std::move(savedParamLifetimes);
  functionScopeDepth_ = savedFunctionScopeDepth;
  currentFunctionReturnsRef_ = savedReturnsRef;
  currentFunction_ = savedFunction;
}

void BorrowChecker::checkClassDef(const ClassDefinitionAST& classDef) {
  // Check for reference-type fields
  bool hasRefFields = false;
  for (const auto& field : classDef.getFields()) {
    if (field.type.isReference()) {
      hasRefFields = true;

      // Rule: Classes cannot have reference-type fields (when config enabled)
      if constexpr (Config::FORBID_REF_FIELDS_IN_CLASSES) {
        reportError("class '" + classDef.getName() +
                        "' cannot have reference field '" + field.name +
                        "' - references cannot be stored in structs",
                    0, 0);
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
                pos.line, pos.column);
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
}

void BorrowChecker::exitFunctionScope() {
  exitScope();
  refVariables_.clear();
  movedVariables_.clear();
  refTypedParams_.clear();
  rawPointerLocals_.clear();
  paramLifetimes_.clear();
  currentFunction_.clear();
  functionScopeDepth_ = 0;
  currentFunctionReturnsRef_ = false;
}

void BorrowChecker::reportError(const std::string& msg, int line, int col) {
  BorrowError err;
  err.message = msg;
  err.location = {line, col, ""};
  errors_.push_back(std::move(err));
}

void BorrowChecker::reportConflict(const std::string& msg, int line, int col,
                                   const Loan& conflict) {
  BorrowError err;
  err.message = msg;
  err.location = {line, col, ""};
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
        return Lifetime::local(target, currentScope_);
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

      // Local variable - bound to current scope
      return Lifetime::local(name, currentScope_);
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
      reportDanglingRef(exprLifetime.getName(), pos.line, pos.column);
    }
  }
}

void BorrowChecker::reportDanglingRef(const std::string& varName, int line,
                                      int col) {
  std::string msg;
  if (varName == "$temp") {
    msg =
        "cannot return reference to temporary value - it would be a "
        "dangling reference";
  } else {
    msg = "cannot return reference to local variable '" + varName +
          "' - it would be a dangling reference after the function returns";
  }
  reportError(msg, line, col);
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
