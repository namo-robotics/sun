// control_flow.h — Questions about how control moves through an expression

#pragma once

#include "ast/ast_fwd.h"

/**
 * True when control never falls out of `expr`: it ends in a `return` or a
 * `throw`, or is an `if` whose branches all do. Whatever such an expression
 * did cannot reach the code that follows it, so a pass that carries state
 * forward should leave it behind.
 */
bool exprDiverges(const ExprAST& expr);
