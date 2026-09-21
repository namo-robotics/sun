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

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::semantic_analysis::DeclarationId;

/** A function definition pairing its signature with an optional body. */
class FunctionAST : public ExprAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<BlockExprAST> Body;
  bool CAbi = false;
  bool IsTest = false;
  size_t fieldInitializerCount_ = 0;
  bool synthesizedConstructor_ = false;

 protected:
  /**
   * Override to allocate FunctionAnalysis instead of base ExprAnalysis
   */
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<FunctionAnalysis>();
    }
  }

 private:
  /**
   * Access as FunctionAnalysis
   */
  FunctionAnalysis& funcAnalysis() const {
    ensureAnalysis();
    return static_cast<FunctionAnalysis&>(*analysis_);
  }

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<BlockExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::FUNCTION; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (Body) Body->forEachChildSlot(fn);
  }
  /** Returns a readable representation for diagnostics and debugging. */
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

  /**
   * Add this method to allow moving the prototype out
   */
  std::unique_ptr<PrototypeAST> releaseProto() { return std::move(Proto); }

  /** A function and its prototype denote the same declaration. */
  DeclarationId getDeclarationId() const override {
    return Proto->getDeclarationId();
  }
  /** Assign the prototype's declaration identity. */
  void setDeclarationId(DeclarationId id) const override {
    Proto->setDeclarationId(id);
  }
  /** Access the identities owned by the prototype. */
  sun::semantic_analysis::DeclarationIdentity& declarationIdentity()
      const override {
    return Proto->declarationIdentity();
  }
  /** Clear body and signature results while retaining identities. */
  void clearComputedAnalysis() const override {
    ExprAST::clearComputedAnalysis();
    Proto->clearComputedAnalysis();
  }
  /** Discard the function's annotations and its prototype's annotations. */
  void resetAnalysisSession() const override {
    ExprAST::resetAnalysisSession();
    Proto->resetAnalysisSession();
  }
  /** Provides the function signature independently of its body. */
  const PrototypeAST& getProto() const { return *Proto; }
  /** Returns the modifiable function signature. */
  PrototypeAST& getProtoMut() { return *Proto; }
  /** Provides access to the expressions that make up the body. */
  const BlockExprAST& getBody() const {
    assert(Body && "getBody() called on extern function with no body");
    return *Body;
  }

  /**
   * Set body (for replacing empty stub with parsed body)
   */
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

  /**
   * Check if function is a bodyless declaration. True for both `extern
   * function` (C ABI) and `declare function` (Sun forward declaration).
   */
  bool isExtern() const { return Body == nullptr; }

  /**
   * True only for `extern function` — a C symbol linked by its exact name,
   * with no module scope. `declare function` is a forward declaration of a
   * Sun function and keeps the symbol derived from its declaration, so the
   * two must not be conflated even though both are bodyless.
   */
  bool isCExtern() const { return CAbi; }
  /** Marks whether the function uses the C calling convention. */
  void setCExtern(bool v) { CAbi = v; }

  /**
   * True for `test_function` declarations. Test functions are compiled only
   * into the test binary; production builds and .moon bundles strip them.
   */
  bool isTest() const { return IsTest; }
  /** Marks whether the function is a discoverable test. */
  void setIsTest(bool v) { IsTest = v; }
  /** Reports whether a function body is present rather than just a declaration.
   */
  bool hasBody() const { return Body != nullptr; }
  /** Reports whether this object has non empty body. */
  bool hasNonEmptyBody() const { return Body && !Body->getBody().empty(); }

  /**
   * Specialization storage for generic functions
   * Called by semantic analyzer when a generic function is instantiated
   */
  void addSpecialization(DeclarationId id,
                         std::shared_ptr<FunctionAST> specializedAST) const {
    funcAnalysis().specializations[id] = std::move(specializedAST);
  }
  const std::map<DeclarationId, std::shared_ptr<FunctionAST>>&
  /** Provides the concrete instances created from this generic declaration. */
  getSpecializations() const {
    return funcAnalysis().specializations;
  }
  /** Reports whether an instance already exists for the supplied type
   * arguments. */
  bool hasSpecialization(DeclarationId id) const {
    return analysis_ &&
           static_cast<FunctionAnalysis&>(*analysis_)
                   .specializations.find(id) !=
               static_cast<FunctionAnalysis&>(*analysis_).specializations.end();
  }
  /** Looks up the instance previously created for the supplied type arguments.
   */
  std::shared_ptr<FunctionAST> getSpecialization(DeclarationId id) const {
    if (!analysis_) return nullptr;
    auto& specs = static_cast<FunctionAnalysis&>(*analysis_).specializations;
    auto it = specs.find(id);
    return it != specs.end() ? it->second : nullptr;
  }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override {
    std::string label =
        std::string(IsTest ? "test_function \n" : "function \n") +
        Proto->getName();
    if (Proto->hasReturnType())
      label += " -> " + Proto->getReturnType()->toString();
    return label;
  }
};

}  // namespace sun::ast
