// qualified_name_ast.h — QualifiedNameAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/analysis.h"
#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Qualified name expression: Module.name or Namespace::name
 */
class QualifiedNameAST : public ExprAST {
  std::vector<std::string> parts;  // ["std", "Vec"] for std.Vec

 public:
  /** Creates this syntax node from its operands and declaration information. */
  explicit QualifiedNameAST(std::vector<std::string> parts)
      : parts(std::move(parts)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::QUALIFIED_NAME; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return getFullName(); }

  /** Returns the parts stored by this object. */
  const std::vector<std::string>& getParts() const { return parts; }

  /**
   * Get the namespace/module path (all parts except the last)
   */
  std::vector<std::string> getNamespacePath() const {
    if (parts.size() <= 1) return {};
    return std::vector<std::string>(parts.begin(), parts.end() - 1);
  }

  /**
   * Get the final name (last part)
   */
  const std::string& getName() const { return parts.back(); }

  /**
   * Get fully qualified name as string with dot separator (e.g., "std.Vec")
   */
  std::string getFullName() const {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) result += ".";
      result += parts[i];
    }
    return result;
  }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "QualName\n" + getFullName(); }
};

}  // namespace sun::ast
