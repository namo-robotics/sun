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
#include "semantic_analysis/types.h"

namespace sun {

/// A single borrow checking error with source location and optional related
/// locations. Rendering in the standard compiler format (source line and
/// caret) is done by the driver, which has the source text at hand.
struct BorrowError {
  std::string message;
  Position location;
  std::vector<Position>
      relatedLocations;  // Where conflicting borrows occurred
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
  // Shared by `ref r = lvalue` and `var r: ref T = lvalue`: both bind a
  // borrow of the lvalue's storage rather than moving a value into r.
  void checkBorrowBinding(const std::string& refName, const ExprAST& targetExpr,
                          bool isMutable, const Position& refPos);
  // Binding a ref-returning call's result to a name borrows every named
  // variable the call could hand back a reference into: the receiver and
  // each argument bound to a ref parameter.
  void borrowRefCallInputs(const std::string& refName, const CallExprAST& call,
                           bool isMutable, const Position& refPos);
  void checkBorrowTargetIntact(const ExprAST& target, const Position& refPos);
  void checkVariableAssignment(const VariableAssignmentAST& assign);
  void checkVariableReference(const VariableReferenceAST& varRef);
  void checkBinaryExpr(const BinaryExprAST& binary);
  void checkCallExpr(const CallExprAST& call);
  void checkIfExpr(const IfExprAST& ifExpr);
  void checkTernaryExpr(const TernaryExprAST& ternary);
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
  void checkCompoundAssignment(const CompoundAssignmentAST& assign);
  void checkVariableWrite(const std::string& varName, const TypePtr& valueType,
                          const Position& pos);
  bool isRefCapturingLambdaExpr(const ExprAST& expr) const;
  // Does this class type hold a reference in any field, transitively?
  bool classStoresRefs(const TypePtr& type) const;
  bool classStoresRefsWalk(const TypePtr& type,
                           std::unordered_set<const Type*>& visited) const;

  // Protos of the lambdas whose bodies are currently being checked
  // (innermost last) - nested by-ref captures alias their loans
  std::vector<const PrototypeAST*> lambdaProtoStack_;
  void checkTryCatch(const TryCatchExprAST& tryCatch);
  void checkUnsafeBlock(const UnsafeBlockAST& unsafeBlock);

  // Scope management
  void enterScope();
  void exitScope();
  void enterFunctionScope(const std::string& funcName);
  void exitFunctionScope();

  // Error reporting - positions carry the file path so the driver can
  // render the standard source-line-and-caret format
  void reportError(const std::string& msg, const Position& pos);
  void reportConflict(const std::string& msg, const Position& pos,
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
  const std::string* getBaseVariableName(const ExprAST& expr) const;

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

  // Compound match-payload bindings currently in scope. They BORROW the
  // matched value's payload slot in place: moving them (var creation,
  // by-value argument, return, assignment source) is rejected.
  std::unordered_set<std::string> matchBorrowedBindings_;

  // Discriminant variables of enclosing match expressions: frozen (no
  // assignment, no move) while their arms borrow payloads.
  std::unordered_set<std::string> frozenDiscriminants_;

  // Reject moving out of a match binding / frozen discriminant. Returns
  // true if `name` may be moved.
  bool checkMoveAllowed(const std::string& name, const Position& pos);

  // =========================================================================
  // Partial moves (a field moved out of its object)
  // =========================================================================

  // Moved field paths ("cfg.line", "this.keys") live in movedVariables_
  // alongside moved variable names, so branches, loops and function exits
  // handle them the same way. A path is forgotten again when a value is
  // assigned back into the field.

  // "cfg.line" for a read of a field of a named object; empty when the
  // expression is not one (a temporary, a call result, an enum constant, a
  // module-qualified name, a method).
  std::string fieldPath(const ExprAST& expr) const;

  // Record that `value` moved a compound field out of its object.
  void noteFieldMove(const ExprAST& value);

  // Forget every field path under `base` (it was reassigned or moved away).
  void clearFieldPaths(const std::string& base);

  // The field of `name` that is currently moved out, if any.
  const std::string* movedFieldOf(const std::string& name) const;

  // Reject moving a whole object whose field was moved out. Returns true if
  // `name` may be moved.
  bool checkFieldsIntact(const std::string& name, const Position& pos);

  // Depth of member-access objects being checked: reading `cfg.query` is a
  // use of that field, not of `cfg` as a whole, so a partially moved `cfg` is
  // still fine there. Assignment targets suppress it the same way.
  int fieldBaseDepth_ = 0;

  // =========================================================================
  // Loop-carried moves
  // =========================================================================

  // Record that `place` (a variable name or a field path) was moved here.
  void recordMove(const std::string& place, const Position& pos);

  // Where each still-standing move happened, for the loop report below.
  std::unordered_map<std::string, Position> moveLocations_;

  // Names declared inside the loop bodies currently being checked (innermost
  // last). Such a name is fresh on every iteration, so moving it is fine.
  std::vector<std::unordered_set<std::string>> loopLocals_;

  // Note a declaration in every enclosing loop body.
  void noteLoopLocal(const std::string& name);

  // Check a loop body: a move that is still standing when the body ends would
  // run again on the next iteration, using a value that is already gone.
  void checkLoopBody(const ExprAST* body,
                     const std::vector<std::string>& loopVars = {});

  // Reference-typed parameters of the current function: name -> true when
  // the parameter is `ref T` (mutable), false for `const ref T`
  std::unordered_map<std::string, bool> refTypedParams_;

  // Locals declared as raw_ptr<T>. What they point at is storage the checker
  // cannot see (raw pointers are the unsafe escape hatch), so a reference
  // reached through one is unrestricted rather than tied to the local.
  std::unordered_set<std::string> rawPointerLocals_;

  // Track function return types to validate no ref returns
  std::unordered_map<std::string, TypePtr> functionReturnTypes_;

  // =========================================================================
  // Lifetime Inference
  // =========================================================================

  /// Infer the lifetime of an expression.
  /// - Variable references return the variable's lifetime
  /// - Member access inherits the object's lifetime
  /// - Parameters have param lifetimes (outlive function body)
  /// - Locals have local lifetimes (bound to their scope)
  Lifetime inferExprLifetime(const ExprAST& expr);

  /// Check that a return statement's lifetime is valid.
  /// A ref return is only valid if the returned value's lifetime
  /// is tied to a parameter (not a local variable).
  void checkReturnLifetime(const ReturnExprAST& ret);

  /// Report a dangling reference error
  void reportDanglingRef(const std::string& varName, const Position& pos);

  // Scope depth when current function was entered (for lifetime comparison)
  size_t functionScopeDepth_ = 0;

  // True if current function has a reference return type
  bool currentFunctionReturnsRef_ = false;

  // Track lifetimes for parameters in current function
  // paramName -> Lifetime
  std::unordered_map<std::string, Lifetime> paramLifetimes_;

  // Counter for generating unique anonymous lifetime IDs
  uint32_t nextLifetimeId_ = 0;

  // Track classes that have reference fields (need special handling)
  std::unordered_set<std::string> classesWithRefFields_;

  // =========================================================================
  // Temporary Ownership Tracking (for ref params)
  // =========================================================================

  /// When a function returns ref and any ref param received a temporary,
  /// the return value has local lifetime (can't be stored).
  /// This tracks whether the current expression being analyzed came from
  /// a call where temporaries were passed to ref params.

  /// Infer the lifetime of a call expression's return value,
  /// considering whether temporaries were passed to ref params.
  Lifetime inferCallReturnLifetime(const CallExprAST& call);
};

}  // namespace sun
