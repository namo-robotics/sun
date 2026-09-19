// scalar_ops.cpp — Scalar conversions that hold no codegen state
// (see scalar_ops.h)

#include "codegen/support/scalar_ops.h"

using sun::types::TypePtr;

/** Provides shared diagnostics, source tracking, and compiler utilities. */
namespace sun::codegen::support {

/** Extends an integer to the requested width using its signedness. */
llvm::Value* extendInt(llvm::IRBuilder<>& builder, llvm::Value* value,
                       llvm::Type* destTy, const TypePtr& sourceType) {
  auto srcType = sun::types::unwrapRef(sourceType);
  return srcType && srcType->isUnsigned()
             ? builder.CreateZExt(value, destTy, "widen")
             : builder.CreateSExt(value, destTy, "widen");
}

/** Emits integer division or remainder with the required error checks. */
llvm::Value* createIntDivRem(llvm::IRBuilder<>& builder, llvm::Value* L,
                             llvm::Value* R, bool isModulo, bool isUnsigned) {
  if (isModulo) {
    return isUnsigned ? builder.CreateURem(L, R, "modtmp")
                      : builder.CreateSRem(L, R, "modtmp");
  }
  return isUnsigned ? builder.CreateUDiv(L, R, "divtmp")
                    : builder.CreateSDiv(L, R, "divtmp");
}

/** Widens numeric operands to a compatible LLVM representation when needed. */
llvm::Value* widenNumericIfNeeded(llvm::IRBuilder<>& builder,
                                  sun::codegen::LLVMTypeResolver& types,
                                  llvm::Value* argVal, const TypePtr& paramType,
                                  const TypePtr& sourceType) {
  if (!paramType) {
    return argVal;
  }

  llvm::Type* expectedType = types.resolve(paramType);

  // Integer widening: smaller int -> larger int
  if (argVal->getType()->isIntegerTy() && expectedType->isIntegerTy()) {
    unsigned argBits = argVal->getType()->getIntegerBitWidth();
    unsigned paramBits = expectedType->getIntegerBitWidth();
    if (argBits < paramBits) {
      return extendInt(builder, argVal, expectedType, sourceType);
    }
  }
  // Float widening: f32 -> f64
  else if (argVal->getType()->isFloatTy() && expectedType->isDoubleTy()) {
    return builder.CreateFPExt(argVal, expectedType, "widen");
  }

  return argVal;
}

}  // namespace sun::codegen::support
