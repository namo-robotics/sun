// thread_utils.cpp — Thread support utilities for code generation
//
// Threads are pthreads (see include/codegen/intrinsics/libc.h). The
// wait-on-address primitive Mutex builds on differs per OS: Linux's futex
// has no libc wrapper, so it goes through libc's syscall() with a per-target
// syscall number; macOS has no futex at all, so there the same intrinsics
// lower to Darwin's __ulock_wait/__ulock_wake.

#include "codegen/intrinsics/thread_utils.h"

#include <llvm/TargetParser/Triple.h>

#include "codegen/intrinsics/libc.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "support/error.h"

using namespace llvm;

// Futex operations
static constexpr int64_t FUTEX_WAIT = 0;
static constexpr int64_t FUTEX_WAKE = 1;

// Darwin ulock operation: compare-and-wait on a 32-bit word, the futex
// equivalent. (ULF_WAKE_ALL, 0x100, would wake every waiter; the intrinsics
// wake one, matching the futex path.)
static constexpr uint64_t UL_COMPARE_AND_WAIT = 1;

// Whether this module compiles for an OS that uses ulock instead of futex.
static bool isDarwinTarget(const llvm::Module* module) {
  return llvm::Triple(module->getTargetTriple()).isOSDarwin();
}

// The futex syscall number is the one per-target constant left in thread
// support — it is data, not assembly, so each Linux target is one table row.
// Darwin never reaches this: its targets take the ulock path instead.
static int64_t futexSyscallNumber(const llvm::Module* module) {
  llvm::Triple triple(module->getTargetTriple());
  switch (triple.getArch()) {
    case llvm::Triple::x86_64:
      return 202;
    case llvm::Triple::aarch64:
      return 98;
    default:
      logAndThrowError("no futex syscall number for target '" +
                       module->getTargetTriple() + "'");
  }
}

// -------------------------------------------------------------------
// Futex emitters
// -------------------------------------------------------------------

Value* ThreadUtils::emitSyscallFutex(Value* addr, Value* op, Value* val) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // long syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3)
  Value* sysno = ConstantInt::get(i64Ty, futexSyscallNumber(module));
  Value* opVal = ctx.builder->CreateZExtOrTrunc(op, i64Ty);
  Value* valVal = ctx.builder->CreateZExtOrTrunc(val, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  return ctx.builder->CreateCall(
      sun::libc::syscall(module),
      {sysno, ctx.builder->CreatePtrToInt(addr, i64Ty), opVal, valVal, zero,
       zero, zero},
      "futex_result");
}

void ThreadUtils::emitSyscallFutexWait(Value* addr, Value* expected) {
  LLVMContext& llvmCtx = ctx.getContext();

  if (isDarwinTarget(module)) {
    // int __ulock_wait(op, addr, value, timeout_us); timeout 0 waits forever.
    auto* i32Ty = Type::getInt32Ty(llvmCtx);
    auto* i64Ty = Type::getInt64Ty(llvmCtx);
    ctx.builder->CreateCall(
        sun::libc::ulockWait(module),
        {ConstantInt::get(i32Ty, UL_COMPARE_AND_WAIT), addr,
         ctx.builder->CreateZExt(expected, i64Ty, "ulock.expected"),
         ConstantInt::get(i32Ty, 0)},
        "ulock_wait_result");
    return;
  }

  Value* op = ConstantInt::get(Type::getInt64Ty(llvmCtx), FUTEX_WAIT);
  emitSyscallFutex(addr, op, expected);
}

void ThreadUtils::emitSyscallFutexWake(Value* addr) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  if (isDarwinTarget(module)) {
    // int __ulock_wake(op, addr, wake_value); wakes one waiter, like the
    // futex path below.
    auto* i32Ty = Type::getInt32Ty(llvmCtx);
    ctx.builder->CreateCall(sun::libc::ulockWake(module),
                            {ConstantInt::get(i32Ty, UL_COMPARE_AND_WAIT), addr,
                             ConstantInt::get(i64Ty, 0)},
                            "ulock_wake_result");
    return;
  }

  Value* op = ConstantInt::get(i64Ty, FUTEX_WAKE);
  Value* numWaiters = ConstantInt::get(i64Ty, 1);  // Wake one waiter
  emitSyscallFutex(addr, op, numWaiters);
}

