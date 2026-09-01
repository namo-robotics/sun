// c_abi_types.h — Shared C-compatible type predicates.

#pragma once

#include "semantic_analysis/types.h"

namespace sun::c_abi {

/** Return whether an enum uses C's integer representation. */
inline bool isCStyleEnum(const TypePtr& type) {
  return type && type->isEnum() &&
         !static_cast<const EnumType*>(type.get())->hasPayload();
}

/** Return whether a C function pointer can accept this parameter type. */
inline bool isCallbackParameter(const TypePtr& type) {
  return type && !type->isVoid() &&
         (type->isPrimitive() || type->isRawPointer() ||
          type->isReference() || isCStyleEnum(type));
}

/** Return whether a C function pointer can return this type. */
inline bool isCallbackReturn(const TypePtr& type) {
  return type &&
         (type->isPrimitive() || type->isRawPointer() || isCStyleEnum(type));
}

/** Return whether a function type is a non-throwing C function pointer. */
inline bool isFunctionPointer(const TypePtr& type) {
  const auto* function = tryGetType<FunctionType>(type);
  if (!function || function->canThrow() ||
      !isCallbackReturn(function->getReturnType())) {
    return false;
  }
  for (const auto& param : function->getParamTypes()) {
    if (!isCallbackParameter(param)) return false;
  }
  return true;
}

/** Return whether a value has a supported C global or parameter layout. */
inline bool isValue(const TypePtr& type) {
  return type && !type->isVoid() &&
         (type->isPrimitive() || type->isRawPointer() ||
          type->isReference() || type->isClass() || isCStyleEnum(type) ||
          isFunctionPointer(type));
}

/** Return whether a value has a supported C return layout. */
inline bool isReturn(const TypePtr& type) {
  return type && (type->isPrimitive() || type->isRawPointer() ||
                  type->isClass() || isCStyleEnum(type));
}

}  // namespace sun::c_abi
