// src/codegen/intrinsics/atomic.cpp - Atomic intrinsic function codegen
//
// This file contains codegen for atomic and synchronization intrinsics:
// - _atomic_cmpxchg_i32, _atomic_store_i32, _atomic_load_i32
// - _futex_wait, _futex_wake (Linux futex syscalls)

#include "codegen_visitor.h"
#include "error.h"
#include "thread_utils.h"

using namespace llvm;

// -------------------------------------------------------------------
// Atomic intrinsics: _atomic_cmpxchg_i32, _atomic_store_i32, _atomic_load_i32
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenAtomicCmpxchgI32Intrinsic(
    const CallExprAST& expr) {
  // _atomic_cmpxchg_i32(ptr, expected, desired) -> old_value
  // Returns the old value. Success if old == expected.
  const auto& args = expr.getArgs();
  if (args.size() != 3) {
    logAndThrowError(
        "_atomic_cmpxchg_i32 expects 3 arguments: (ptr, expected, desired)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  llvm::Value* expected = codegen(*args[1]);
  llvm::Value* desired = codegen(*args[2]);
  if (!ptr || !expected || !desired) return nullptr;

  auto* i32Ty = llvm::Type::getInt32Ty(ctx.getContext());

  // Ensure expected and desired are i32
  expected = ctx.builder->CreateTrunc(expected, i32Ty, "cmpxchg.expected");
  desired = ctx.builder->CreateTrunc(desired, i32Ty, "cmpxchg.desired");

  // LLVM cmpxchg: returns { old_value, success_flag }
  // Use acquire ordering for success (read-acquire), monotonic for failure
  llvm::Value* result = ctx.builder->CreateAtomicCmpXchg(
      ptr, expected, desired, llvm::MaybeAlign(),
      llvm::AtomicOrdering::AcquireRelease, llvm::AtomicOrdering::Acquire);

  // Extract just the old value (element 0)
  return ctx.builder->CreateExtractValue(result, 0, "cmpxchg.old");
}

Value* CodegenVisitor::codegenAtomicStoreI32Intrinsic(const CallExprAST& expr) {
  // _atomic_store_i32(ptr, value) -> void
  // Performs an atomic store with release ordering
  const auto& args = expr.getArgs();
  if (args.size() != 2) {
    logAndThrowError("_atomic_store_i32 expects 2 arguments: (ptr, value)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  llvm::Value* value = codegen(*args[1]);
  if (!ptr || !value) return nullptr;

  auto* i32Ty = llvm::Type::getInt32Ty(ctx.getContext());

  // Ensure value is i32
  value = ctx.builder->CreateTrunc(value, i32Ty, "atomic.store.val");

  // Create atomic store with release ordering
  llvm::StoreInst* store = ctx.builder->CreateStore(value, ptr);
  store->setAtomic(llvm::AtomicOrdering::Release);

  return llvm::ConstantInt::get(i32Ty, 0);
}

Value* CodegenVisitor::codegenAtomicLoadI32Intrinsic(const CallExprAST& expr) {
  // _atomic_load_i32(ptr) -> i32
  // Performs an atomic load with acquire ordering
  const auto& args = expr.getArgs();
  if (args.size() != 1) {
    logAndThrowError("_atomic_load_i32 expects 1 argument: (ptr)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  if (!ptr) return nullptr;

  auto* i32Ty = llvm::Type::getInt32Ty(ctx.getContext());

  // Create atomic load with acquire ordering
  llvm::LoadInst* load = ctx.builder->CreateLoad(i32Ty, ptr, "atomic.load.val");
  load->setAtomic(llvm::AtomicOrdering::Acquire);

  return load;
}

// -------------------------------------------------------------------
// Futex intrinsics: _futex_wait, _futex_wake
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenFutexWaitIntrinsic(const CallExprAST& expr) {
  // _futex_wait(ptr, expected) -> void
  // Blocks if *ptr == expected, until woken by _futex_wake
  const auto& args = expr.getArgs();
  if (args.size() != 2) {
    logAndThrowError("_futex_wait expects 2 arguments: (ptr, expected)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  llvm::Value* expected = codegen(*args[1]);
  if (!ptr || !expected) return nullptr;

  auto* i32Ty = llvm::Type::getInt32Ty(ctx.getContext());

  // Ensure expected is i32
  expected = ctx.builder->CreateTrunc(expected, i32Ty, "futex.expected");

  // Use ThreadUtils to emit the futex syscall
  ThreadUtils threadUtils(ctx, module);
  threadUtils.emitSyscallFutexWait(ptr, expected);

  return llvm::ConstantInt::get(i32Ty, 0);
}

Value* CodegenVisitor::codegenFutexWakeIntrinsic(const CallExprAST& expr) {
  // _futex_wake(ptr) -> void
  // Wakes one thread waiting on the futex at ptr
  const auto& args = expr.getArgs();
  if (args.size() != 1) {
    logAndThrowError("_futex_wake expects 1 argument: (ptr)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  if (!ptr) return nullptr;

  // Use ThreadUtils to emit the futex syscall
  ThreadUtils threadUtils(ctx, module);
  threadUtils.emitSyscallFutexWake(ptr);

  return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
}
