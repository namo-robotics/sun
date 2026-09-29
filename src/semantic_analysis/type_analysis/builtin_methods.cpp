/** Types the builtin methods of arrays and static_ptr. */
#include "semantic_analysis/type_analysis/builtin_methods.h"

#include "support/error.h"
#include "types/type_utils.h"

using sun::support::logAndThrowError;
using sun::types::StaticPointerType;
using sun::types::TypePtr;
using sun::types::Types;
using sun::types::unwrapRef;

/** Applies type rules to syntax and reports source-located semantic errors. */
namespace sun::semantic_analysis::type_analysis {

/** Unwraps a static_ptr unless it points to a class. */
StaticPointerType* asNonClassStaticPtr(const TypePtr& type) {
  if (!type || !type->isStaticPointer()) return nullptr;
  auto* staticPtr = static_cast<StaticPointerType*>(type.get());
  const auto& pointee = staticPtr->getPointeeType();
  if (pointee && pointee->isClass()) return nullptr;
  return staticPtr;
}

/** Matches the static_ptr accessor names. */
bool isStaticPtrMethod(const std::string& name) {
  return name == "length" || name == "raw";
}

/** Matches the array accessor names. */
bool isArrayMethod(const std::string& name) {
  return name == "ndims" || name == "dim";
}

/** Checks the arguments of ndims() and dim(i); both return i64. */
TypePtr resolveArrayMethodType(const std::string& name,
                               const std::vector<TypePtr>& argTypes,
                               const sun::support::Position& loc) {
  if (!isArrayMethod(name)) {
    logAndThrowError(
        "Array has no method '" + name + "'; available: ndims(), dim(i)", loc);
  }
  if (name == "ndims") {
    if (!argTypes.empty()) {
      logAndThrowError("array.ndims() takes no arguments", loc);
    }
    return Types::Int64();
  }
  if (argTypes.size() != 1 || !argTypes[0] ||
      !unwrapRef(argTypes[0])->isIntegral()) {
    logAndThrowError("array.dim(i) takes one integer argument", loc);
  }
  return Types::Int64();
}

/** Checks the call takes no arguments and picks the result type. */
TypePtr resolveStaticPtrMethodType(const StaticPointerType& ptrType,
                                   const std::string& name, size_t argCount,
                                   const sun::support::Position& loc) {
  if (!isStaticPtrMethod(name)) {
    logAndThrowError(
        "static_ptr has no method '" + name + "'; available: length(), raw()",
        loc);
  }
  if (argCount != 0) {
    logAndThrowError("static_ptr." + name + "() takes no arguments", loc);
  }
  if (name == "length") return Types::Int64();
  return Types::RawPointer(ptrType.getPointeeType());
}

}  // namespace sun::semantic_analysis::type_analysis
