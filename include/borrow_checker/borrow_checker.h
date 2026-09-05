// include/borrow_checker/borrow_checker.h
// Main borrow checker - validates reference safety at compile time

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "borrow_checker/borrow_state.h"
#include "borrow_checker/loan.h"
#include "semantic_analysis/types.h"
#include "support/error.h"

namespace sun {

/// A single borrow checking error with source location and optional related
/// locations. Rendering in the standard compiler format (source line and
/// caret) is done by the driver, which has the source text at hand.
struct BorrowError {
  std::string message;
  Position location;
  std::vector<Position> relatedLocations;  // Where conflicting borrows occurred
};

/// Turn the borrow errors of one compilation into the single error the driver
/// throws. The first error gives the message and location; every other error
/// and related borrow rides along as a related diagnostic, so printing the
/// error shows them all with carets and a tool catching it (the language
/// server) can point at each one in the source. Requires a non-empty list.
SunError buildBorrowCheckError(const std::vector<BorrowError>& errors);

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
  void checkBlockExpr(const BlockExprAST& block, size_t start = 0);
  void checkReturnStmt(const ReturnExprAST& ret);
  void checkFunctionDef(const FunctionAST& func);
  void checkLambdaDef(const LambdaAST& lambda);
  void checkClassDef(const ClassDefinitionAST& classDef);
  void checkMemberAccess(const MemberAccessAST& access);
  void checkMemberAssignment(const MemberAssignmentAST& assign);
  void checkIndexedAssignment(const IndexedAssignmentAST& assign);
  void checkArrayLiteral(const ArrayLiteralAST& literal);
  void checkIndexExpr(const IndexAST& index);
  void checkCompoundAssignment(const CompoundAssignmentAST& assign);
  void checkVariableWrite(const std::string& varName, const TypePtr& valueType,
                          const Position& pos);
  bool isRefCapturingLambdaExpr(const ExprAST& expr) const;
  // A value that must not leave this frame: a call result built from a
  // lambda with a capture list (spawn stores that lambda inside the Thread
  // handle it returns), a local such a result was moved into, or a literal
  // holding one. See frameBoundVars_.
  bool isFrameBoundExpr(const ExprAST& expr) const;
  // Reject frame-bound arguments bound to by-value parameters: the callee
  // could keep them past this frame's death, and nothing in the parameter
  // type says so.
  void forbidFrameBoundByValueArgs(const CallExprAST& call,
                                   const std::vector<TypePtr>& paramTypes);
  void reportFrameBoundEscapeThroughCall(const Position& pos);
  // A lambda value whose captured environment provably lives in THIS frame:
  // a capture-list literal, a bound method of a frame-local receiver, or a
  // local one of those was assigned to. See frameSourcedLambdas_.
  bool isFrameSourcedLambdaExpr(const ExprAST& expr) const;
  // True when the named place outlives this frame: `this`, a ref
  // parameter's referent, or a global (any name this frame did not declare)
  bool nameOutlivesFrame(const std::string& base) const;
  // The scope depth the named storage was declared at, resolving ref
  // aliases to their ultimate target. Names that outlive the frame -
  // `this`, ref parameters, globals - rank as 0: outer than every local.
  size_t lookupDeclDepth(const std::string& name) const;
  // The scope depth a frame-sourced value's environment is pinned to: the
  // deepest declaration among a capture list's borrowed variables, a bound
  // method's receiver's declaration, or a tracked local's recorded bound.
  // An environment that only depends on storage outliving the frame ranks
  // as functionScopeDepth_, valid anywhere in the frame.
  size_t inferEnvDepth(const ExprAST& expr) const;
  // A frame-sourced environment pinned at envDepth is entering the named
  // frame-local destination. Rejects the store when the destination's scope
  // outlives the environment's - it would hold a dangling environment once
  // the inner scope ends. Returns true when the store is allowed.
  bool checkFrameStoreDepth(const std::string& destBase, size_t envDepth,
                            const Position& pos);
  // A lifetime as the caller-side checker sees it. Concrete: pinned to a
  // scope depth of this frame (deeper dies sooner). Symbolic: one of the
  // current signature's named lifetimes - valid in some ancestor frame the
  // name stands for. Outlives: outlives this frame with no name relating
  // it to anything (the elided, trusted case - today's semantics).
  struct LifetimeValue {
    enum class Kind { Outlives, Concrete, Symbolic };
    Kind kind = Kind::Outlives;
    size_t depth = 0;       // Concrete: declaration/environment scope depth
    std::string name;       // Symbolic: the signature lifetime's name
    std::string described;  // the variable it came from, for messages
  };
  // Does a value with lifetime src provably live at least as long as
  // storage with lifetime dst?
  static bool lifetimeValueOutlives(const LifetimeValue& src,
                                    const LifetimeValue& dst);
  // The lifetime of the captured environment an expression's value
  // carries: concrete for frame-sourced values, symbolic for named
  // parameters and calls composed of them, outlives for everything else.
  LifetimeValue inferEnvLifetimeValue(const ExprAST& expr) const;
  // The lifetime of the storage a call argument lets the callee write
  // into: the referent's declaration for a local, symbolic for a named
  // ref parameter, outlives for an elided one.
  LifetimeValue inferDestLifetimeValue(const ExprAST& arg) const;
  // The same, for a plain name (an assignment target).
  LifetimeValue destLifetimeValueForName(const std::string& base) const;
  // Enforce a callee's named-lifetime relations at one call: bind each
  // name to its contributing arguments, require every source to outlive
  // every destination sharing the name, and mark caller-local
  // destinations frame-bound at the sources' depth.
  void checkNamedLifetimesAtCall(const CallExprAST& call,
                                 const std::vector<TypePtr>& paramTypes);

