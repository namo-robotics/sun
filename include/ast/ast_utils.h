// ast_utils.h — Common utility functions for AST operations

#pragma once

#include <memory>
#include <vector>

#include "ast/ast_fwd.h"
#include "ast/expr_ast.h"  // Required for ExprAST::clone()
#include "ast/type_annotation.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Helper to clone a vector of unique_ptr<ExprAST>
 */
inline std::vector<std::unique_ptr<ExprAST>> cloneExprVector(
    const std::vector<std::unique_ptr<ExprAST>>& vec) {
  std::vector<std::unique_ptr<ExprAST>> result;
  result.reserve(vec.size());
  for (const auto& expr : vec) {
    result.push_back(expr->clone());
  }
  return result;
}

/**
 * Helper to clone a vector of unique_ptr<TypeAnnotation>
 */
inline std::vector<std::unique_ptr<TypeAnnotation>> cloneTypeAnnotationVector(
    const std::vector<std::unique_ptr<TypeAnnotation>>& vec) {
  std::vector<std::unique_ptr<TypeAnnotation>> result;
  result.reserve(vec.size());
  for (const auto& ta : vec) {
    result.push_back(std::make_unique<TypeAnnotation>(*ta));
  }
  return result;
}

/**
 * Forgets the types analysis recorded on a tree, including match arm tags and
 * binding types, so a copy of a generic body can be analyzed afresh. Visits
 * every child, including lambda and function bodies. Declaration identities
 * and other annotations are kept.
 */
void clearResolvedTypes(const ExprAST& expr);

}  // namespace sun::ast
