// include/borrow_checker/borrow_checker.h
// Main borrow checker - validates reference safety at compile time

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "borrow_checker/borrow_state.h"
#include "borrow_checker/loan.h"
#include "types.h"

namespace sun {

/// A single borrow checking error with source location and optional related
/// locations
struct BorrowError {
  std::string message;
  SourceLoc location;
  std::vector<SourceLoc>
      relatedLocations;  // Where conflicting borrows occurred

  std::string format() const {
    std::string result = location.toString() + ": error: " + message;
    for (const auto& rel : relatedLocations) {
      result += "\n  " + rel.toString() + ": note: related borrow here";
    }
    return result;
  }
};

/// Main borrow checker class
/// Validates reference safety by tracking borrows across the AST
///
/// Configuration is taken from sun::Config compile-time constants:
/// - FORBID_REF_RETURNS: If true, functions cannot return references
/// - FORBID_REF_FIELDS_IN_CLASSES: If true, classes cannot have ref fields
/// - STRICT_MUTATION_CHECKING: If true, disallow mutating borrowed variables
class BorrowChecker {
 public:
  BorrowChecker();

  /// Main entry point - check an entire program (BlockExprAST)
  /// Returns a list of borrow errors (empty if valid)
  std::vector<BorrowError> check(const BlockExprAST& program);

 private:
  // AST traversal methods
  void checkExpr(const ExprAST& expr);

  // Specific node handlers
  void checkVariableCreation(const VariableCreationAST& var);
  void checkReferenceCreation(const ReferenceCreationAST& ref);
  void checkVariableAssignment(const VariableAssignmentAST& assign);
  void checkVariableReference(const VariableReferenceAST& varRef);
  void checkBinaryExpr(const BinaryExprAST& binary);
  void checkCallExpr(const CallExprAST& call);
  void checkIfExpr(const IfExprAST& ifExpr);
  void checkMatchExpr(const MatchExprAST& matchExpr);
  void checkWhileExpr(const WhileExprAST& whileExpr);
  void checkForExpr(const ForExprAST& forExpr);
  void checkForInExpr(const ForInExprAST& forInExpr);
  void checkBlockExpr(const BlockExprAST& block);
  void checkReturnStmt(const ReturnExprAST& ret);
  void checkFunctionDef(const FunctionAST& func);
  void checkLambdaDef(const LambdaAST& lambda);
  void checkClassDef(const ClassDefinitionAST& classDef);
  void checkMemberAccess(const MemberAccessAST& access);
  void checkMemberAssignment(const MemberAssignmentAST& assign);
  void checkIndexedAssignment(const IndexedAssignmentAST& assign);
  void checkTryCatch(const TryCatchExprAST& tryCatch);

  // Scope management
  void enterScope();
  void exitScope();
  void enterFunctionScope(const std::string& funcName);
  void exitFunctionScope();

  // Error reporting
  void reportError(const std::string& msg, int line, int col);
  void reportConflict(const std::string& msg, int line, int col,
                      const Loan& conflict);

  // Helper to check if a type is or contains a reference
  bool isReferenceType(const TypePtr& type) const;
  bool typeContainsReference(const TypePtr& type) const;

  // Track reference variables in current scope
  void trackRef(const std::string& refName, const std::string& targetVar,
                BorrowKind kind = BorrowKind::Mutable);

  // Check if an expression evaluates to a reference
  bool isRefExpr(const ExprAST& expr) const;

  // Get the name of the variable being referenced (if expression is a variable
  // ref)
  const std::string* getVariableName(const ExprAST& expr) const;

  // Result of resolving a reference target
  struct RefTargetInfo {
    std::string actualTarget;  // The ultimate variable being borrowed
    bool isRebind = false;     // True if rebinding through another ref
    BorrowKind sourceBorrowKind =
        BorrowKind::Mutable;  // Kind of source ref (if rebinding)
    bool isRefParam = false;  // True if target is a ref-typed parameter
  };

  // Resolve the actual target of a reference creation
  // Handles rebinding through refs, ref params, and direct variable refs
  RefTargetInfo resolveRefTarget(const std::string& targetVarName) const;

  BorrowState state_;
  std::vector<BorrowError> errors_;

  size_t currentScope_ = 0;
  std::string currentFunction_;

  // Track which variables are references in current scope
  // refName -> (targetVarName, borrowKind)
  std::unordered_map<std::string, std::pair<std::string, BorrowKind>>
      refVariables_;

  // Track which variables have been moved (for move semantics)
  // varName -> true if ownership was transferred
  std::unordered_set<std::string> movedVariables_;

  // Track which variables are known to be reference-typed (for function params)
  std::unordered_set<std::string> refTypedParams_;

  // Track function return types to validate no ref returns
  std::unordered_map<std::string, TypePtr> functionReturnTypes_;
};

}  // namespace sun
