/** Queries storage and control-flow properties of analyzed expressions. */
#pragma once
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

}  // namespace sun::semantic_analysis
