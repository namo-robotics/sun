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

}  // namespace sun::semantic_analysis
