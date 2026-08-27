// scalar_ops.cpp — Scalar conversions that hold no codegen state
// (see scalar_ops.h)

#include "codegen/support/scalar_ops.h"

namespace sun::codegen::ops {

llvm::Value* extendInt(llvm::IRBuilder<>& builder, llvm::Value* value,
                       llvm::Type* destTy, const sun::TypePtr& sourceType) {
  auto srcType = sun::unwrapRef(sourceType);
  return srcType && srcType->isUnsigned()
             ? builder.CreateZExt(value, destTy, "widen")
             : builder.CreateSExt(value, destTy, "widen");
}

llvm::Value* createIntDivRem(llvm::IRBuilder<>& builder, llvm::Value* L,
                             llvm::Value* R, bool isModulo, bool isUnsigned) {
  if (isModulo) {
    return isUnsigned ? builder.CreateURem(L, R, "modtmp")
                      : builder.CreateSRem(L, R, "modtmp");
  }
  return isUnsigned ? builder.CreateUDiv(L, R, "divtmp")
                    : builder.CreateSDiv(L, R, "divtmp");
}

llvm::Value* widenNumericIfNeeded(llvm::IRBuilder<>& builder,
                                  LLVMTypeResolver& types, llvm::Value* argVal,
                                  const sun::TypePtr& paramType,
                                  const sun::TypePtr& sourceType) {
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

}  // namespace sun::codegen::ops