// -------------------------------------------------------------------
// Thread trampoline
// -------------------------------------------------------------------

Function* ThreadUtils::getOrCreateThreadTrampoline(FunctionType* lambdaFuncType,
                                                   StructType* fatType,
                                                   Type* resultLLVMType,
                                                   StructType* contextType,
                                                   StructType* argsType) {
  LLVMContext& llvmCtx = ctx.getContext();

  // Two spawns of same-typed callees share one trampoline. The argument
  // struct is part of that sameness: two lambdas can agree on their return
  // type and still take different arguments. A null fatType means the callee
  // is a named-function value: a bare pointer with no environment.
  std::string key;
  raw_string_ostream keyStream(key);
  lambdaFuncType->print(keyStream);
  resultLLVMType->print(keyStream);
  if (argsType) argsType->print(keyStream);
  keyStream << (fatType ? "/fat" : "/bare");
  if (auto it = trampolineCache.find(keyStream.str());
      it != trampolineCache.end()) {
    return it->second;
  }

  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // ptr __sun_thread_start(ptr context) — LLVM uniques the name per module.
  FunctionType* funcType = FunctionType::get(ptrTy, {ptrTy}, false);
  Function* func = Function::Create(funcType, Function::InternalLinkage,
                                    "__sun_thread_start", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  Value* contextPtr = func->arg_begin();
  contextPtr->setName("context");

  Value* funcFieldPtr =
      builder.CreateStructGEP(contextType, contextPtr, 0, "ctx.func_ptr");
  Value* lambdaFunc = builder.CreateLoad(ptrTy, funcFieldPtr, "lambda.func");

  std::vector<Value*> callArgs;
  if (fatType) {
    Value* envFieldPtr =
        builder.CreateStructGEP(contextType, contextPtr, 1, "ctx.env_ptr");
    Value* lambdaEnv = builder.CreateLoad(ptrTy, envFieldPtr, "lambda.env");

    // Rebuild the fat pointer; the lambda calling convention expects a
    // pointer to the fat struct as its argument. A named function has no
    // environment and is called straight through the loaded pointer.
    Value* fat = UndefValue::get(fatType);
    fat = builder.CreateInsertValue(fat, lambdaFunc, 0, "fat.func");
    fat = builder.CreateInsertValue(fat, lambdaEnv, 1, "fat.env");
    AllocaInst* fatAlloca =
        builder.CreateAlloca(fatType, nullptr, "fat.alloca");
    builder.CreateStore(fat, fatAlloca);
    callArgs.push_back(fatAlloca);
  }

  // The arguments spawn moved into the context, in the order the callee
  // declares them. Each was stored as the very value an ordinary call would
  // have passed, so reading it back needs no conversion.
  Value* argsBlob = nullptr;
  if (argsType) {
    Value* argsFieldPtr =
        builder.CreateStructGEP(contextType, contextPtr, 4, "ctx.args_ptr");
    argsBlob = builder.CreateLoad(ptrTy, argsFieldPtr, "args.blob");
    for (unsigned i = 0; i < argsType->getNumElements(); ++i) {
      Value* slot = builder.CreateStructGEP(argsType, argsBlob, i,
                                            "args." + std::to_string(i) + ".p");
      callArgs.push_back(builder.CreateLoad(argsType->getElementType(i), slot,
                                            "args." + std::to_string(i)));
    }
  }

  Value* result = builder.CreateCall(
      lambdaFuncType, lambdaFunc, callArgs,
      lambdaFuncType->getReturnType()->isVoidTy() ? "" : "result");

  // The arguments moved into the lambda's own parameter slots, so the blob is
  // dead the moment the call returns and the thread releases it.
  if (argsBlob) {
    builder.CreateCall(sun::libc::free(module), {argsBlob});
  }

  if (!resultLLVMType->isVoidTy()) {
    Value* resultFieldPtr =
        builder.CreateStructGEP(contextType, contextPtr, 2, "ctx.result_ptr");
    Value* resultSlot =
        builder.CreateLoad(ptrTy, resultFieldPtr, "result_slot");
    builder.CreateStore(result, resultSlot);
  }

  builder.CreateRet(ConstantPointerNull::get(cast<PointerType>(ptrTy)));

  trampolineCache[keyStream.str()] = func;
  return func;
}
