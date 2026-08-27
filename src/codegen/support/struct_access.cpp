// struct_access.cpp — Reading and writing class fields (see struct_access.h)

#include "codegen/support/struct_access.h"

#include "semantic_analysis/packed_layout.h"

namespace sun::codegen::layout {

llvm::Value* fieldPtr(llvm::IRBuilder<>& builder, sun::ClassType* classType,
                      llvm::Value* objectPtr, const sun::ClassField& field,
                      const std::string& name) {
  llvm::StructType* structType = classType->getStructType(builder.getContext());
  return builder.CreateStructGEP(structType, objectPtr, field.index, name);
}

llvm::Align fieldAlign(const sun::ClassType* owner, llvm::Type* fieldTy,
                       const llvm::DataLayout& dl) {
  return sun::packed::fieldAlign(owner, fieldTy, dl);
}

llvm::Align lvalueAlign(const ExprAST& target, llvm::Type* slotTy,
                        const llvm::DataLayout& dl) {
  return sun::packed::lvalueAlign(target, slotTy, dl);
}

void storeIntoSlot(llvm::IRBuilder<>& builder, const llvm::DataLayout& dl,
                   llvm::Value* dest, llvm::Value* value,
                   const sun::TypePtr& slotType, const sun::ClassType* owner) {
  if (slotType && slotType->isClass() && value->getType()->isPointerTy()) {
    const auto* classType = static_cast<const sun::ClassType*>(slotType.get());
    llvm::StructType* structTy = classType->getStructType(builder.getContext());
    llvm::Align align = fieldAlign(owner, structTy, dl);
    builder.CreateMemCpy(dest, align, value, align,
                         dl.getTypeAllocSize(structTy));
    return;
  }
  builder.CreateAlignedStore(value, dest,
                             fieldAlign(owner, value->getType(), dl));
}

}  // namespace sun::codegen::layout
