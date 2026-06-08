// qualified_name_ast.h — QualifiedNameAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/analysis.h"
#include "ast/expr_ast.h"

// Qualified name expression: Module.name or Namespace::name
class QualifiedNameAST : public ExprAST {
  std::vector<std::string> parts;  // ["sun", "Vec"] for sun.Vec

 protected:
  // Override to allocate QualifiedNameExprAnalysis instead of base ExprAnalysis
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<QualifiedNameExprAnalysis>();
    }
  }

 private:
  // Access as QualifiedNameExprAnalysis
  QualifiedNameExprAnalysis& qnAnalysis() const {
    ensureAnalysis();
    return static_cast<QualifiedNameExprAnalysis&>(*analysis_);
  }

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

  // Get fully qualified name as string with dot separator (e.g., "sun.Vec")
  std::string getFullName() const {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) result += ".";
      result += parts[i];
    }
    return result;
  }

  // Get mangled name for LLVM symbols (e.g., "sun_Vec")
  // Uses resolved name if set by semantic analyzer, otherwise computes from
  // parts
  std::string getMangledName() const {
    if (analysis_ && !static_cast<QualifiedNameExprAnalysis&>(*analysis_)
                          .resolvedMangledName.empty()) {
      return static_cast<QualifiedNameExprAnalysis&>(*analysis_)
          .resolvedMangledName;
    }
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) result += "_";
      result += parts[i];
    }
    return result;
  }

  void setResolvedMangledName(std::string name) const {
    qnAnalysis().resolvedMangledName = std::move(name);
  }

  std::string dotLabel() const override { return "QualName\n" + getFullName(); }
};
