/** Emits scalar conversions and checked integer arithmetic. */

#include "codegen/support/scalar_ops.h"

using sun::types::TypePtr;

/** Emits scalar conversions and arithmetic shared by code generators. */
namespace sun::codegen::support {

/** Extends an integer to the requested width using its signedness. */
llvm::Value* extendInt(llvm::IRBuilder<>& builder, llvm::Value* value,
                       llvm::Type* destTy, const TypePtr& sourceType) {
  auto srcType = sun::types::unwrapRef(sourceType);
  return srcType && srcType->isUnsigned()
             ? builder.CreateZExt(value, destTy, "widen")
             : builder.CreateSExt(value, destTy, "widen");
}

/** Keeps arithmetic control-flow helpers local to this file. */
namespace {
/** Emits a terminating error callback when the arithmetic predicate is true. */
void throwIf(llvm::IRBuilder<>& builder, llvm::Value* invalid, int code,
             llvm::function_ref<void(int)> throwError) {
  if (auto* constant = llvm::dyn_cast<llvm::ConstantInt>(invalid))
    if (constant->isZero()) return;
  auto* function = builder.GetInsertBlock()->getParent();
  auto* failed = llvm::BasicBlock::Create(builder.getContext(),
                                          "arithmetic.throw", function);
  auto* valid =
      llvm::BasicBlock::Create(builder.getContext(), "arithmetic.ok", function);
  builder.CreateCondBr(invalid, failed, valid);
  builder.SetInsertPoint(failed);
  throwError(code);
  builder.SetInsertPoint(valid);
}
}  // namespace

llvm::Value* createIntDivRem(llvm::IRBuilder<>& builder, llvm::Value* L,
                             llvm::Value* R, bool isModulo, bool isUnsigned,
                             llvm::function_ref<void(int)> throwError) {
  throwIf(builder,
          builder.CreateICmpEQ(R, llvm::ConstantInt::get(R->getType(), 0)), 4,
          throwError);
  auto* divisor = llvm::dyn_cast<llvm::ConstantInt>(R);
  if (!isUnsigned && (!divisor || divisor->isMinusOne())) {
    auto* minimum = llvm::ConstantInt::get(
        builder.getContext(),
        llvm::APInt::getSignedMinValue(L->getType()->getIntegerBitWidth()));
    auto* minusOne = llvm::ConstantInt::getSigned(R->getType(), -1);
    throwIf(builder,
            builder.CreateAnd(builder.CreateICmpEQ(L, minimum),
                              builder.CreateICmpEQ(R, minusOne)),
            5, throwError);
  }
  if (isModulo) {
    return isUnsigned ? builder.CreateURem(L, R, "modtmp")
                      : builder.CreateSRem(L, R, "modtmp");
  }
  return isUnsigned ? builder.CreateUDiv(L, R, "divtmp")
                    : builder.CreateSDiv(L, R, "divtmp");
}

llvm::Value* createIntShift(llvm::IRBuilder<>& builder, llvm::Value* L,
                            llvm::Value* R, bool isRight, bool isUnsigned) {
  auto* mask = llvm::ConstantInt::get(R->getType(),
                                      L->getType()->getIntegerBitWidth() - 1);
  R = builder.CreateAnd(R, mask, "shift.count");
  if (!isRight) return builder.CreateShl(L, R, "shltmp");
  return isUnsigned ? builder.CreateLShr(L, R, "shrtmp")
                    : builder.CreateAShr(L, R, "shrtmp");
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
