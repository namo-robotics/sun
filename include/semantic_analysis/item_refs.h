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

namespace sun::access {

/** Name a class field for a uniform access-denial message. */
ItemRef fieldRef(const sun::ClassType& cls, const sun::ClassField& f);

/** Name a class method for a uniform access-denial message. */
ItemRef methodRef(const sun::ClassType& cls, const sun::ClassMethod& m);

/** Name an interface field for a uniform access-denial message. */
ItemRef fieldRef(const sun::InterfaceType& iface, const sun::InterfaceField& f);

/** Name an interface method for a uniform access-denial message. */
ItemRef methodRef(const sun::InterfaceType& iface,
                  const sun::InterfaceMethod& m);

/** Name a module for a uniform access-denial message. */
inline ItemRef moduleRef(const ModuleScope& scope) { return accessItem(scope); }

/** `deinit` is compiler-invoked and therefore always public. */
Visibility methodVisibility(const FunctionAST& method);

}  // namespace sun::access
