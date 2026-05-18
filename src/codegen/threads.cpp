// threads.cpp — OS thread support via raw Linux syscalls
//
// Implements spawn/join semantics using ThreadUtils for low-level operations:
// - clone(2) for thread creation (syscall 56)
// - futex(2) for join synchronization (syscall 202)
// - mmap(2) for thread stack allocation (syscall 9)
// - munmap(2) for thread stack deallocation (syscall 11)
// - exit(2) for thread exit (syscall 60)
//
// No libc dependency - all syscalls are emitted as inline assembly.

#include "codegen_visitor.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

// -------------------------------------------------------------------
// Spawn codegen
// -------------------------------------------------------------------

// Codegen for spawn expression
// Takes a lambda and returns a Thread<T> handle
Value* CodegenVisitor::codegen(const SpawnExprAST& expr) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // Get the lambda expression and its type
  const ExprAST& lambdaExpr = expr.getLambda();
  sun::TypePtr lambdaSunType = lambdaExpr.getResolvedType();

  if (!lambdaSunType || !lambdaSunType->isLambda()) {
    logAndThrowError("spawn requires a lambda expression");
    return nullptr;
  }

  auto* lambdaType = static_cast<sun::LambdaType*>(lambdaSunType.get());
  sun::TypePtr returnType = lambdaType->getReturnType();
  Type* resultLLVMType = typeResolver.resolveForReturn(returnType);

  // Get current function for creating blocks
  BasicBlock* currentBB = ctx.builder->GetInsertBlock();
  if (!currentBB) {
    logAndThrowError("spawn: no current basic block");
    return nullptr;
  }
  Function* parentFunc = currentBB->getParent();
  if (!parentFunc) {
    logAndThrowError("spawn: no parent function");
    return nullptr;
  }

  // Generate the lambda (returns alloca pointer to fat struct { func*, env* })
  Value* lambdaFatPtr = codegen(lambdaExpr);
  if (!lambdaFatPtr) {
    logAndThrowError("Failed to generate lambda for spawn");
    return nullptr;
  }

  // Load the fat struct from the pointer
  StructType* fatType = cast<StructType>(lambdaType->toLLVMType(llvmCtx));
  Value* lambdaFat =
      ctx.builder->CreateLoad(fatType, lambdaFatPtr, "spawn.fat");

  // Extract function pointer and env pointer from fat struct
  Value* funcPtr = ctx.builder->CreateExtractValue(lambdaFat, 0, "spawn.func");
  Value* envPtr = ctx.builder->CreateExtractValue(lambdaFat, 1, "spawn.env");

  // Allocate thread stack via mmap
  Value* stackSize = ConstantInt::get(i64Ty, ThreadUtils::DEFAULT_STACK_SIZE);
  Value* stackBase = threadUtils.emitSyscallMmap(stackSize);

  // Stack grows down on x86_64, so thread starts at top of allocation
  Value* stackBaseInt = ctx.builder->CreatePtrToInt(stackBase, i64Ty);
  Value* stackTopInt =
      ctx.builder->CreateAdd(stackBaseInt, stackSize, "stack_top_int");

  // Align stack to 16 bytes (required by x86_64 ABI)
  Value* alignMask = ConstantInt::get(i64Ty, ~0xFULL);
  stackTopInt = ctx.builder->CreateAnd(stackTopInt, alignMask, "stack_aligned");
  Value* stackTop =
      ctx.builder->CreateIntToPtr(stackTopInt, ptrTy, "stack_top");

  // Allocate thread context on heap (must outlive this function)
  Function* mallocFunc = module->getFunction("malloc");
  if (!mallocFunc) {
    FunctionType* mallocType = FunctionType::get(ptrTy, {i64Ty}, false);
    mallocFunc = Function::Create(mallocType, Function::ExternalLinkage,
                                  "malloc", module);
  }

  StructType* contextType = threadUtils.getThreadContextType();
  Value* contextSize = ConstantInt::get(
      i64Ty, module->getDataLayout().getTypeAllocSize(contextType));
  Value* contextPtr =
      ctx.builder->CreateCall(mallocFunc, {contextSize}, "context_ptr");

  // Store func pointer (index 0)
  Value* funcFieldPtr =
      ctx.builder->CreateStructGEP(contextType, contextPtr, 0, "ctx.func");
  ctx.builder->CreateStore(funcPtr, funcFieldPtr);

  // Store env pointer (index 1)
  Value* envFieldPtr =
      ctx.builder->CreateStructGEP(contextType, contextPtr, 1, "ctx.env");
  ctx.builder->CreateStore(envPtr, envFieldPtr);

  // Allocate result slot
  Value* resultSize = ConstantInt::get(
      i64Ty, module->getDataLayout().getTypeAllocSize(resultLLVMType));
  Value* resultSlot =
      ctx.builder->CreateCall(mallocFunc, {resultSize}, "result_slot");

  // Store result slot pointer (index 2)
  Value* resultFieldPtr =
      ctx.builder->CreateStructGEP(contextType, contextPtr, 2, "ctx.result");
  ctx.builder->CreateStore(resultSlot, resultFieldPtr);

  // Initialize futex word to 0 (index 3) - not done
  Value* futexFieldPtr =
      ctx.builder->CreateStructGEP(contextType, contextPtr, 3, "ctx.futex");
  ctx.builder->CreateStore(ConstantInt::get(i32Ty, 0), futexFieldPtr);

  // Store stack base (index 4)
  Value* stackBaseFieldPtr = ctx.builder->CreateStructGEP(
      contextType, contextPtr, 4, "ctx.stack_base");
  ctx.builder->CreateStore(stackBase, stackBaseFieldPtr);

  // Store stack size (index 5)
  Value* stackSizeFieldPtr = ctx.builder->CreateStructGEP(
      contextType, contextPtr, 5, "ctx.stack_size");
  ctx.builder->CreateStore(stackSize, stackSizeFieldPtr);

  // Store context pointer on child's stack so it can find it after clone
  // Push context pointer just below stack top (stack grows down)
  Value* ctxSlotInt = ctx.builder->CreateSub(
      stackTopInt, ConstantInt::get(i64Ty, 8), "ctx_slot_int");
  Value* ctxSlotPtr = ctx.builder->CreateIntToPtr(
      ctxSlotInt, PointerType::getUnqual(ptrTy), "ctx_slot_ptr");
  ctx.builder->CreateStore(contextPtr, ctxSlotPtr);

  // Adjust stack top for the pushed value
  Value* adjustedStackTop =
      ctx.builder->CreateIntToPtr(ctxSlotInt, ptrTy, "adjusted_stack_top");

  // Clone flags
  Value* flags = ConstantInt::get(i64Ty, ThreadUtils::THREAD_FLAGS);

  // Parent TID storage
  AllocaInst* parentTidAlloca =
      createEntryBlockAlloca(parentFunc, "parent_tid", i32Ty);

  // Child TID storage - use the futex word so kernel clears it on exit
  Value* childTidPtr = futexFieldPtr;

  // Call clone
  Value* tid = threadUtils.emitSyscallClone(flags, adjustedStackTop,
                                            parentTidAlloca, childTidPtr);

  // Create basic blocks for parent/child paths
  BasicBlock* childBB = BasicBlock::Create(llvmCtx, "spawn.child", parentFunc);
  BasicBlock* parentBB =
      BasicBlock::Create(llvmCtx, "spawn.parent", parentFunc);

  // Branch based on clone result: 0 = child, positive = parent
  Value* isChild =
      ctx.builder->CreateICmpEQ(tid, ConstantInt::get(i64Ty, 0), "is_child");
  ctx.builder->CreateCondBr(isChild, childBB, parentBB);

  // ========== CHILD PATH ==========
  ctx.builder->SetInsertPoint(childBB);

  // Pop context pointer from stack (it was pushed before clone)
  // Actually, after clone the child's RSP points to adjustedStackTop
  // and the context pointer is at [RSP], so we load it
  Value* childContextPtr =
      ctx.builder->CreateLoad(ptrTy, ctxSlotPtr, "child.context");

  // Load func and env from context
  Value* childFuncFieldPtr = ctx.builder->CreateStructGEP(
      contextType, childContextPtr, 0, "child.func_ptr");
  Value* childFunc =
      ctx.builder->CreateLoad(ptrTy, childFuncFieldPtr, "child.func");

  Value* childEnvFieldPtr = ctx.builder->CreateStructGEP(
      contextType, childContextPtr, 1, "child.env_ptr");
  Value* childEnv =
      ctx.builder->CreateLoad(ptrTy, childEnvFieldPtr, "child.env");

  // Build fat pointer for lambda call
  Value* childFat = UndefValue::get(fatType);
  childFat =
      ctx.builder->CreateInsertValue(childFat, childFunc, 0, "child.fat.func");
  childFat =
      ctx.builder->CreateInsertValue(childFat, childEnv, 1, "child.fat.env");

  // Create temp alloca for fat pointer (lambda calling convention expects ptr
  // to fat)
  AllocaInst* childFatAlloca =
      ctx.builder->CreateAlloca(fatType, nullptr, "child.fat.alloca");
  ctx.builder->CreateStore(childFat, childFatAlloca);

  // Call the lambda function: result = func(fat_ptr)
  FunctionType* lambdaFuncType = lambdaType->toLLVMFunctionType(llvmCtx);
  Value* childResult = ctx.builder->CreateCall(
      lambdaFuncType, childFunc, {childFatAlloca}, "child.result");

  // Load result slot pointer and store result
  Value* childResultFieldPtr = ctx.builder->CreateStructGEP(
      contextType, childContextPtr, 2, "child.result_slot_ptr");
  Value* childResultSlot =
      ctx.builder->CreateLoad(ptrTy, childResultFieldPtr, "child.result_slot");
  ctx.builder->CreateStore(childResult, childResultSlot);

  // Signal completion: set futex word to 1 and wake waiters
  Value* childFutexFieldPtr = ctx.builder->CreateStructGEP(
      contextType, childContextPtr, 3, "child.futex_ptr");
  ctx.builder->CreateStore(ConstantInt::get(i32Ty, 1), childFutexFieldPtr);
  threadUtils.emitSyscallFutexWake(childFutexFieldPtr);

  // Exit the thread
  threadUtils.emitSyscallExit(ConstantInt::get(i64Ty, 0));
  // Note: emitSyscallExit adds unreachable, so no need for terminator

  // ========== PARENT PATH ==========
  ctx.builder->SetInsertPoint(parentBB);

  // Build and return Thread handle
  StructType* handleType = threadUtils.getThreadHandleType();
  AllocaInst* handleAlloca =
      createEntryBlockAlloca(parentFunc, "thread_handle", handleType);

  // Store fields in handle
  Value* handleContextPtr = ctx.builder->CreateStructGEP(
      handleType, handleAlloca, 0, "handle.context.ptr");
  ctx.builder->CreateStore(contextPtr, handleContextPtr);

  Value* handleStackPtr = ctx.builder->CreateStructGEP(handleType, handleAlloca,
                                                       1, "handle.stack.ptr");
  ctx.builder->CreateStore(stackBase, handleStackPtr);

  Value* handleSizePtr = ctx.builder->CreateStructGEP(handleType, handleAlloca,
                                                      2, "handle.size.ptr");
  ctx.builder->CreateStore(stackSize, handleSizePtr);

  return handleAlloca;
}

