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
  bool CAbi = false;
  bool IsTest = false;
  size_t fieldInitializerCount_ = 0;
  bool synthesizedConstructor_ = false;

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

  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (Body) Body->forEachChildSlot(fn);
  }
  std::string toString() const override {
    std::string result = std::string(isPublic() ? "public " : "") +
                         (IsTest ? "test_function " : "function ") +
                         Proto->getName() + "(";
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
  PrototypeAST& getProtoMut() { return *Proto; }
  const BlockExprAST& getBody() const {
    assert(Body && "getBody() called on extern function with no body");
    return *Body;
  }

  // Set body (for replacing empty stub with parsed body)
  void setBody(std::unique_ptr<BlockExprAST> newBody) {
    Body = std::move(newBody);
  }

  /** Returns the number of generated field assignments before the source body.
   */
  size_t getFieldInitializerCount() const { return fieldInitializerCount_; }
  /** Records the generated prefix so lowering and analysis do not repeat it. */
  void setFieldInitializerCount(size_t count) {
    fieldInitializerCount_ = count;
  }
  /** Reports whether field defaults supplied this constructor. */
  bool isSynthesizedConstructor() const { return synthesizedConstructor_; }
  /** Marks a constructor supplied by field defaults. */
  void setSynthesizedConstructor(bool value) {
    synthesizedConstructor_ = value;
  }

  // Check if function is a bodyless declaration. True for both `extern
  // function` (C ABI) and `declare function` (Sun forward declaration).
  bool isExtern() const { return Body == nullptr; }

  // True only for `extern function` — a C symbol linked by its exact name,
  // with no module scope and no overload suffix. `declare function` is a
  // forward declaration of a Sun function and keeps normal mangling, so the
  // two must not be conflated even though both are bodyless.
  bool isCExtern() const { return CAbi; }
  void setCExtern(bool v) { CAbi = v; }

  // True for `test_function` declarations. Test functions are compiled only
  // into the test binary; production builds and .moon bundles strip them.
  bool isTest() const { return IsTest; }
  void setIsTest(bool v) { IsTest = v; }
  bool hasBody() const { return Body != nullptr; }
  bool hasNonEmptyBody() const { return Body && !Body->getBody().empty(); }

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
    std::string label =
        std::string(IsTest ? "test_function \n" : "function \n") +
        Proto->getName();
    if (Proto->hasReturnType())
      label += " -> " + Proto->getReturnType()->toString();
    return label;
  }
};
