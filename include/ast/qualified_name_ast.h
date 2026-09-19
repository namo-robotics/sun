// qualified_name_ast.h — QualifiedNameAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/analysis.h"
#include "ast/expr_ast.h"

namespace sun::ast {

// Qualified name expression: Module.name or Namespace::name
class QualifiedNameAST : public ExprAST {
  std::vector<std::string> parts;  // ["std", "Vec"] for std.Vec

 public:
  explicit QualifiedNameAST(std::vector<std::string> parts)
      : parts(std::move(parts)) {}

  ASTNodeType getType() const override { return ASTNodeType::QUALIFIED_NAME; }
  std::string toString() const override { return getFullName(); }

  const std::vector<std::string>& getParts() const { return parts; }

  // Get the namespace/module path (all parts except the last)
  std::vector<std::string> getNamespacePath() const {
    if (parts.size() <= 1) return {};
    return std::vector<std::string>(parts.begin(), parts.end() - 1);
  }

  // Get the final name (last part)
  const std::string& getName() const { return parts.back(); }

  // Get fully qualified name as string with dot separator (e.g., "std.Vec")
  std::string getFullName() const {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) result += ".";
      result += parts[i];
    }
    return result;
  }

  std::string dotLabel() const override { return "QualName\n" + getFullName(); }
};

}  // namespace sun::ast
