#pragma once

/** Provides scalar conversions and checked integer arithmetic emission. */

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include "codegen/llvm_type_resolver.h"
#include "types/types.h"

/** Emits scalar conversions and arithmetic shared by code generators. */
namespace sun::codegen::support {
using sun::types::TypePtr;

/**
 * Widens an integer to destTy. The source expression's Sun type decides zero-
 * versus sign-extension, and this is the single place that owns that rule.
 */
llvm::Value* extendInt(llvm::IRBuilder<>& builder, llvm::Value* value,
                       llvm::Type* destTy, const TypePtr& sourceType);

/**
 * Checks and emits integer division or remainder. The callback emits a
 * terminating throw for division by zero (code 4) or signed overflow (code 5).
 */
llvm::Value* createIntDivRem(llvm::IRBuilder<>& builder, llvm::Value* L,
                             llvm::Value* R, bool isModulo, bool isUnsigned,
                             llvm::function_ref<void(int)> throwError);

/** Emits a shift with the count masked to the widened operand bit width.
 */
llvm::Value* createIntShift(llvm::IRBuilder<>& builder, llvm::Value* L,
                            llvm::Value* R, bool isRight, bool isUnsigned);

/**
 * Widens an integer or float argument to what the parameter expects
 * (i32 to i64, f32 to f64). Returns the value unchanged if no widening is
 * needed.
 */
llvm::Value* widenNumericIfNeeded(llvm::IRBuilder<>& builder,
                                  sun::codegen::LLVMTypeResolver& types,
                                  llvm::Value* argVal, const TypePtr& paramType,
                                  const TypePtr& sourceType);

}  // namespace sun::codegen::support
