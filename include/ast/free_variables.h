// free_variables.h — Names an expression uses but does not declare

#pragma once

#include <set>
#include <string>

#include "ast/ast_fwd.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * The variables an expression reads or writes but does not declare: what a
 * lambda has to capture. `bound` names the ones already in scope. Nested
 * functions, types and modules are skipped, since they cannot reach an
 * enclosing local.
 */
std::set<std::string> collectFreeVariables(const ExprAST& expr,
                                           const std::set<std::string>& bound);

/**
 * The same over a block, adding each declaration to `bound` as it is
 * reached so later statements do not count it as free.
 */
std::set<std::string> collectFreeVariablesInBlock(const BlockExprAST& block,
                                                  std::set<std::string> bound);

}  // namespace sun::ast
