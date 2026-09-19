// struct_access.cpp — Reading and writing class fields (see struct_access.h)

#include "codegen/support/struct_access.h"

#include "semantic_analysis/packed_layout.h"

using sun::semantic_analysis::ClassType;

/** Provides shared diagnostics, source tracking, and compiler utilities. */
namespace sun::codegen::support {

llvm::Value* fieldPtr(llvm::IRBuilder<>& builder, ClassType* classType,
                      llvm::Value* objectPtr,
                      const sun::semantic_analysis::ClassField& field,
                      const std::string& name) {
  llvm::StructType* structType = classType->getStructType(builder.getContext());
  return builder.CreateStructGEP(structType, objectPtr, field.index, name);
}

llvm::Align fieldAlign(const ClassType* owner, llvm::Type* fieldTy,
                       const llvm::DataLayout& dl) {
  return sun::semantic_analysis::fieldAlign(owner, fieldTy, dl);
}

llvm::Align lvalueAlign(const sun::ast::ExprAST& target, llvm::Type* slotTy,
                        const llvm::DataLayout& dl) {
  return sun::semantic_analysis::lvalueAlign(target, slotTy, dl);
}

void storeIntoSlot(llvm::IRBuilder<>& builder, const llvm::DataLayout& dl,
                   llvm::Value* dest, llvm::Value* value,
                   const sun::semantic_analysis::TypePtr& slotType,
                   const ClassType* owner) {
  if (slotType && slotType->isClass() && value->getType()->isPointerTy()) {
    const auto* classType = static_cast<const ClassType*>(slotType.get());
    llvm::StructType* structTy = classType->getStructType(builder.getContext());
    llvm::Align align = sun::codegen::support::fieldAlign(owner, structTy, dl);
    builder.CreateMemCpy(dest, align, value, align,
                         dl.getTypeAllocSize(structTy));
    return;
  }
  // A sized array arrives as the address of its inline storage
  if (auto* arrayType =
          sun::codegen::support::tryGetType<sun::semantic_analysis::ArrayType>(
              slotType)) {
    if (!arrayType->isUnsized() && value->getType()->isPointerTy()) {
      llvm::Type* storageTy =
          arrayType->getDataStorageType(builder.getContext());
      llvm::Align align =
          sun::codegen::support::fieldAlign(owner, storageTy, dl);
      builder.CreateMemCpy(dest, align, value, align,
                           dl.getTypeAllocSize(storageTy));
      return;
    }
  }
  builder.CreateAlignedStore(
      value, dest,
      sun::codegen::support::fieldAlign(owner, value->getType(), dl));
}

}  // namespace sun::codegen::support
