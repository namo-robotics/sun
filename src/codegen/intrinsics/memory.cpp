// src/codegen/intrinsics/memory.cpp - Memory intrinsic function codegen
//
// This file contains codegen for memory intrinsics:
// - _load_i64, _store_i64 (raw memory access)
// - _malloc, _free (heap allocation via libc)

#include "codegen_visitor.h"
#include "error.h"

using namespace llvm;

// -------------------------------------------------------------------
// Non-generic memory intrinsics: _load_i64, _store_i64, _malloc, _free
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenLoadI64Intrinsic(const CallExprAST& expr) {
  // _load_i64(ptr, index) - load i64 from ptr at element offset index
  const auto& args = expr.getArgs();
  if (args.size() != 2) {
    logAndThrowError("_load_i64 expects 2 arguments: (ptr, index)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  llvm::Value* index = codegen(*args[1]);
  if (!ptr || !index) return nullptr;

  auto* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  llvm::Value* elemPtr = ctx.builder->CreateGEP(i64Ty, ptr, index, "i64.ptr");
  return ctx.builder->CreateLoad(i64Ty, elemPtr, "i64.val");
}

Value* CodegenVisitor::codegenStoreI64Intrinsic(const CallExprAST& expr) {
  // _store_i64(ptr, index, value) - store i64 to ptr at element offset index
  const auto& args = expr.getArgs();
  if (args.size() != 3) {
    logAndThrowError("_store_i64 expects 3 arguments: (ptr, index, value)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  llvm::Value* index = codegen(*args[1]);
  llvm::Value* value = codegen(*args[2]);
  if (!ptr || !index || !value) return nullptr;

  auto* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  llvm::Value* elemPtr = ctx.builder->CreateGEP(i64Ty, ptr, index, "i64.ptr");
  ctx.builder->CreateStore(value, elemPtr);
  return value;
}

Value* CodegenVisitor::codegenMallocIntrinsic(const CallExprAST& expr) {
  // _malloc(size) - allocate size bytes, returns raw_ptr<i8>
  const auto& args = expr.getArgs();
  if (args.size() != 1) {
    logAndThrowError("_malloc expects 1 argument: (size)");
    return nullptr;
  }

  llvm::Value* size = codegen(*args[0]);
  if (!size) return nullptr;

  // Get or declare libc malloc: void* malloc(size_t)
  auto* i8PtrTy = llvm::PointerType::getUnqual(ctx.getContext());
  auto* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  llvm::FunctionType* mallocType =
      llvm::FunctionType::get(i8PtrTy, {i64Ty}, false);
  llvm::FunctionCallee mallocFunc =
      module->getOrInsertFunction("malloc", mallocType);

  return ctx.builder->CreateCall(mallocFunc, {size}, "malloc.result");
}

Value* CodegenVisitor::codegenFreeIntrinsic(const CallExprAST& expr) {
  // _free(ptr) - free previously allocated memory
  const auto& args = expr.getArgs();
  if (args.size() != 1) {
    logAndThrowError("_free expects 1 argument: (ptr)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  if (!ptr) return nullptr;

  // Get or declare libc free: void free(void*)
  auto* i8PtrTy = llvm::PointerType::getUnqual(ctx.getContext());
  auto* voidTy = llvm::Type::getVoidTy(ctx.getContext());
  llvm::FunctionType* freeType =
      llvm::FunctionType::get(voidTy, {i8PtrTy}, false);
  llvm::FunctionCallee freeFunc = module->getOrInsertFunction("free", freeType);

  ctx.builder->CreateCall(freeFunc, {ptr});
  return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
}
