// abi/aapcs64.cpp — AArch64 AAPCS64 (ELF) argument classification.
// See abi/aapcs64.h.

#include "abi/aapcs64.h"

namespace sun::abi::aapcs64 {

namespace {

bool isAggregate(llvm::Type* type) {
  return type && (type->isStructTy() || type->isArrayTy());
}

// Walk the leaves of an aggregate collecting HFA evidence: every leaf must
// be the same floating-point type. Sun's only FP leaves are float and double
// (no fp16, no vectors, no long double). Returns false as soon as the type
// cannot be an HFA; `count` may exceed 4, which the caller rejects.
bool collectHFALeaves(llvm::Type* type, llvm::Type*& base, unsigned& count) {
  if (auto* structTy = llvm::dyn_cast<llvm::StructType>(type)) {
    for (unsigned i = 0; i < structTy->getNumElements(); ++i) {
      if (!collectHFALeaves(structTy->getElementType(i), base, count)) {
        return false;
      }
    }
    return true;
  }

  if (auto* arrayTy = llvm::dyn_cast<llvm::ArrayType>(type)) {
    for (uint64_t i = 0; i < arrayTy->getNumElements(); ++i) {
      if (!collectHFALeaves(arrayTy->getElementType(), base, count)) {
        return false;
      }
    }
    return true;
  }

  if (!type->isFloatTy() && !type->isDoubleTy()) return false;
  if (!base) base = type;
  if (base != type) return false;
  ++count;
  return true;
}

// A Homogeneous Floating-point Aggregate: 1-4 members, all the same FP type
// after flattening nested structs and arrays. HFAs travel in FP registers
// regardless of total size (four doubles is 32 bytes and still not memory).
bool isHFA(llvm::Type* type, llvm::Type*& base, unsigned& count) {
  base = nullptr;
  count = 0;
  if (!collectHFALeaves(type, base, count)) return false;
  return count >= 1 && count <= 4;
}

ArgLowering lowerAggregate(llvm::Type* type, const llvm::DataLayout& dl,
                           bool isReturn) {
  ArgLowering result;
  result.type = type;

  uint64_t size = dl.getTypeAllocSize(type);

  // A zero-sized aggregate occupies no argument slots at all.
  if (size == 0) {
    result.kind = ArgKind::Coerced;
    return result;
  }

  llvm::Type* base = nullptr;
  unsigned count = 0;
  if (isHFA(type, base, count)) {
    // Returned HFAs keep the literal struct type — clang emits
    // `declare %struct.H @f()` — and LLVM assigns the FP registers itself.
    if (isReturn) return result;  // Direct
    // As an argument the HFA coerces to one [N x float/double] value, with
    // alignstack(8) so a spill to the stack stays 8-byte aligned.
    result.kind = ArgKind::Coerced;
    result.pieces.push_back(llvm::ArrayType::get(base, count));
    result.pieceOffsets.push_back(0);
    result.stackAlign = 8;
    return result;
  }

  // Non-HFA beyond two registers: a pointer to a caller-made copy. The
  // pointer itself is the argument (no byval — that is x86-64's spelling).
  if (size > 16) {
    result.kind = ArgKind::Indirect;
    result.align = dl.getABITypeAlign(type).value();
    result.indirectByval = false;
    return result;
  }

  // In registers: one i64 or an [2 x i64] pair. Arguments round up to whole
  // registers; returns use the exact width (a 3-byte struct returns i24).
  // Both match clang for aarch64-linux-gnu.
  result.kind = ArgKind::Coerced;
  llvm::LLVMContext& ctx = type->getContext();
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
  if (size <= 8) {
    result.pieces.push_back(
        isReturn ? llvm::IntegerType::get(ctx, static_cast<unsigned>(size * 8))
                 : i64Ty);
  } else {
    result.pieces.push_back(llvm::ArrayType::get(i64Ty, 2));
  }
  result.pieceOffsets.push_back(0);
  return result;
}

}  // namespace

ArgLowering lowerArgument(llvm::Type* type, const llvm::DataLayout& dl) {
  ArgLowering result;
  result.type = type;
  if (!isAggregate(type)) return result;  // Direct
  return lowerAggregate(type, dl, /*isReturn=*/false);
}

ArgLowering lowerReturn(llvm::Type* type, const llvm::DataLayout& dl) {
  ArgLowering result;
  result.type = type;
  if (!type || type->isVoidTy() || !isAggregate(type)) return result;  // Direct
  return lowerAggregate(type, dl, /*isReturn=*/true);
}

SignatureLowering lowerCSignature(llvm::Type* returnType,
                                  llvm::ArrayRef<llvm::Type*> paramTypes,
                                  const llvm::DataLayout& dl) {
  SignatureLowering lowering;
  lowering.ret = lowerReturn(returnType, dl);
  lowering.params.reserve(paramTypes.size());
  for (llvm::Type* p : paramTypes) {
    lowering.params.push_back(lowerArgument(p, dl));
  }
  return lowering;
}

}  // namespace sun::abi::aapcs64
