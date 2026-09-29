/** Queries storage and control-flow properties of analyzed expressions. */
#pragma once
#include <vector>

#include "ast.h"

/** Checks expression properties during semantic analysis. */
namespace sun::semantic_analysis {
using sun::ast::ExprAST;

/**
 * A borrow binds the storage of an addressable lvalue: a variable, a field, or
 * an array element. Rejects everything else (temporaries, class `__index__`
 * results, slices), so `ref r = x` and `var r: ref T = x` agree.
 */
bool isBorrowableLvalue(const ExprAST& target);

/**
 * Does evaluating this statement guarantee the function exits — through a
 * return or a throw — rather than falling through to whatever comes next?
 * Conservative: anything unrecognized answers no. Two rules build on it: a
 * non-void body must end on a path where this answers yes (Sun has no
 * implicit returns), and a block whose body always exits produces no value
 * and cannot be bound.
 */
bool alwaysExits(const ExprAST& expr);

/**
 * Does this expression, or anything nested inside it, call a function? Counts
 * plain calls (`half(8)`) and generic calls (`make<i32>()`), and looks into
 * every child, including struct literal values, slice bounds and match
 * patterns.
 */
bool containsCall(const ExprAST& expr);

/**
 * The first `this` inside the expression, or null if there is none. Stops at
 * a class or interface defined inside the expression, because a `this` in
 * one of its methods refers to that type's own receiver, not the enclosing
 * one.
 */
const ExprAST* findThisUse(const ExprAST& expr);

/**
 * The type analysis already recorded on the expression. Throws an internal
 * error if there is none, since callers rely on the expression having been
 * analyzed first.
 */
sun::types::TypePtr requireResolvedType(const ExprAST& expr);

/**
 * The value type of an analyzed block: the type of its first `return`, else
 * of its last statement when the block produces a value, else void.
 */
sun::types::TypePtr preparedBlockType(const sun::ast::BlockExprAST& block);

/**
 * The arms of an analyzed match that can run: an arm for an enum variant an
 * earlier arm already covers is left out, and nothing after the first `_`
 * arm is included.
 */
std::vector<const sun::ast::MatchArm*> reachableMatchArms(
    const sun::ast::MatchExprAST& match);

/**
 * The value type of an analyzed match: that of the first reachable arm whose
 * body does not always leave (by return, throw, break or continue), else
 * void.
 */
sun::types::TypePtr preparedMatchType(const sun::ast::MatchExprAST& match);

/**
 * When a match produces an owned value, throws unless every reachable arm
 * that yields a value yields exactly that type.
 */
void checkOwnedMatchArmTypes(const sun::ast::MatchExprAST& match);

/**
 * When a match over a non-enum value produces an owned value, throws unless
 * every input is covered: by a `_` arm, or by `true` and `false` arms when
 * `discriminantType` is bool.
 */
void checkOwnedMatchCoverage(const sun::ast::MatchExprAST& match,
                             const sun::types::TypePtr& discriminantType);

}  // namespace sun::semantic_analysis
