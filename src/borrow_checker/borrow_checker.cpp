// src/borrow_checker/borrow_checker.cpp
// Implementation of the main borrow checker

#include "borrow_checker/borrow_checker.h"

#include <cassert>

#include "config.h"
#include "error.h"

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

    case ASTNodeType::TRY_CATCH:
      checkTryCatch(static_cast<const TryCatchExprAST&>(expr));
      break;

    // These don't need borrow checking
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
    case ASTNodeType::BOOL_LITERAL:
    case ASTNodeType::NULL_LITERAL:
    case ASTNodeType::ARRAY_LITERAL:
    case ASTNodeType::INDEX:
    case ASTNodeType::THIS:
    case ASTNodeType::PROTOTYPE:
    case ASTNodeType::IMPORT:
    case ASTNodeType::DECLARE_TYPE:
    case ASTNodeType::NAMESPACE:
    case ASTNodeType::USING:
    case ASTNodeType::QUALIFIED_NAME:
    case ASTNodeType::INTERFACE_DEFINITION:
    case ASTNodeType::ENUM_DEFINITION:
    case ASTNodeType::GENERIC_CALL:
    case ASTNodeType::THROW:
    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
    case ASTNodeType::UNARY:
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

  // Move semantics: if initializing from a class-typed variable, mark source as
  // moved. Classes are value types that get moved, not copied.
  if (var.getValue() &&
      var.getValue()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& srcRef =
        static_cast<const VariableReferenceAST&>(*var.getValue());
    // Get the resolved type from the source expression
    auto srcType = var.getValue()->getResolvedType();
    if (srcType && srcType->isClass()) {
      movedVariables_.insert(srcRef.getName());
    }
  }
}

