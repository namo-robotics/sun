/** Types the builtin methods of arrays and static_ptr. */
#pragma once

#include <string>
#include <vector>

#include "support/position.h"
#include "types/types.h"

/** Applies type rules to syntax and reports source-located semantic errors. */
namespace sun::semantic_analysis::type_analysis {

/**
 * The static_ptr&lt;T&gt; type when `type` is a static_ptr to a non-class,
 * else null. A static_ptr<Class> dispatches to the class's own methods
 * instead of the builtin ones.
 */
sun::types::StaticPointerType *asNonClassStaticPtr(
    const sun::types::TypePtr &type);

/** True for a builtin static_ptr&lt;T&gt; method name: length() or raw(). */
bool isStaticPtrMethod(const std::string &name);

/**
 * The result type of a static_ptr&lt;T&gt; builtin method call, checking the
 * argument count.
 */
sun::types::TypePtr resolveStaticPtrMethodType(
    const sun::types::StaticPointerType &ptrType, const std::string &name,
    size_t argCount, const sun::support::Position &loc);

/** True for a builtin array method name: ndims() or dim(i). */
bool isArrayMethod(const std::string &name);

/**
 * The result type of an array builtin method call (on a sized array or a
 * `ref array<T>` view), checking the arguments.
 */
sun::types::TypePtr resolveArrayMethodType(
    const std::string &name, const std::vector<sun::types::TypePtr> &argTypes,
    const sun::support::Position &loc);

}  // namespace sun::semantic_analysis::type_analysis