  // A frame-sourced lambda is being stored into the named destination:
  // reject if the destination outlives the frame or was declared in an
  // outer scope than the lambda's environment, otherwise mark the
  // destination frame-bound so the carrier cannot cross a call boundary
  void noteFrameSourcedLambdaStore(const std::string& destBase, size_t envDepth,
                                   const Position& pos);
  // A ref-storing class value is landing in the named destination (a fresh
  // construction, or a holder local moved in). Rejects a destination that
  // outlives what the value borrows, and records the destination's own
  // bound so later moves keep the whole journey in check.
  void trackRefHolderStore(const std::string& destName, size_t destDepth,
                           const ExprAST& value, const Position& pos);
  // Conservatively relate callbacks whose generic parameters lost lifetime
  // names to every receiver and mutable-ref destination the callee can keep.
  void checkErasedLifetimeLambdaArgs(const CallExprAST& call,
                                     const std::vector<TypePtr>& paramTypes);
  // Does this type point into storage it does not own - a reference in any
  // field, transitively, or a '<'_>' lambda environment it can carry?
  bool classStoresRefs(const TypePtr& type) const;
  bool classStoresRefsWalk(const TypePtr& type,
                           std::unordered_set<const Type*>& visited) const;
  // Does this class hold a mutable reference anywhere in its fields?
  bool classStoresMutableRefs(const TypePtr& type) const;
  // Visit the by-ref inputs a holder-producing call keeps pointing into: a
  // constructor of a ref-storing class visits the arguments bound to its ref
  // init parameters; a call returning such a class by value visits the
  // receiver and the arguments bound to ref parameters. `mutableRef` says
  // whether the holder may write through that input. Returns false when the
  // call produces no holder.
  bool forEachHolderInput(
      const CallExprAST& call,
      const std::function<void(const ExprAST& input, bool mutableRef)>& visit)
      const;
  // A holder-producing call keeps pointing into its by-ref inputs after it
  // returns, so the value holds a loan on each until scope exit.
  void borrowHolderInputs(const CallExprAST& call);
  // Does a ref-storing value point into this frame? A construction or
  // holder-returning call fed a frame-local (or a temporary) by ref, or a
  // local whose recorded bound names one. A holder built only from `this`,
  // ref parameters and globals points at storage the caller owns.
  bool holderPointsIntoFrame(const ExprAST& value) const;

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

