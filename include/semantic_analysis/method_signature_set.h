#pragma once

#include <unordered_set>

#include "semantic_analysis/callable_signature.h"
#include "semantic_analysis/semantic_context.h"
#include "semantic_analysis/type_analysis/type_resolver.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Detect duplicate method signatures, comparing generic binders by position.
 */
class MethodSignatureSet {
 public:
  /** Use the declaration's context to substitute generic parameter bindings. */
  MethodSignatureSet(SemanticContext& context,
                     type_analysis::TypeResolver& types)
      : context_(context), resolver_(types) {}

  /** Return false when an equivalent source method has already been recorded.
   */
  bool insert(const sun::ast::PrototypeAST& prototype,
              const std::vector<sun::types::TypePtr>& parameters) {
    sun::semantic_analysis::CallableSignature signature{prototype.getName(),
                                                        parameters};
    SemanticContext::ScopeSwitchGuard scope(context_, context_.scope());
    const auto& binders = prototype.getTypeParameters();
    if (!binders.empty()) {
      for (size_t i = binders_.size(); i < binders.size(); ++i)
        binders_.push_back(
            prototype.declarationIdentity().typeParameters.at(i));
      std::vector<sun::types::TypePtr> bindings;
      for (size_t i = 0; i < binders.size(); ++i)
        bindings.push_back(
            binders[i].toSunType(context_.declarationTable(), binders_[i]));
      context_.enterTypeParamScope(prototype.getTypeParameterNames(), bindings);
      signature.parameters.clear();
      for (const auto& [name, annotation] : prototype.getArgs())
        signature.parameters.push_back(
            resolver_.typeAnnotationToType(annotation));
    }
    return signatures_.insert(std::move(signature)).second;
  }

 private:
  SemanticContext& context_;
  type_analysis::TypeResolver& resolver_;
  std::vector<sun::semantic_analysis::DeclarationId> binders_;
  std::unordered_set<sun::semantic_analysis::CallableSignature,
                     sun::semantic_analysis::CallableSignatureHash>
      signatures_;
};

}  // namespace sun::semantic_analysis