void BorrowChecker::checkReferenceCreation(const ReferenceCreationAST& ref) {
  const std::string& refName = ref.getName();
  const ExprAST* target = ref.getTarget();

  // Target must be a variable (lvalue)
  const std::string* targetVarName = getVariableName(*target);
  if (!targetVarName) {
    reportError("reference must be bound to a variable, not a temporary", 0, 0);
    return;
  }

  // Resolve the actual target and check rebinding rules
  auto targetInfo = resolveRefTarget(*targetVarName);

  // Borrow kind comes from the AST node
  BorrowKind kind = ref.isMutable() ? BorrowKind::Mutable : BorrowKind::Shared;

  // Rebinding rules: mutable -> immutable is OK, immutable -> mutable is NOT
  if (targetInfo.isRebind) {
    if (targetInfo.sourceBorrowKind == BorrowKind::Shared &&
        kind == BorrowKind::Mutable) {
      const auto& pos = ref.getLocation();
      reportError("cannot create mutable reference '" + refName +
                      "' from immutable reference '" + *targetVarName + "'",
                  pos.line, pos.column);
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
  const auto& pos = ref.getLocation();
  SourceLoc loc{pos.line, pos.column, ""};
  auto result = state_.addBorrow(targetInfo.actualTarget, refName, kind,
                                 currentScope_, loc);

  if (!result.allowed) {
    reportConflict(result.errorMessage, pos.line, pos.column,
                   result.conflictingLoan);
  } else {
    // Track this reference with its borrow kind
    refVariables_[refName] = {targetInfo.actualTarget, kind};
  }
}

void BorrowChecker::checkVariableAssignment(
    const VariableAssignmentAST& assign) {
  const std::string& varName = assign.getName();

  // Check the value expression first
  if (assign.getValue()) {
    checkExpr(*assign.getValue());
  }

  // Move semantics: if assigning from a class-typed variable, mark source as
  // moved. Classes are value types that get moved, not copied.
  if (assign.getValue() &&
      assign.getValue()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& srcRef =
        static_cast<const VariableReferenceAST&>(*assign.getValue());
    auto srcType = assign.getValue()->getResolvedType();
    if (srcType && srcType->isClass()) {
      movedVariables_.insert(srcRef.getName());
    }
  }

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

void BorrowChecker::checkVariableReference(const VariableReferenceAST& varRef) {
  const std::string& name = varRef.getName();

  // Check for use-after-move
  if (movedVariables_.count(name)) {
    const auto& pos = varRef.getLocation();
    reportError("use of moved variable '" + name +
                    "'. Ownership was transferred in a previous assignment.",
                pos.line, pos.column);
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

  // Handle FunctionType (direct calls)
  auto* funcType = dynamic_cast<const FunctionType*>(calleeType.get());
  if (!funcType) return;

  const auto& paramTypes = funcType->getParamTypes();
  const auto& args = call.getArgs();

  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    const auto& arg = args[i];
    const auto& paramType = paramTypes[i];

    // Only check variable references (not complex expressions)
    if (!arg || arg->getType() != ASTNodeType::VARIABLE_REFERENCE) continue;

    const auto& varRef = static_cast<const VariableReferenceAST&>(*arg);
    TypePtr argType = arg->getResolvedType();

    // If argument is compound type AND parameter is NOT a reference (by-value)
    // then we move the argument
    if (argType && argType->isCompound() && !paramType->isReference()) {
      movedVariables_.insert(varRef.getName());
    }
  }
}

void BorrowChecker::checkIfExpr(const IfExprAST& ifExpr) {
  // Check condition
  if (ifExpr.getCond()) {
    checkExpr(*ifExpr.getCond());
  }

  // Check then branch in its own scope
  enterScope();
  if (ifExpr.getThen()) {
    checkExpr(*ifExpr.getThen());
  }
  exitScope();

  // Check else branch in its own scope
  if (ifExpr.getElse()) {
    enterScope();
    checkExpr(*ifExpr.getElse());
    exitScope();
  }
}

void BorrowChecker::checkMatchExpr(const MatchExprAST& matchExpr) {
  // Check discriminant expression
  if (matchExpr.getDiscriminant()) {
    checkExpr(*matchExpr.getDiscriminant());
  }

  // Check each arm in its own scope
  for (const auto& arm : matchExpr.getArms()) {
    enterScope();
    // Check pattern if present
    if (arm.pattern) {
      checkExpr(*arm.pattern);
    }
    // Check body
    if (arm.body) {
      checkExpr(*arm.body);
    }
    exitScope();
  }
}

void BorrowChecker::checkWhileExpr(const WhileExprAST& whileExpr) {
  // Check condition
  if (whileExpr.getCondition()) {
    checkExpr(*whileExpr.getCondition());
  }

  // Check body in its own scope
  enterScope();
  if (whileExpr.getBody()) {
    checkExpr(*whileExpr.getBody());
  }
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
  if (forExpr.getBody()) {
    checkExpr(*forExpr.getBody());
  }
  exitScope();
}

void BorrowChecker::checkForInExpr(const ForInExprAST& forInExpr) {
  // Check the iterable expression first (outside the loop scope)
  if (forInExpr.getIterable()) {
    checkExpr(*forInExpr.getIterable());
  }

  // Enter scope for loop variable
  enterScope();

  // Check body
  if (forInExpr.getBody()) {
    checkExpr(*forInExpr.getBody());
  }
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

  // Note: Returning a reference variable like `return r` actually returns the
  // dereferenced VALUE, not the reference itself. This is fine because the
  // value is copied. The only restriction is on functions declared with ref
  // return types, which is checked in checkFunctionDef.
}

void BorrowChecker::checkFunctionDef(const FunctionAST& func) {
  const auto& proto = func.getProto();
  const std::string& funcName = proto.getName();

  // Rule: Return type cannot be a reference
  if constexpr (Config::FORBID_REF_RETURNS) {
    if (proto.hasReturnType()) {
      const auto& retType = *proto.getReturnType();
      if (retType.isReference()) {
        reportError(
            "function '" + funcName + "' cannot return a reference type", 0, 0);
      }
    }
  }

  // Enter function scope
  enterFunctionScope(funcName);

  // Track reference parameters
  for (const auto& [argName, argType] : proto.getArgs()) {
    if (argType.isReference()) {
      refTypedParams_.insert(argName);
    }
  }

  // Check the function body (skip for extern declarations)
  if (func.hasBody()) {
    checkBlockExpr(func.getBody());
  }

  // Exit function scope - clears all borrows
  exitFunctionScope();
}

void BorrowChecker::checkLambdaDef(const LambdaAST& lambda) {
  const auto& proto = lambda.getProto();

  // Rule: Return type cannot be a reference
  if constexpr (Config::FORBID_REF_RETURNS) {
    if (proto.hasReturnType()) {
      const auto& retType = *proto.getReturnType();
      if (retType.isReference()) {
        reportError("lambda cannot return a reference type", 0, 0);
      }
    }
  }

  // Enter function scope (anonymous)
  enterFunctionScope("<lambda>");

  // Track reference parameters
  for (const auto& [argName, argType] : proto.getArgs()) {
    if (argType.isReference()) {
      refTypedParams_.insert(argName);
    }
  }

  // Check the lambda body (lambdas always have a body)
  if (lambda.hasBody()) {
    checkBlockExpr(lambda.getBody());
  }

  // Exit function scope - clears all borrows
  exitFunctionScope();
}

void BorrowChecker::checkClassDef(const ClassDefinitionAST& classDef) {
  // Rule: Classes cannot have reference-type fields (without lifetimes)
  if constexpr (Config::FORBID_REF_FIELDS_IN_CLASSES) {
    for (const auto& field : classDef.getFields()) {
      if (field.type.isReference()) {
        reportError("class '" + classDef.getName() +
                        "' cannot have reference field '" + field.name +
                        "' - references cannot be stored in structs",
                    0, 0);
      }
    }
  }

  // Check methods
  for (const auto& method : classDef.getMethods()) {
    if (method.function) {
      checkFunctionDef(*method.function);
    }
  }
}

void BorrowChecker::checkMemberAccess(const MemberAccessAST& access) {
  if (access.getObject()) {
    checkExpr(*access.getObject());
  }
}

void BorrowChecker::checkMemberAssignment(const MemberAssignmentAST& assign) {
  if (assign.getObject()) {
    checkExpr(*assign.getObject());
  }
  if (assign.getValue()) {
    checkExpr(*assign.getValue());
  }
}

void BorrowChecker::checkIndexedAssignment(const IndexedAssignmentAST& assign) {
  if (assign.getTarget()) {
    checkExpr(*assign.getTarget());
  }
  if (assign.getValue()) {
    checkExpr(*assign.getValue());
  }
}

void BorrowChecker::checkTryCatch(const TryCatchExprAST& tryCatch) {
  enterScope();
  checkBlockExpr(tryCatch.getTryBlock());
  exitScope();

  // Single catch clause
  const auto& clause = tryCatch.getCatchClause();
  enterScope();
  if (clause.body) {
    checkBlockExpr(*clause.body);
  }
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
  refTypedParams_.clear();
}

void BorrowChecker::exitFunctionScope() {
  exitScope();
  refVariables_.clear();
  movedVariables_.clear();
  refTypedParams_.clear();
  currentFunction_.clear();
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
  if (refTypedParams_.count(targetVarName)) {
    // Treat the param as a virtual borrow target since we don't know
    // what it references outside our scope
    info.actualTarget = "param:" + targetVarName;
    info.isRefParam = true;
    // Ref params are assumed mutable unless we add const ref support
    info.sourceBorrowKind = BorrowKind::Mutable;
    info.isRebind = true;  // Treat as rebind for rule checking
    return info;
  }

  // Direct variable reference
  info.actualTarget = targetVarName;
  return info;
}

}  // namespace sun
