// src/codegen/intrinsics/atomic.cpp - Atomic intrinsic function codegen
//
// This file contains codegen for typed integer atomics, memory fences, and
// operating-system wait-on-address synchronization. The futex intrinsics use
// Linux system calls.

#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics_generator.h"
#include "codegen/intrinsics/thread_utils.h"
#include "support/error.h"

using namespace llvm;

// -------------------------------------------------------------------
// Atomic integer and fence intrinsics
// -------------------------------------------------------------------

/**
 * Emits compare-and-exchange for one supported atomic integer width.
 */
Value* IntrinsicsGenerator::codegenAtomicCmpxchgIntrinsic(
    const CallExprAST& expr, unsigned bitWidth, bool signedValues,
    const char* name) {
  const auto& args = expr.getArgs();
  if (args.size() != 3) {
    logAndThrowError(std::string(name) +
                     " expects 3 arguments: (ptr, expected, desired)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  llvm::Value* expected = codegen(*args[1]);
  llvm::Value* desired = codegen(*args[2]);
  if (!ptr || !expected || !desired) return nullptr;

  auto* intTy = llvm::Type::getIntNTy(ctx.getContext(), bitWidth);
  expected =
      signedValues
          ? ctx.builder->CreateSExtOrTrunc(expected, intTy, "cmpxchg.expected")
          : ctx.builder->CreateZExtOrTrunc(expected, intTy, "cmpxchg.expected");
  desired =
      signedValues
          ? ctx.builder->CreateSExtOrTrunc(desired, intTy, "cmpxchg.desired")
          : ctx.builder->CreateZExtOrTrunc(desired, intTy, "cmpxchg.desired");

  llvm::Value* result = ctx.builder->CreateAtomicCmpXchg(
      ptr, expected, desired, llvm::MaybeAlign(),
      llvm::AtomicOrdering::AcquireRelease, llvm::AtomicOrdering::Acquire);
  return ctx.builder->CreateExtractValue(result, 0, "cmpxchg.old");
}

/**
 * Emits a release store for one supported atomic integer width.
 */
Value* IntrinsicsGenerator::codegenAtomicStoreIntrinsic(const CallExprAST& expr,
                                                        unsigned bitWidth,
                                                        bool signedValues,
                                                        const char* name) {
  const auto& args = expr.getArgs();
  if (args.size() != 2) {
    logAndThrowError(std::string(name) + " expects 2 arguments: (ptr, value)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  llvm::Value* value = codegen(*args[1]);
  if (!ptr || !value) return nullptr;

  auto* intTy = llvm::Type::getIntNTy(ctx.getContext(), bitWidth);
  value =
      signedValues
          ? ctx.builder->CreateSExtOrTrunc(value, intTy, "atomic.store.val")
          : ctx.builder->CreateZExtOrTrunc(value, intTy, "atomic.store.val");

  llvm::StoreInst* store = ctx.builder->CreateStore(value, ptr);
  store->setAtomic(llvm::AtomicOrdering::Release);
  return llvm::ConstantInt::get(intTy, 0);
}

/**
 * Emits an acquire load for one supported atomic integer width.
 */
Value* IntrinsicsGenerator::codegenAtomicLoadIntrinsic(const CallExprAST& expr,
                                                       unsigned bitWidth,
                                                       const char* name) {
  const auto& args = expr.getArgs();
  if (args.size() != 1) {
    logAndThrowError(std::string(name) + " expects 1 argument: (ptr)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  if (!ptr) return nullptr;

  auto* intTy = llvm::Type::getIntNTy(ctx.getContext(), bitWidth);
  llvm::LoadInst* load = ctx.builder->CreateLoad(intTy, ptr, "atomic.load.val");
  load->setAtomic(llvm::AtomicOrdering::Acquire);
  return load;
}

/**
 * Emits fetch-add or fetch-sub for one supported atomic integer width.
 */
Value* IntrinsicsGenerator::codegenAtomicFetchOpIntrinsic(
    const CallExprAST& expr, unsigned bitWidth, bool signedValues,
    bool subtract, const char* name) {
  const auto& args = expr.getArgs();
  if (args.size() != 2) {
    logAndThrowError(std::string(name) + " expects 2 arguments: (ptr, delta)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  llvm::Value* delta = codegen(*args[1]);
  if (!ptr || !delta) return nullptr;

  auto* intTy = llvm::Type::getIntNTy(ctx.getContext(), bitWidth);
  delta =
      signedValues
          ? ctx.builder->CreateSExtOrTrunc(delta, intTy, "atomic.rmw.delta")
          : ctx.builder->CreateZExtOrTrunc(delta, intTy, "atomic.rmw.delta");

  return ctx.builder->CreateAtomicRMW(
      subtract ? llvm::AtomicRMWInst::Sub : llvm::AtomicRMWInst::Add, ptr,
      delta, llvm::MaybeAlign(), llvm::AtomicOrdering::AcquireRelease);
}

/**
 * Emits an explicit acquire or release fence.
 */
Value* IntrinsicsGenerator::codegenAtomicFenceIntrinsic(const CallExprAST& expr,
                                                        bool acquire) {
  if (!expr.getArgs().empty()) {
    logAndThrowError(std::string(acquire ? "_atomic_fence_acquire"
                                         : "_atomic_fence_release") +
                     " expects no arguments");
    return nullptr;
  }
  ctx.builder->CreateFence(acquire ? llvm::AtomicOrdering::Acquire
                                   : llvm::AtomicOrdering::Release);
  return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
}

// -------------------------------------------------------------------
// Futex intrinsics: _futex_wait, _futex_wake
// -------------------------------------------------------------------

Value* IntrinsicsGenerator::codegenFutexWaitIntrinsic(const CallExprAST& expr) {
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

Value* IntrinsicsGenerator::codegenFutexWakeIntrinsic(const CallExprAST& expr) {
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
