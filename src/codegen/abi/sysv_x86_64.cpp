// abi/sysv_x86_64.cpp — System V AMD64 argument classification.
// See abi/sysv_x86_64.h.

#include "codegen/abi/sysv_x86_64.h"

#include <algorithm>

namespace sun::abi::sysv {

namespace {

// psABI classes, restricted to the ones reachable from Sun's type system.
// X87/COMPLEX_X87 would require long double, which Sun does not have.
enum class EightbyteClass { NoClass, SSE, Integer, Memory };

// psABI merge rule, simplified to the classes above: Memory wins over
// Integer, which wins over SSE, which wins over NoClass.
EightbyteClass merge(EightbyteClass a, EightbyteClass b) {
  if (a == b) return a;
  if (a == EightbyteClass::NoClass) return b;
  if (b == EightbyteClass::NoClass) return a;
  if (a == EightbyteClass::Memory || b == EightbyteClass::Memory)
    return EightbyteClass::Memory;
  if (a == EightbyteClass::Integer || b == EightbyteClass::Integer)
    return EightbyteClass::Integer;
  return EightbyteClass::SSE;
}

struct EightbyteInfo {
  EightbyteClass cls = EightbyteClass::NoClass;
  // Highest byte offset used within this eightbyte. Determines the width of
  // the coerced scalar: `struct { int; long; }` leaves the first eightbyte
  // half empty, and it coerces to i32 rather than i64.
  uint64_t usedBytes = 0;
  // Whether every SSE leaf here is a 4-byte float. A full SSE eightbyte made
  // of two floats is passed as <2 x float>, not double.
  bool allFloat32 = true;
};

// Walk the leaves of an aggregate, folding each into the eightbyte it lands
// in. Returns false if the type cannot be classified in registers (a field
// straddling an eightbyte boundary, which packed layouts can produce).
bool classifyLeaves(llvm::Type* type, uint64_t offset,
                    const llvm::DataLayout& dl, EightbyteInfo eightbytes[2]) {
  if (auto* structTy = llvm::dyn_cast<llvm::StructType>(type)) {
    const llvm::StructLayout* layout = dl.getStructLayout(structTy);
    for (unsigned i = 0; i < structTy->getNumElements(); ++i) {
      if (!classifyLeaves(structTy->getElementType(i),
                          offset + layout->getElementOffset(i), dl,
                          eightbytes)) {
        return false;
      }
    }
    return true;
  }

  if (auto* arrayTy = llvm::dyn_cast<llvm::ArrayType>(type)) {
    llvm::Type* elem = arrayTy->getElementType();
    uint64_t stride = dl.getTypeAllocSize(elem);
    for (uint64_t i = 0; i < arrayTy->getNumElements(); ++i) {
      if (!classifyLeaves(elem, offset + i * stride, dl, eightbytes)) {
        return false;
      }
    }
    return true;
  }

  // Scalar leaf.
  uint64_t size = dl.getTypeStoreSize(type);
  if (size == 0) return true;

  uint64_t first = offset / 8;
  uint64_t last = (offset + size - 1) / 8;
  if (last > 1) return false;  // beyond the two eightbytes we handle
  if (first != last) return false;  // straddles a boundary -> MEMORY

  // Only float and double land in SSE registers; everything else Sun can
  // express in a struct (integers, bools, pointers) is INTEGER.
  EightbyteClass cls = type->isFloatingPointTy() ? EightbyteClass::SSE
                                                 : EightbyteClass::Integer;
  if (cls == EightbyteClass::SSE && !type->isFloatTy()) {
    eightbytes[first].allFloat32 = false;
  }
  eightbytes[first].cls = merge(eightbytes[first].cls, cls);
  eightbytes[first].usedBytes =
      std::max(eightbytes[first].usedBytes, (offset + size) - first * 8);
  return true;
}

// The scalar an eightbyte is coerced to.
//
// Integers take the exact width of the bytes actually occupied, so a
// three-byte eightbyte is i24 rather than i32 — this is what clang emits, and
// matching it exactly avoids relying on the callee ignoring stray high bits.
// SSE eightbytes are `float` when half-full, `<2 x float>` when two floats
// share them, and `double` otherwise.
llvm::Type* pieceType(const EightbyteInfo& eb, llvm::LLVMContext& ctx) {
  if (eb.cls == EightbyteClass::SSE) {
    llvm::Type* floatTy = llvm::Type::getFloatTy(ctx);
    if (eb.usedBytes <= 4) return floatTy;
    if (eb.allFloat32) {
      return llvm::FixedVectorType::get(floatTy, 2);
    }
    return llvm::Type::getDoubleTy(ctx);
  }
  return llvm::IntegerType::get(ctx,
                                static_cast<unsigned>(eb.usedBytes * 8));
}

bool isAggregate(llvm::Type* type) {
  return type && (type->isStructTy() || type->isArrayTy());
}

ArgLowering lowerAggregate(llvm::Type* type, const llvm::DataLayout& dl) {
  ArgLowering result;
  result.type = type;

  uint64_t size = dl.getTypeAllocSize(type);
  uint64_t align = dl.getABITypeAlign(type).value();

  // A zero-sized aggregate occupies no argument slots at all.
  if (size == 0) {
    result.kind = ArgKind::Coerced;
    return result;
  }

  // Larger than two eightbytes always goes through memory.
  if (size > 16) {
    result.kind = ArgKind::Indirect;
    result.align = align;
    return result;
  }

  EightbyteInfo eightbytes[2];
  if (!classifyLeaves(type, 0, dl, eightbytes)) {
    result.kind = ArgKind::Indirect;
    result.align = align;
    return result;
  }

  if (eightbytes[0].cls == EightbyteClass::Memory ||
      eightbytes[1].cls == EightbyteClass::Memory) {
    result.kind = ArgKind::Indirect;
    result.align = align;
    return result;
  }

  result.kind = ArgKind::Coerced;
  llvm::LLVMContext& ctx = type->getContext();
  unsigned count = size > 8 ? 2 : 1;
  for (unsigned i = 0; i < count; ++i) {
    // A trailing eightbyte with no fields (possible with odd padding) still
    // has to carry its bytes; treat it as a full integer eightbyte.
    EightbyteInfo eb = eightbytes[i];
    if (eb.cls == EightbyteClass::NoClass) {
      eb.cls = EightbyteClass::Integer;
      eb.usedBytes = std::min<uint64_t>(8, size - i * 8);
    }
    result.pieces.push_back(pieceType(eb, ctx));
    result.pieceOffsets.push_back(i * 8);
  }
  return result;
}

}  // namespace

ArgLowering lowerArgument(llvm::Type* type, const llvm::DataLayout& dl) {
  ArgLowering result;
  result.type = type;
  if (!isAggregate(type)) return result;  // Direct
  return lowerAggregate(type, dl);
}

ArgLowering lowerReturn(llvm::Type* type, const llvm::DataLayout& dl) {
  ArgLowering result;
  result.type = type;
  if (!type || type->isVoidTy() || !isAggregate(type)) return result;  // Direct
  return lowerAggregate(type, dl);
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

}  // namespace sun::abi::sysv
