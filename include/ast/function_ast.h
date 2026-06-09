// function_ast.h — FunctionAST class

#pragma once

#include <cassert>
#include <map>
#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"
#include "ast/prototype_ast.h"

class FunctionAST : public ExprAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<BlockExprAST> Body;

 protected:
  // Override to allocate FunctionAnalysis instead of base ExprAnalysis
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<FunctionAnalysis>();
    }
  }

 private:
  // Access as FunctionAnalysis
  FunctionAnalysis& funcAnalysis() const {
    ensureAnalysis();
    return static_cast<FunctionAnalysis&>(*analysis_);
  }

 public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<BlockExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}

  ASTNodeType getType() const override { return ASTNodeType::FUNCTION; }
  std::string toString() const override {
    std::string result = "function " + Proto->getName() + "(";
    const auto& args = Proto->getArgs();
    for (size_t i = 0; i < args.size(); ++i) {
      if (i > 0) result += ", ";
      result += args[i].first + ": " + args[i].second.toString();
    }
    result += ")";
    if (Proto->hasReturnType())
      result += " " + Proto->getReturnType()->toString();
    if (Body) result += " " + Body->toString();
    return result;
  }

  // Add this method to allow moving the prototype out
  std::unique_ptr<PrototypeAST> releaseProto() { return std::move(Proto); }

  const PrototypeAST& getProto() const { return *Proto; }
  const BlockExprAST& getBody() const {
    assert(Body && "getBody() called on extern function with no body");
    return *Body;
  }

  // Set body (for replacing empty stub with parsed body)
  void setBody(std::unique_ptr<BlockExprAST> newBody) {
    Body = std::move(newBody);
  }

  // Check if function is an extern declaration (no body)
  bool isExtern() const { return Body == nullptr; }
  bool hasBody() const { return Body != nullptr; }

  // Specialization storage for generic functions
  // Called by semantic analyzer when a generic function is instantiated
  void addSpecialization(const std::string& mangledName,
                         std::shared_ptr<FunctionAST> specializedAST) const {
    funcAnalysis().specializations[mangledName] = std::move(specializedAST);
  }
  const std::map<std::string, std::shared_ptr<FunctionAST>>&
  getSpecializations() const {
    return funcAnalysis().specializations;
  }
  bool hasSpecialization(const std::string& mangledName) const {
    return analysis_ &&
           static_cast<FunctionAnalysis&>(*analysis_)
                   .specializations.find(mangledName) !=
               static_cast<FunctionAnalysis&>(*analysis_).specializations.end();
  }
  std::shared_ptr<FunctionAST> getSpecialization(
      const std::string& mangledName) const {
    if (!analysis_) return nullptr;
    auto& specs = static_cast<FunctionAnalysis&>(*analysis_).specializations;
    auto it = specs.find(mangledName);
    return it != specs.end() ? it->second : nullptr;
  }

  std::string dotLabel() const override {
    std::string label = "function \n" + Proto->getName();
    if (Proto->hasReturnType())
      label += " -> " + Proto->getReturnType()->toString();
    return label;
  }
};
