#pragma once

#include <unordered_set>

#include "semantic_analysis/callable_signature.h"
#include "semantic_analysis/semantic_context.h"
#include "semantic_analysis/type_inferer.h"

/** Detect duplicate method signatures, comparing generic binders by position.
 */
class MethodSignatureSet {
 public:
  /** Use the declaration's context to substitute generic parameter bindings. */
  MethodSignatureSet(SemanticContext& context, TypeInferer& types)
      : context_(context), types_(types) {}

  /** Return false when an equivalent source method has already been recorded.
   */
  bool insert(const PrototypeAST& prototype,
              const std::vector<sun::TypePtr>& parameters) {
    sun::CallableSignature signature{prototype.getName(), parameters};
    SemanticContext::ScopeSwitchGuard scope(context_, context_.scope());
    const auto& binders = prototype.getTypeParameters();
    if (!binders.empty()) {
      for (size_t i = binders_.size(); i < binders.size(); ++i)
        binders_.push_back(binders[i].toSunType(
            context_.declarationTable(),
            prototype.declarationIdentity().typeParameters.at(i)));
      std::vector<sun::TypePtr> bindings(binders_.begin(),
                                         binders_.begin() + binders.size());
      context_.enterTypeParamScope(prototype.getTypeParameterNames(), bindings);
      signature.parameters.clear();
      for (const auto& [name, annotation] : prototype.getArgs())
        signature.parameters.push_back(types_.typeAnnotationToType(annotation));
    }
    return signatures_.insert(std::move(signature)).second;
  }

 private:
  SemanticContext& context_;
  TypeInferer& types_;
  std::vector<sun::TypePtr> binders_;
  std::unordered_set<sun::CallableSignature, sun::CallableSignatureHash>
      signatures_;
};
