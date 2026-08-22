// abi/c_abi.cpp — C ABI target dispatch and shared lowering helpers.

#include "codegen/abi/c_abi.h"

#include "codegen/abi/aapcs64.h"
#include "codegen/abi/sysv_x86_64.h"
#include "support/error.h"

namespace sun::abi {

SignatureLowering lowerCSignature(const llvm::Triple& triple,
                                  llvm::Type* returnType,
                                  llvm::ArrayRef<llvm::Type*> paramTypes,
                                  const llvm::DataLayout& dl) {
  switch (triple.getArch()) {
    case llvm::Triple::x86_64:
      return sysv::lowerCSignature(returnType, paramTypes, dl);
    case llvm::Triple::aarch64:
      // Darwin deviates from base AAPCS64 (varargs on the stack, packed
      // arguments); only the ELF rules are implemented.
      if (triple.isOSDarwin()) break;
      return aapcs64::lowerCSignature(returnType, paramTypes, dl);
    default:
      break;
  }
  logAndThrowError("no C ABI rules for target '" + triple.str() +
                   "'; extern \"C\" supports x86_64 and aarch64 (ELF) only");
}

llvm::FunctionType* buildLoweredFunctionType(const SignatureLowering& lowering,
                                             llvm::LLVMContext& ctx,
                                             bool isVarArg) {
  std::vector<llvm::Type*> params;
  llvm::Type* retTy = nullptr;

  if (lowering.ret.isIndirect()) {
    // sret: the caller's buffer arrives as a hidden leading pointer and the
    // function itself returns nothing.
    params.push_back(llvm::PointerType::getUnqual(ctx));
    retTy = llvm::Type::getVoidTy(ctx);
  } else if (lowering.ret.isCoerced()) {
    const auto& pieces = lowering.ret.pieces;
    if (pieces.empty()) {
      retTy = llvm::Type::getVoidTy(ctx);
    } else if (pieces.size() == 1) {
      retTy = pieces[0];
    } else {
      retTy = llvm::StructType::get(ctx, {pieces[0], pieces[1]});
    }
  } else {
    retTy = lowering.ret.type ? lowering.ret.type : llvm::Type::getVoidTy(ctx);
  }

  for (const auto& p : lowering.params) {
    if (p.isIndirect()) {
      params.push_back(llvm::PointerType::getUnqual(ctx));
    } else if (p.isCoerced()) {
      for (llvm::Type* piece : p.pieces) params.push_back(piece);
    } else {
      params.push_back(p.type);
    }
  }

  return llvm::FunctionType::get(retTy, params, isVarArg);
}

}  // namespace sun::abi
