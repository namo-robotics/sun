/** Provides focused checking services for one semantic analysis session. */
#pragma once
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "semantic_analysis/semantic_context.h"
/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
class SemanticAnalyzer;
/** Resolves expressions and checks their types, conversions, and value access.
 */
class ExpressionAnalyzer {
 public:
  /** Borrows the session context and the cooperating analysis services. */
  ExpressionAnalyzer(SemanticContext &context, SemanticAnalyzer &analyzer)
      : ctx_(context), sema_(analyzer) {}
  /**
   * Throw when `target` names a variable the enclosing lambda picked up
   * without a capture list. That capture is the closure's own copy, so a
   * borrow of it would alias the copy rather than the original. Every borrow
   * site calls this, so the explanation is worded once.
   */
  void rejectBorrowOfByValueCapture(const sun::ast::ExprAST &target,
                                    const sun::support::Position &loc);

  /**
   * Throw unless `target` is something a borrow can bind: an addressable
   * lvalue that is not a slice, a class __index__ result, or a packed field.
   */
  void validateBorrowTarget(const sun::ast::ExprAST &target,
                            const sun::support::Position &loc);

  /**
   * Resolves declarations and checks types in this lambda expression, recording
   * the results on its syntax nodes.
   */
  void analyzeLambdaExpr(sun::ast::LambdaAST &lambda);

  /**
   * Resolves declarations and checks types in this ternary expression,
   * recording the results on its syntax nodes.
   */
  void analyzeTernaryExpr(sun::ast::TernaryExprAST &ternary,
                          sun::types::TypePtr expectedType);

  /** Resolve a variable or callable name, recording its declaration identity.
   */
  sun::types::TypePtr resolveVariableReferenceType(
      const sun::ast::VariableReferenceAST &expr);

  /** Resolve the receiver member and enforce access rules. */
  sun::types::TypePtr resolveModuleMemberType(
      const sun::ast::MemberAccessAST &expr,
      const sun::types::TypePtr &objectType, const std::string &memberName);

  /** Resolve the receiver member and enforce access rules. */
  sun::types::TypePtr resolveClassMemberType(
      const sun::ast::MemberAccessAST &expr,
      const sun::types::TypePtr &objectType, const std::string &memberName);

  /** Resolve the receiver member and enforce access rules. */
  sun::types::TypePtr resolveTypeParameterMemberType(
      const sun::ast::MemberAccessAST &expr,
      const sun::types::TypePtr &objectType, const std::string &memberName);

  /** Resolve a member against its prepared receiver and record its identity. */
  sun::types::TypePtr resolveMemberType(const sun::ast::MemberAccessAST &expr);

  /** Resolve a module identity and enforce its visibility. */
  sun::types::TypePtr resolveModuleReference(const sun::ast::ExprAST &expr);

  /** Require a type already established by semantic analysis. */
  static sun::types::TypePtr requireResolvedType(const sun::ast::ExprAST &expr);

  /** Select a block result from statements analyzed in their original scope. */
  sun::types::TypePtr preparedBlockType(const sun::ast::BlockExprAST &block);

  /** Select the first reachable match value after pattern and body checking. */
  sun::types::TypePtr preparedMatchType(const sun::ast::MatchExprAST &match);

  /**
   * Value expressions (analysis_expressions.cpp)
   */
  void analyzeNumberLiteral(sun::ast::ExprAST &expr,
                            sun::types::TypePtr expectedType);

  /**
   * Resolves declarations and checks types in this array literal, recording the
   * results on its syntax nodes.
   */
  void analyzeArrayLiteral(sun::ast::ArrayLiteralAST &arrLit,
                           sun::types::TypePtr expectedType = nullptr);

  /** Compute an array result from checked elements and a separate contextual
   * hint. */
  void resolveArrayLiteralResult(sun::ast::ArrayLiteralAST &literal,
                                 sun::types::TypePtr expectedType);

  /**
   * Resolves declarations and checks types in this index expression, recording
   * the results on its syntax nodes.
   */
  void analyzeIndexExpr(sun::ast::IndexAST &arrIdx);

  /**
   * Resolves declarations and checks types in this slice expression, recording
   * the results on its syntax nodes.
   */
  void analyzeSliceExpr(sun::ast::ExprAST &expr);

