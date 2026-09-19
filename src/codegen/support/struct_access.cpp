// struct_access.cpp — Reading and writing class fields (see struct_access.h)

#include "codegen/support/struct_access.h"

#include "semantic_analysis/packed_layout.h"

using sun::types::ClassType;

/** Provides shared diagnostics, source tracking, and compiler utilities. */
namespace sun::codegen::support {

/** Computes the LLVM address of a field in class storage. */
llvm::Value* fieldPtr(llvm::IRBuilder<>& builder, ClassType* classType,
                      llvm::Value* objectPtr,
                      const sun::types::ClassField& field,
                      const std::string& name) {
  llvm::StructType* structType = classType->getStructType(builder.getContext());
  return builder.CreateStructGEP(structType, objectPtr, field.index, name);
}

/** Returns the alignment available for a class field access. */
llvm::Align fieldAlign(const ClassType* owner, llvm::Type* fieldTy,
                       const llvm::DataLayout& dl) {
  return sun::semantic_analysis::fieldAlign(owner, fieldTy, dl);
}

/** Returns the alignment available when accessing an assignable expression. */
llvm::Align lvalueAlign(const sun::ast::ExprAST& target, llvm::Type* slotTy,
                        const llvm::DataLayout& dl) {
  return sun::semantic_analysis::lvalueAlign(target, slotTy, dl);
}

/** Stores a generated value with the alignment required by its storage. */
void storeIntoSlot(llvm::IRBuilder<>& builder, const llvm::DataLayout& dl,
                   llvm::Value* dest, llvm::Value* value,
                   const sun::types::TypePtr& slotType,
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
          sun::codegen::support::tryGetType<sun::types::ArrayType>(slotType)) {
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