  // Locals holding a value a capture-list lambda was moved into (a Thread
  // handle from spawn over a `[ref x]` or `[x]` lambda). The lambda's
  // environment lives in this frame, so the value may move between locals
  // and be dropped here, but must not escape: not returned, not stored into
  // a field, an indexed slot or a global, and not passed to a by-value
  // parameter (the callee could keep it - in a field, a container element,
  // a global - and nothing in the parameter type says so). The lambda's own
  // capture loans pin the borrowed variables until scope exit; this set is
  // what keeps the value from outliving them. Lambdas themselves carry the
  // frame binding in their `<'_>` type, but the type does not say WHICH
  // frame - so lambda-typed locals sourced from THIS frame are tracked in
  // frameSourcedLambdas_ below, and objects such a lambda was stored into
  // land here, keeping the carrier from crossing a call boundary either.
  // The mapped value is the scope depth the buried environment is pinned
  // to: the carrier must not land in storage declared in an outer scope.
  std::unordered_map<std::string, size_t> frameBoundVars_;

  // Lambda-typed locals whose captured environment provably lives in THIS
  // frame: assigned from a capture-list literal, or from a bound method of
  // a frame-local receiver. A `<'_>` value received as a parameter is NOT
  // here - its environment lives in some ancestor frame, which outlives
  // this one, so it may be stored anywhere its type allows. A frame-sourced
  // one must not reach a destination that outlives the frame: not a field
  // of `this`, not an object behind a ref parameter, not a global. The
  // mapped value is the environment's pinned scope depth, as above.
  std::unordered_map<std::string, size_t> frameSourcedLambdas_;

  // Every name that names storage owned by this frame: declared locals,
  // by-value parameters and loop variables. What is NOT here outlives the
  // frame - `this`, ref parameters, globals - and must not be handed a
  // frame-sourced lambda.
  std::unordered_set<std::string> frameLocalNames_;

  // The scope depth each frame-local name was declared at. This is what
  // relates two locals from different scopes: a value pinned to an inner
  // scope must not be stored into a name declared in an outer one, even
  // though both live in the same frame (issue #178).
  std::unordered_map<std::string, size_t> declDepths_;

  // Locals holding a ref-storing class value, mapped to the deepest
  // declaration among the variables the value borrows. The holder - and
  // every local it later moves into - must not be declared in an outer
  // scope than that bound, or it would keep the borrowed storage's address
  // past its death (issue #178, Rule 5's sub-frame gap).
  std::unordered_map<std::string, size_t> refHolderBounds_;

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

  // Named lifetimes of the current function's own parameters, from the
  // signature's annotations: a lambda param's environment name
  // ('cb: <'a>(i32) => i32') and a ref param's referent name
  // ('dst: ref 'a Holder'). Empty-name params are simply absent.
  std::unordered_map<std::string, std::string> paramEnvNames_;
  std::unordered_map<std::string, std::string> refParamNames_;
  // Class-slot bindings of ref parameters ('bus: ref Bus<'this>' binds
  // Bus's declared lifetimes, positionally, to our signature's names)
  std::unordered_map<std::string, std::vector<std::string>>
      refParamClassBindings_;

  // The current function's declared return lifetime, from a
  // '<'a>(...) => ...' return annotation; empty when the return type
  // is not a named frame-bound lambda.
  std::string returnLifetimeName_;

  // The lifetime names declared by the class whose methods are being
  // checked ('class Bus<'a>'): values bound to them may enter fields of
  // `this`, because the class's contract makes them outlive its objects.
  std::vector<std::string> activeClassLifetimes_;

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