  /** Requires a handler or throwing function unless integer division is safe.
   */
  void checkIntegerDivision(const sun::ast::ExprAST &lhs,
                            const sun::ast::ExprAST &rhs,
                            sun::parsing::TokenKind op,
                            const sun::support::Position &location);

  /**
   * Resolves declarations and checks types in this binary expression, recording
   * the results on its syntax nodes.
   */
  void analyzeBinaryExpr(sun::ast::BinaryExprAST &binExpr,
                         sun::types::TypePtr expectedType);

  /**
   * Resolves declarations and checks types in this unary expression, recording
   * the results on its syntax nodes.
   */
  void analyzeUnaryExpr(sun::ast::UnaryExprAST &unaryExpr);

  /**
   * Resolves declarations and checks types in this member access, recording the
   * results on its syntax nodes.
   */
  void analyzeMemberAccess(sun::ast::MemberAccessAST &memberAccess,
                           sun::types::TypePtr expectedType);

  /**
   * Resolves declarations and checks types in this qualified name, recording
   * the results on its syntax nodes.
   */
  void analyzeQualifiedName(sun::ast::QualifiedNameAST &qualName);

  /**
   * Constness. A place (`x`, `x.f`, `x[i]`, `this.f`, `a ? x : y`, a call
   * result) cannot be changed when its base is a `const` variable, a
   * `const ref`, or `this` inside a const method. Returns why, or an empty
   * string when the place may be changed.
   */
  std::string immutableBaseOf(const sun::ast::ExprAST &place);

  /** Throws "Cannot <action> <why>" when `place` cannot be changed. */
  void requireMutablePlace(const sun::ast::ExprAST &place,
                           const std::string &action,
                           const sun::support::Position &loc);

  /**
   * `value` is consumed by value: a compound field read out of an immutable
   * object (a partial move) or a constant global is rejected.
   */
  void checkMoveSource(const sun::ast::ExprAST &value,
                       const sun::support::Position &loc);

  /**
   * An argument bound to a `ref T` parameter must be a mutable place; one
   * bound to a by-value compound parameter is a move (see checkMoveSource).
   */
  void checkArgumentPlaces(
      const std::vector<std::unique_ptr<sun::ast::ExprAST>> &args,
      const std::vector<sun::types::TypePtr> &paramTypes,
      const std::string &callee, const sun::support::Position &loc);

  /** Require an unsafe block when a call has a caller-side safety contract. */
  void checkUnsafeCall(bool requiresUnsafe, const std::string &name,
                       const sun::support::Position &loc) const;

  /**
   * Calling `method` on `receiver`: a non-const method needs a mutable
   * receiver. Returns true when the receiver is immutable, so a `ref T`
   * result must be downgraded to `const ref T`.
   */
  bool checkMethodReceiver(const sun::ast::ExprAST &receiver,
                           const std::string &name, bool methodIsConst,
                           bool isConstructor,
                           const sun::support::Position &loc);

  /** Checks a value expression after declaration and statement dispatch. */
  void analyzeExpression(sun::ast::ExprAST &expr,
                         sun::types::TypePtr expectedType = nullptr);

  /**
   * Reject access to C-owned global storage outside an unsafe block. (Calls
   * into C and unsafe intrinsics are gated the same way by CallAnalyzer.)
   */
  void checkExternVariableAccessAllowed(
      const VariableInfo &info, const std::string &displayName,
      const sun::support::Position &loc) const;

 private:
  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;

  /**
   * Resolve a `{ field: value }` literal against the type the context
   * expects. A struct literal has no type of its own, so without an expected
   * class type there is nothing to check the field names against.
   */
  void analyzeStructLiteral(sun::ast::StructLiteralAST &literal,
                            const sun::types::TypePtr &expectedType);

  /**
   * If the member access names a class method in value position, resolve it
   * as a bound method reference: pick the overload (using expectedType when
   * the name is overloaded), set a LambdaType resolved type and the
   * isBoundMethodRef flag. No-op for fields, non-class receivers, and
   * call-position callees (those never route through here).
   */
  void maybeResolveBoundMethodRef(sun::ast::MemberAccessAST &memberAccess,
                                  sun::types::TypePtr expectedType);
};
}  // namespace sun::semantic_analysis
