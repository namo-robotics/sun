// using_ast.h — UsingAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/expr_ast.h"

// Using declaration: using Namespace::name; or using Namespace::*;
// Also supports: using Module; (imports all from module)
class UsingAST : public ExprAST {
  std::vector<std::string> namespacePath;  // The namespace path
  std::string target;    // The specific symbol name, or "*" for module import
                         // (using std;)
  bool isModuleImport_;  // true for "using std;" (imports whole module)

 public:
  UsingAST(std::vector<std::string> nsPath, std::string targetName)
      : namespacePath(std::move(nsPath)),
        target(std::move(targetName)),
        isModuleImport_(target == "*") {}

  ASTNodeType getType() const override { return ASTNodeType::USING; }
  std::string toString() const override {
    if (target == "*") {
      return "using " + getNamespacePathString();
    }
    return "using " + getNamespacePathString() + "." + target;
  }

  const std::vector<std::string>& getNamespacePath() const {
    return namespacePath;
  }
  const std::string& getTarget() const { return target; }
  bool isModuleImport() const { return isModuleImport_; }
  /** Record whether semantic binding identified a whole-module import. */
  void setModuleImport(bool value) { isModuleImport_ = value; }

  // Get the full path as string (e.g., "Math.Trig" or "Math::Trig")
  std::string getNamespacePathString() const {
    std::string result;
    for (size_t i = 0; i < namespacePath.size(); ++i) {
      if (i > 0) result += ".";
      result += namespacePath[i];
    }
    return result;
  }
  std::string dotLabel() const override {
    if (target == "*") {
      return "Using\n" + getNamespacePathString();
    }
    return "Using\n" + getNamespacePathString() + "." + target;
  }
};
