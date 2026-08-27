#pragma once

// scalar_ops.h — Scalar conversions that hold no codegen state
//
// Widening, signedness and the small coercions a call boundary needs. They
// take an IR builder and the Sun types involved, nothing else, so any pass
// that emits IR can reach the same answers. The rules that need to know what
// function is being emitted — checked division, for one — stay with the
// component that knows it.

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include "codegen/llvm_type_resolver.h"
#include "semantic_analysis/types.h"

namespace sun::codegen::ops {

/**
 * Widens an integer to destTy. The source expression's Sun type decides zero-
 * versus sign-extension, and this is the single place that owns that rule.
 */
llvm::Value* extendInt(llvm::IRBuilder<>& builder, llvm::Value* value,
                       llvm::Type* destTy, const sun::TypePtr& sourceType);

/**
 * Integer division or remainder with the given signedness.
 */
llvm::Value* createIntDivRem(llvm::IRBuilder<>& builder, llvm::Value* L,
                             llvm::Value* R, bool isModulo, bool isUnsigned);

/**
 * Widens an integer or float argument to what the parameter expects
 * (i32 to i64, f32 to f64). Returns the value unchanged if no widening is
 * needed.
 */
llvm::Value* widenNumericIfNeeded(llvm::IRBuilder<>& builder,
                                  LLVMTypeResolver& types, llvm::Value* argVal,
                                  const sun::TypePtr& paramType,
                                  const sun::TypePtr& sourceType);

}  // namespace sun::codegen::ops
