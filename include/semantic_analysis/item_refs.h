// item_refs.h — Naming class, interface and module members for the uniform
// access-denial message.
//
// Kept apart from access_checker.h so that header stays free of the type and
// AST headers: the moon builder and tooling include it only for the predicate.

#pragma once

#include "ast.h"
#include "semantic_analysis/access_checker.h"
#include "semantic_analysis/semantic_scope.h"
#include "semantic_analysis/types.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Name a class field for a uniform access-denial message. */
ItemRef fieldRef(const sun::semantic_analysis::ClassType& cls,
                 const sun::semantic_analysis::ClassField& f);

/** Name a class method for a uniform access-denial message. */
ItemRef methodRef(const sun::semantic_analysis::ClassType& cls,
                  const sun::semantic_analysis::ClassMethod& m);

/** Name an interface field for a uniform access-denial message. */
ItemRef fieldRef(const sun::semantic_analysis::InterfaceType& iface,
                 const sun::semantic_analysis::InterfaceField& f);

/** Name an interface method for a uniform access-denial message. */
ItemRef methodRef(const sun::semantic_analysis::InterfaceType& iface,
                  const sun::semantic_analysis::InterfaceMethod& m);

/** Name a module for a uniform access-denial message. */
inline ItemRef moduleRef(const sun::semantic_analysis::ModuleScope& scope) {
  return sun::semantic_analysis::accessItem(scope);
}

/** `deinit` is compiler-invoked and therefore always public. */
Visibility methodVisibility(const sun::ast::FunctionAST& method);

}  // namespace sun::semantic_analysis
