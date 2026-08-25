// threads.cpp — OS thread support via pthreads
//
// spawn compiles the lambda body's entry into a trampoline function and
// hands it to pthread_create; join is pthread_join plus reading the result
// slot. All thread plumbing goes through libc (see intrinsics/libc.h), so
// the emitted IR is target-neutral.
//
// Threads are scoped: the handle slot spawn writes is drop-tracked, so a
// thread that was not joined by hand is joined when its handle's scope ends.
// The lambda's environment lives in the spawning frame, and a [ref x]
// capture points straight at x, so both are still standing at that join.

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
  auto& lambdaType =
      sun::requireType<sun::LambdaType>(lambdaExpr, "spawn argument");
  sun::TypePtr returnType = lambdaType.getReturnType();
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

  StructType* fatType = cast<StructType>(lambdaType.toLLVMType(llvmCtx));
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

  FunctionType* lambdaFuncType = lambdaType.toLLVMFunctionType(llvmCtx);
  Function* trampoline = threadUtils.getOrCreateThreadTrampoline(
      lambdaFuncType, fatType, resultLLVMType);

  Value* nullPtr = ConstantPointerNull::get(cast<PointerType>(ptrTy));
  ctx.builder->CreateCall(sun::libc::pthreadCreate(module),
                          {tidFieldPtr, nullPtr, trampoline, contextPtr},
                          "pthread_create_result");

  // Build the thread handle
  StructType* handleType = threadUtils.getThreadHandleType();
  AllocaInst* handleAlloca =
      createEntryBlockAlloca(parentFunc, "thread_handle", handleType);

  Value* handleContextPtr = ctx.builder->CreateStructGEP(
      handleType, handleAlloca, 0, "handle.context.ptr");
  ctx.builder->CreateStore(contextPtr, handleContextPtr);

  // The slot is dropped like a class instance, and dropping a Thread<T>
  // joins it. `var t = spawn(...)` adopts this same slot, so the thread is
  // joined once, at the end of the scope that spawned it.
  trackClassAllocation(handleAlloca, "thread_handle", expr.getResolvedType());

  // Return the slot, not the loaded struct: join() needs its address to
  // record that the thread was already joined.
  return handleAlloca;
}

// Join the thread this handle slot still holds, if any. join() nulls the
// slot, so a hand-written join makes this a no-op. Nobody is taking the
// result of a thread joined this way, so `resultType` is dropped in place.
void CodegenVisitor::emitThreadJoinIfNeeded(Value* handleSlot,
                                            const sun::TypePtr& resultType,
                                            const std::string& name) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  StructType* handleType = threadUtils.getThreadHandleType();

  Value* contextPtr =
      ctx.builder->CreateLoad(ptrTy, handleSlot, name + ".context");
  Value* isNull = ctx.builder->CreateICmpEQ(
      contextPtr, ConstantPointerNull::get(cast<PointerType>(ptrTy)),
      name + ".joined");

  Function* parentFunc = ctx.builder->GetInsertBlock()->getParent();
  auto* joinBB = BasicBlock::Create(llvmCtx, name + ".join", parentFunc);
  auto* doneBB = BasicBlock::Create(llvmCtx, name + ".join.done", parentFunc);
  ctx.builder->CreateCondBr(isNull, doneBB, joinBB);

  ctx.builder->SetInsertPoint(joinBB);
  emitThreadJoinCall(contextPtr, /*resultLLVMType=*/nullptr, resultType, name);
  ctx.builder->CreateStore(
      ConstantPointerNull::get(cast<PointerType>(ptrTy)),
      ctx.builder->CreateStructGEP(handleType, handleSlot, 0));
  ctx.builder->CreateBr(doneBB);

  ctx.builder->SetInsertPoint(doneBB);
}

// -------------------------------------------------------------------
// Join codegen
// -------------------------------------------------------------------

// Block until the thread has exited, then release its context. With a
// non-void result type the result is read out of the slot before the slot is
// freed and handed back; otherwise the (void-typed) free call is, so callers
// see a non-null value.
//
// The slot is raw memory, so freeing it releases the result's own bytes and
// nothing they point at. Reading the result out is therefore a move: the
// caller takes over whatever it owns. When nobody reads it — the automatic
// join at scope exit — `dropResultType` says what the slot holds so its
// deinit still runs.
Value* CodegenVisitor::emitThreadJoinCall(Value* contextPtr,
                                          Type* resultLLVMType,
                                          const sun::TypePtr& dropResultType,
                                          const std::string& name) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  StructType* contextType = threadUtils.getThreadContextType();

  // pthread_join blocks until the thread has fully exited
  Value* tidFieldPtr = ctx.builder->CreateStructGEP(contextType, contextPtr, 3,
                                                    name + ".tid_ptr");
  Value* tid = ctx.builder->CreateLoad(i64Ty, tidFieldPtr, name + ".tid");
  Value* nullPtr = ConstantPointerNull::get(cast<PointerType>(ptrTy));
  ctx.builder->CreateCall(sun::libc::pthreadJoin(module), {tid, nullPtr},
                          "pthread_join_result");

  // Thread is done - load result (a void thread has no result slot)
  Value* resultSlotPtrPtr = ctx.builder->CreateStructGEP(
      contextType, contextPtr, 2, name + ".result_ptr_ptr");
  Value* resultSlotPtr =
      ctx.builder->CreateLoad(ptrTy, resultSlotPtrPtr, name + ".result_ptr");

  Value* result = nullptr;
  if (resultLLVMType && !resultLLVMType->isVoidTy()) {
    result = ctx.builder->CreateLoad(resultLLVMType, resultSlotPtr,
                                     name + ".result");
  } else if (sun::typeNeedsDrop(dropResultType)) {
    emitDropInPlace(dropResultType, resultSlotPtr, name + ".result");
  }

  // Free result slot (null for void threads - free(null) is a no-op) and
  // context
  FunctionCallee freeFunc = sun::libc::free(module);
  ctx.builder->CreateCall(freeFunc, {resultSlotPtr});
  Value* freeContext = ctx.builder->CreateCall(freeFunc, {contextPtr});

  return result ? result : freeContext;
}

// Codegen for Thread.join() method
// Blocks until thread completes, returns the result
Value* CodegenVisitor::codegenThreadJoin(Value* threadHandle,
                                         sun::TypePtr resultType) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  StructType* handleType = threadUtils.getThreadHandleType();

  // The receiver may arrive as the handle slot (pointer) or as an
  // already-loaded handle struct value
  Value* handle =
      threadHandle->getType()->isPointerTy()
          ? ctx.builder->CreateLoad(handleType, threadHandle, "join.handle")
          : threadHandle;

  Value* contextPtr =
      ctx.builder->CreateExtractValue(handle, 0, "join.context");

  // The result is handed to the caller, so it is moved out, not dropped
  Value* result =
      emitThreadJoinCall(contextPtr, typeResolver.resolveForReturn(resultType),
                         /*dropResultType=*/nullptr, "join");

  // Record that this thread is joined, so the automatic join at scope exit
  // does nothing
  if (threadHandle->getType()->isPointerTy()) {
    ctx.builder->CreateStore(
        ConstantPointerNull::get(cast<PointerType>(ptrTy)),
        ctx.builder->CreateStructGEP(handleType, threadHandle, 0));
  }

  return result;
}
