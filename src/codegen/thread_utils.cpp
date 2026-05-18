// thread_utils.cpp — Thread support utilities for code generation
//
// Implements raw syscall emitters and thread structure types
// for spawn/join semantics using raw Linux syscalls.
//
// No libc dependency - all syscalls are emitted as inline assembly.

#include "thread_utils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "struct_names.h"

using namespace llvm;

// Linux x86_64 syscall numbers
static constexpr int64_t SYS_MMAP = 9;
static constexpr int64_t SYS_MUNMAP = 11;
static constexpr int64_t SYS_CLONE = 56;
static constexpr int64_t SYS_EXIT = 60;
static constexpr int64_t SYS_FUTEX = 202;

// Futex operations
static constexpr int64_t FUTEX_WAIT = 0;
static constexpr int64_t FUTEX_WAKE = 1;

// mmap flags for anonymous private mapping
static constexpr int64_t MAP_PRIVATE = 0x02;
static constexpr int64_t MAP_ANONYMOUS = 0x20;
static constexpr int64_t PROT_READ = 0x1;
static constexpr int64_t PROT_WRITE = 0x2;

// -------------------------------------------------------------------
// Raw syscall emitters
// -------------------------------------------------------------------

Value* ThreadUtils::emitSyscallMmap(Value* size) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // mmap takes 6 arguments: addr, length, prot, flags, fd, offset
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty,
                                   i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);

  // syscall with 6 args uses: rax=sysno, rdi, rsi, rdx, r10, r8, r9
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_MMAP);
  Value* addr = ConstantInt::get(i64Ty, 0);                     // NULL
  Value* length = ctx.builder->CreateZExtOrTrunc(size, i64Ty);  // size
  Value* prot = ConstantInt::get(i64Ty, PROT_READ | PROT_WRITE);
  Value* flags = ConstantInt::get(i64Ty, MAP_PRIVATE | MAP_ANONYMOUS);
  Value* fd = ConstantInt::get(i64Ty, -1);     // No file
  Value* offset = ConstantInt::get(i64Ty, 0);  // No offset

  Value* result = ctx.builder->CreateCall(
      syscallAsm, {sysno, addr, length, prot, flags, fd, offset},
      "mmap_result");

  // Convert i64 result to pointer
  return ctx.builder->CreateIntToPtr(result, ptrTy, "mmap_ptr");
}

void ThreadUtils::emitSyscallMunmap(Value* addr, Value* size) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);

  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_MUNMAP);
  Value* addrInt = ctx.builder->CreatePtrToInt(addr, i64Ty);
  Value* length = ctx.builder->CreateZExtOrTrunc(size, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  ctx.builder->CreateCall(syscallAsm, {sysno, addrInt, length, zero});
}

Value* ThreadUtils::emitSyscallClone(Value* flags, Value* stackTop,
                                     Value* parentTidPtr, Value* childTidPtr) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // clone(flags, stack, parent_tid, child_tid, tls)
  // Note: tls is not used for basic threads
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);

  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_CLONE);
  Value* flagsVal = ctx.builder->CreateZExtOrTrunc(flags, i64Ty);
  Value* stackVal = ctx.builder->CreatePtrToInt(stackTop, i64Ty);
  Value* parentTid = ctx.builder->CreatePtrToInt(parentTidPtr, i64Ty);
  Value* childTid = ctx.builder->CreatePtrToInt(childTidPtr, i64Ty);
  Value* tls = ConstantInt::get(i64Ty, 0);  // Not using TLS

  return ctx.builder->CreateCall(
      syscallAsm, {sysno, flagsVal, stackVal, parentTid, childTid, tls},
      "clone_result");
}

Value* ThreadUtils::emitSyscallFutex(Value* addr, Value* op, Value* val) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty,
                                   i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);

  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_FUTEX);
  Value* addrVal = ctx.builder->CreatePtrToInt(addr, i64Ty);
  Value* opVal = ctx.builder->CreateZExtOrTrunc(op, i64Ty);
  Value* valVal = ctx.builder->CreateZExtOrTrunc(val, i64Ty);
  Value* timeout = ConstantInt::get(i64Ty, 0);  // No timeout (infinite wait)
  Value* addr2 = ConstantInt::get(i64Ty, 0);    // Not used
  Value* val3 = ConstantInt::get(i64Ty, 0);     // Not used

  return ctx.builder->CreateCall(
      syscallAsm, {sysno, addrVal, opVal, valVal, timeout, addr2, val3},
      "futex_result");
}

void ThreadUtils::emitSyscallFutexWait(Value* addr, Value* expected) {
  LLVMContext& llvmCtx = ctx.getContext();
  Value* op = ConstantInt::get(Type::getInt64Ty(llvmCtx), FUTEX_WAIT);
  emitSyscallFutex(addr, op, expected);
}

void ThreadUtils::emitSyscallFutexWake(Value* addr) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  Value* op = ConstantInt::get(i64Ty, FUTEX_WAKE);
  Value* numWaiters = ConstantInt::get(i64Ty, 1);  // Wake one waiter
  emitSyscallFutex(addr, op, numWaiters);
}

void ThreadUtils::emitSyscallExit(Value* exitCode) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  std::vector<Type*> paramTypes = {i64Ty, i64Ty};
  FunctionType* asmType =
      FunctionType::get(Type::getVoidTy(llvmCtx), paramTypes, false);

  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall", "{rax},{rdi},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, SYS_EXIT);
  Value* code = ctx.builder->CreateZExtOrTrunc(exitCode, i64Ty);

  ctx.builder->CreateCall(syscallAsm, {sysno, code});
  // exit never returns, but LLVM needs a terminator
  ctx.builder->CreateUnreachable();
}

// -------------------------------------------------------------------
// Thread structure types
// -------------------------------------------------------------------

StructType* ThreadUtils::getThreadContextType() {
  LLVMContext& llvmCtx = ctx.getContext();

  if (auto* existing = StructType::getTypeByName(llvmCtx, "thread_context")) {
    return existing;
  }

  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // { func, env, result_slot, futex_word, stack_base, stack_size }
  return StructType::create(llvmCtx, {ptrTy, ptrTy, ptrTy, i32Ty, ptrTy, i64Ty},
                            "thread_context");
}

StructType* ThreadUtils::getThreadHandleType() {
  LLVMContext& llvmCtx = ctx.getContext();

  if (auto* existing =
          StructType::getTypeByName(llvmCtx, sun::StructNames::Thread)) {
    return existing;
  }

  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  return StructType::create(llvmCtx, {ptrTy, ptrTy, i64Ty},
                            sun::StructNames::Thread);
}
