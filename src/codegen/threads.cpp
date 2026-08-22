// threads.cpp — OS thread support via pthreads
//
// spawn compiles the lambda body's entry into a trampoline function and
// hands it to pthread_create; join is pthread_join plus reading the result
// slot. All thread plumbing goes through libc (see intrinsics/libc.h), so
// the emitted IR is target-neutral.

#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/libc.h"
#include "llvm/IR/Constants.h"
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

  BasicBlock* currentBB = ctx.builder->GetInsertBlock();
  if (!currentBB) {
    logAndThrowError("spawn: no current basic block");
    return nullptr;
  }
  Function* parentFunc = currentBB->getParent();

  // Generate the lambda. A lambda literal yields a pointer to the fat
  // struct { func*, env* }; a variable holding a lambda yields the fat
  // struct by value.
  Value* lambdaVal = codegen(lambdaExpr);
  if (!lambdaVal) {
    logAndThrowError("Failed to generate lambda for spawn");
    return nullptr;
  }

  StructType* fatType = cast<StructType>(lambdaType->toLLVMType(llvmCtx));
  Value* lambdaFat =
      lambdaVal->getType()->isPointerTy()
          ? ctx.builder->CreateLoad(fatType, lambdaVal, "spawn.fat")
          : lambdaVal;
  Value* funcPtr = ctx.builder->CreateExtractValue(lambdaFat, 0, "spawn.func");
  Value* envPtr = ctx.builder->CreateExtractValue(lambdaFat, 1, "spawn.env");

  // Allocate thread context on heap (must outlive this function)
  FunctionCallee mallocFunc = sun::libc::malloc(module);
  StructType* contextType = threadUtils.getThreadContextType();
  Value* contextSize = ConstantInt::get(
      i64Ty, module->getDataLayout().getTypeAllocSize(contextType));
  Value* contextPtr =
      ctx.builder->CreateCall(mallocFunc, {contextSize}, "context_ptr");

  // Store func pointer (index 0) and env pointer (index 1)
  Value* funcFieldPtr =
      ctx.builder->CreateStructGEP(contextType, contextPtr, 0, "ctx.func");
  ctx.builder->CreateStore(funcPtr, funcFieldPtr);
  Value* envFieldPtr =
      ctx.builder->CreateStructGEP(contextType, contextPtr, 1, "ctx.env");
  ctx.builder->CreateStore(envPtr, envFieldPtr);

  // Allocate the result slot and store its pointer (index 2). A void lambda
  // produces no result, so its slot stays null and the trampoline skips the
  // store.
  Value* resultSlot;
  if (resultLLVMType->isVoidTy()) {
    resultSlot = ConstantPointerNull::get(cast<PointerType>(ptrTy));
  } else {
    Value* resultSize = ConstantInt::get(
        i64Ty, module->getDataLayout().getTypeAllocSize(resultLLVMType));
    resultSlot =
        ctx.builder->CreateCall(mallocFunc, {resultSize}, "result_slot");
  }
  Value* resultFieldPtr =
      ctx.builder->CreateStructGEP(contextType, contextPtr, 2, "ctx.result");
  ctx.builder->CreateStore(resultSlot, resultFieldPtr);

  // pthread_create writes the thread id straight into the context (index 3)
  Value* tidFieldPtr =
      ctx.builder->CreateStructGEP(contextType, contextPtr, 3, "ctx.tid");

  FunctionType* lambdaFuncType = lambdaType->toLLVMFunctionType(llvmCtx);
  Function* trampoline = threadUtils.getOrCreateThreadTrampoline(
      lambdaFuncType, fatType, resultLLVMType);

  Value* nullPtr = ConstantPointerNull::get(cast<PointerType>(ptrTy));
  ctx.builder->CreateCall(sun::libc::pthreadCreate(module),
                          {tidFieldPtr, nullPtr, trampoline, contextPtr},
                          "pthread_create_result");

  // Build and return Thread handle
  StructType* handleType = threadUtils.getThreadHandleType();
  AllocaInst* handleAlloca =
      createEntryBlockAlloca(parentFunc, "thread_handle", handleType);

  Value* handleContextPtr = ctx.builder->CreateStructGEP(
      handleType, handleAlloca, 0, "handle.context.ptr");
  ctx.builder->CreateStore(contextPtr, handleContextPtr);

  // Return the handle struct by value: Thread<T> resolves to the handle
  // struct, so `var t = spawn(...)` stores the whole handle and join()
  // extracts its fields from the loaded value
  return ctx.builder->CreateLoad(handleType, handleAlloca, "spawn.handle");
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
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  StructType* handleType = threadUtils.getThreadHandleType();
  StructType* contextType = threadUtils.getThreadContextType();

  // The receiver may arrive as the handle alloca (pointer) or as an
  // already-loaded handle struct value
  Value* handle = threadHandle->getType()->isPointerTy()
                      ? ctx.builder->CreateLoad(handleType, threadHandle,
                                                "join.handle")
                      : threadHandle;

  Value* contextPtr =
      ctx.builder->CreateExtractValue(handle, 0, "join.context");

  // pthread_join blocks until the thread has fully exited
  Value* tidFieldPtr = ctx.builder->CreateStructGEP(contextType, contextPtr, 3,
                                                    "join.tid_ptr");
  Value* tid = ctx.builder->CreateLoad(i64Ty, tidFieldPtr, "join.tid");
  Value* nullPtr = ConstantPointerNull::get(cast<PointerType>(ptrTy));
  ctx.builder->CreateCall(sun::libc::pthreadJoin(module), {tid, nullPtr},
                          "pthread_join_result");

  // Thread is done - load result (a void thread has no result slot)
  Value* resultSlotPtrPtr = ctx.builder->CreateStructGEP(
      contextType, contextPtr, 2, "join.result_ptr_ptr");
  Value* resultSlotPtr =
      ctx.builder->CreateLoad(ptrTy, resultSlotPtrPtr, "join.result_ptr");

  Type* resultLLVMType = typeResolver.resolveForReturn(resultType);
  Value* result = nullptr;
  if (!resultLLVMType->isVoidTy()) {
    result =
        ctx.builder->CreateLoad(resultLLVMType, resultSlotPtr, "join.result");
  }

  // Free result slot (null for void threads - free(null) is a no-op) and
  // context
  FunctionCallee freeFunc = sun::libc::free(module);
  ctx.builder->CreateCall(freeFunc, {resultSlotPtr});
  Value* freeContext = ctx.builder->CreateCall(freeFunc, {contextPtr});

  // A void join has no value; hand back the (void-typed) free call so the
  // caller sees a non-null result
  return result ? result : freeContext;
}