// -------------------------------------------------------------------
// Join codegen
// -------------------------------------------------------------------

// Codegen for Thread.join() method
// Blocks until thread completes, returns the result
Value* CodegenVisitor::codegenThreadJoin(Value* threadHandle,
                                         sun::TypePtr resultType) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  StructType* handleType = threadUtils.getThreadHandleType();
  StructType* contextType = threadUtils.getThreadContextType();

  // Load handle from alloca
  Value* handle =
      ctx.builder->CreateLoad(handleType, threadHandle, "join.handle");

  // Extract context pointer from handle
  Value* contextPtr =
      ctx.builder->CreateExtractValue(handle, 0, "join.context");
  Value* stackBase =
      ctx.builder->CreateExtractValue(handle, 1, "join.stack_base");
  Value* stackSize =
      ctx.builder->CreateExtractValue(handle, 2, "join.stack_size");

  // Get futex word pointer (index 3 in new layout)
  Value* futexWordPtr = ctx.builder->CreateStructGEP(contextType, contextPtr, 3,
                                                     "join.futex_ptr");

  // Wait loop: while futex_word == 0, call futex_wait
  Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
  BasicBlock* checkBB = BasicBlock::Create(llvmCtx, "join.check", currentFunc);
  BasicBlock* waitBB = BasicBlock::Create(llvmCtx, "join.wait", currentFunc);
  BasicBlock* doneBB = BasicBlock::Create(llvmCtx, "join.done", currentFunc);

  ctx.builder->CreateBr(checkBB);

  // Check if thread is done
  ctx.builder->SetInsertPoint(checkBB);
  Value* futexVal = ctx.builder->CreateLoad(i32Ty, futexWordPtr, "futex_val");
  Value* isDone = ctx.builder->CreateICmpNE(
      futexVal, ConstantInt::get(i32Ty, 0), "is_done");
  ctx.builder->CreateCondBr(isDone, doneBB, waitBB);

  // Wait for thread
  ctx.builder->SetInsertPoint(waitBB);
  threadUtils.emitSyscallFutexWait(futexWordPtr, ConstantInt::get(i32Ty, 0));
  ctx.builder->CreateBr(checkBB);  // Recheck after waking

  // Thread is done - load result
  ctx.builder->SetInsertPoint(doneBB);

  // Get result slot pointer (index 2 in new layout)
  Value* resultSlotPtrPtr = ctx.builder->CreateStructGEP(
      contextType, contextPtr, 2, "join.result_ptr_ptr");
  Value* resultSlotPtr =
      ctx.builder->CreateLoad(ptrTy, resultSlotPtrPtr, "join.result_ptr");

  // Load result value
  Type* resultLLVMType = typeResolver.resolveForReturn(resultType);
  Value* result =
      ctx.builder->CreateLoad(resultLLVMType, resultSlotPtr, "join.result");

  // Free result slot
  Function* freeFunc = module->getFunction("free");
  if (!freeFunc) {
    FunctionType* freeType =
        FunctionType::get(Type::getVoidTy(llvmCtx), {ptrTy}, false);
    freeFunc =
        Function::Create(freeType, Function::ExternalLinkage, "free", module);
  }
  ctx.builder->CreateCall(freeFunc, {resultSlotPtr});

  // Free thread stack via munmap
  threadUtils.emitSyscallMunmap(stackBase, stackSize);

  // Free context
  ctx.builder->CreateCall(freeFunc, {contextPtr});

  return result;
}
