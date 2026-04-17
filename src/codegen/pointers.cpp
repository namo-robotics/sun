// src/codegen/pointers.cpp - Pointer type codegen
//
// This file contains codegen for:
// - Pointer member access: raw_ptr<T>.get(), static_ptr<T>.*

#include "codegen_visitor.h"
#include "error.h"

using namespace llvm;

// -------------------------------------------------------------------
// Pointer member access: ptr<T>.get(), raw_ptr<T>.get(), static_ptr<T>.*
// Called from MemberAccessAST codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenStaticPtrMemberAccess(
    const MemberAccessAST& expr, sun::StaticPointerType* ptrType) {
  const std::string& memberName = expr.getMemberName();

  if (memberName == "length") {
    // static_ptr is a fat pointer: { ptr data, i64 length }
    Value* fatPtr = codegen(*expr.getObject());
    if (!fatPtr) return nullptr;
    return ctx.builder->CreateExtractValue(fatPtr, 1, "static_ptr.length");
  }

  if (memberName == "data") {
    Value* fatPtr = codegen(*expr.getObject());
    if (!fatPtr) return nullptr;
    return ctx.builder->CreateExtractValue(fatPtr, 0, "static_ptr.data");
  }

  if (memberName == "get") {
    // Dereference: static_ptr<T>.get() loads from data pointer
    Value* fatPtr = codegen(*expr.getObject());
    if (!fatPtr) return nullptr;
    Value* dataPtr =
        ctx.builder->CreateExtractValue(fatPtr, 0, "static_ptr.data");
    llvm::Type* pointeeLLVMType =
        ptrType->getPointeeType()->toLLVMType(ctx.getContext());
    return ctx.builder->CreateLoad(pointeeLLVMType, dataPtr, "static_ptr.get");
  }

  logAndThrowError("static_ptr has no member '" + memberName +
                   "'; available: 'length', 'data', 'get'");
  return nullptr;
}

Value* CodegenVisitor::codegenRawPtrMemberAccess(const MemberAccessAST& expr,
                                                 sun::RawPointerType* ptrType) {
  const std::string& memberName = expr.getMemberName();

  if (memberName == "get") {
    Value* ptrVal = codegen(*expr.getObject());
    if (!ptrVal) return nullptr;
    llvm::Type* pointeeLLVMType =
        ptrType->getPointeeType()->toLLVMType(ctx.getContext());
    return ctx.builder->CreateLoad(pointeeLLVMType, ptrVal, "raw_ptr.get");
  }

  // raw_ptr only has 'get' member (unless pointing to a class - handled
  // elsewhere)
  if (!ptrType->getPointeeType()->isClass()) {
    logAndThrowError("raw_ptr has no member '" + memberName +
                     "'; available: 'get'");
  }
  return nullptr;  // Fall through to class member access
}
