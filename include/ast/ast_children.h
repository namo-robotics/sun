// ast_children.h — Read-only enumeration of every direct child of a node

#pragma once

#include <functional>

#include "ast/expr_ast.h"

using ChildFn = std::function<void(const ExprAST&)>;

// Calls fn for each direct child expression of node, in source order. Unlike
// ExprAST::forEachChildSlot (a rewriting hook that forwards through strongly
// typed children), this visits every child, including function bodies, class
// and interface methods, slice bounds, match patterns and struct literal
// values. Used by tooling that walks the tree (language server lookups).
void forEachChild(const ExprAST& node, const ChildFn& fn);
