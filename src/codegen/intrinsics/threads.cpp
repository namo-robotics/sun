// threads.cpp — the parts of OS threading that cannot be written in Sun
//
// `spawn` and `Thread<T>` live in stdlib/thread.sun. What is left here is the
// piece Sun has no way to express: pthread_create wants a `ptr(*)(ptr)` entry
// point, while Sun lambdas use the fat-pointer ABI, so a trampoline has to be
// built per lambda signature. Three intrinsics bridge the two:
//
//   _spawn<F>(fn, args...)  build the context, start the thread
//   _thread_join<T>(ctx)    wait, take the result
//   _thread_join_drop<T>(ctx)  wait, drop the result nobody claimed
//
// The context's layout is std.thread.ThreadContext, declared in Sun, so this
// file and the standard library cannot drift apart. All thread plumbing goes
// through libc (see intrinsics/libc.h), so the emitted IR is target-neutral.

#include "ast.h"
#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics_generator.h"
#include "codegen/intrinsics/libc.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

// -------------------------------------------------------------------
// The thread context, as the standard library declares it
// -------------------------------------------------------------------

// { ptr func, ptr env, ptr result, i64 tid, ptr args } — see
// stdlib/thread.sun. Read off the type semantic analysis already resolved
// rather than synthesized here, so there is one definition of the layout and
// codegen never has to spell the class's name (which carries a library hash
// once std.thread arrives through a bundle).
StructType* IntrinsicsGenerator::getThreadContextStruct(
    const sun::TypePtr& contextPtrType) {
  auto* pointer = sun::tryGetType<sun::RawPointerType>(contextPtrType);
  auto* contextClass =
      pointer ? sun::tryGetType<sun::ClassType>(pointer->getPointeeType())
              : nullptr;
  StructType* contextType =
      contextClass ? contextClass->getStructType(ctx.getContext()) : nullptr;
  if (!contextType || contextType->getNumElements() != 5) {
    logAndThrowError(
        "the thread intrinsics take a raw_ptr<ThreadContext>, and "
        "std.thread.ThreadContext must keep the five fields the compiler "
        "writes into it");
  }
  return contextType;
}

// -------------------------------------------------------------------
// _spawn<F>(fn, args...)
// -------------------------------------------------------------------

// Builds the thread context on the heap — it must outlive this frame — moves
// the arguments into it, and starts the thread. Hands back the context
// pointer; stdlib `spawn` wraps that in the Thread<T> handle that owns it.
Value* IntrinsicsGenerator::codegenSpawnIntrinsic(
    const sun::TypePtr& lambdaSunType, const sun::TypePtr& contextPtrType,
    const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<sun::ArgConversion>& conversions) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  if (args.empty()) {
    logAndThrowError("_spawn<F>() requires the function to run");
  }
  // F is either a lambda (fat pointer, hidden environment argument) or a
  // named-function value (bare one-word pointer, no environment).
  auto* lambdaType = sun::tryGetType<sun::LambdaType>(lambdaSunType);
  auto* namedFnType = sun::tryGetType<sun::FunctionType>(lambdaSunType);
  if (!lambdaType && !namedFnType) {
    logAndThrowError("_spawn<F>() requires a lambda or function type argument");
  }

  // The trampoline has no unwind handling, so an error crossing the spawn
  // boundary would take the whole process down mid-unwind. Catch inside the
  // spawned function and carry failures in its result instead.
  if (lambdaType ? lambdaType->canThrow() : namedFnType->canThrow()) {
    logAndThrowError(
        "a spawned function must not throw; catch errors inside it and "
        "return them as part of its result");
  }

  sun::TypePtr returnType =
      lambdaType ? lambdaType->getReturnType() : namedFnType->getReturnType();
  Type* resultLLVMType = typeResolver.resolveForReturn(returnType);
  StructType* fatType =
      lambdaType ? cast<StructType>(lambdaType->toLLVMType(llvmCtx)) : nullptr;
  FunctionType* calleeFuncType = lambdaType
                                     ? lambdaType->toLLVMFunctionType(llvmCtx)
                                     : namedFnType->toLLVMFunctionType(llvmCtx);
  StructType* contextType = getThreadContextStruct(contextPtrType);

  Value* funcPtr = nullptr;
  Value* envPtr = ConstantPointerNull::get(cast<PointerType>(ptrTy));
  Value* calleeSeed = nullptr;
  if (lambdaType) {
    // The lambda. A literal yields a pointer to the fat struct
    // { func*, env* }; a variable holding one yields the fat struct by value.
    Value* lambdaVal = codegen(*args[0]);
    if (!lambdaVal) {
      logAndThrowError("Failed to generate the lambda for _spawn");
    }
    Value* lambdaFat =
        lambdaVal->getType()->isPointerTy()
            ? ctx.builder->CreateLoad(fatType, lambdaVal, "spawn.fat")
            : lambdaVal;
    funcPtr = ctx.builder->CreateExtractValue(lambdaFat, 0, "spawn.func");
    envPtr = ctx.builder->CreateExtractValue(lambdaFat, 1, "spawn.env");
    calleeSeed = lambdaFat;
  } else {
    // A named-function value is already the callable address.
    funcPtr = codegen(*args[0]);
    if (!funcPtr) {
      logAndThrowError("Failed to generate the function for _spawn");
    }
    calleeSeed = funcPtr;
  }

  // The arguments, lowered exactly as any other call to this callee would
  // lower them: semantic analysis recorded one conversion each, and a
  // compound argument moves, leaving its source invalidated. Slot 0 is the
  // callee itself and never reaches the argument blob.
  std::vector<Value*> argValues{calleeSeed};
  const auto& calleeParams =
      lambdaType ? lambdaType->getParamTypes() : namedFnType->getParamTypes();
  if (!gen_.emitCallArguments(args, conversions, calleeParams, calleeFuncType,
                              argValues, "_spawn",
                              /*firstArg=*/1)) {
    logAndThrowError("Failed to generate the arguments for _spawn");
  }

  FunctionCallee mallocFunc = sun::libc::malloc(module);
  const DataLayout& layout = module->getDataLayout();

  // The argument block: one field per parameter, in declared order. A
  // lambda's LLVM signature carries the hidden environment first, which the
  // blob never stores; a named function's parameters start at slot zero.
  StructType* argsType = nullptr;
  Value* argsBlob = ConstantPointerNull::get(cast<PointerType>(ptrTy));
  if (argValues.size() > 1) {
    std::vector<Type*> fieldTypes(
        calleeFuncType->param_begin() + (lambdaType ? 1 : 0),
        calleeFuncType->param_end());
    argsType = StructType::get(llvmCtx, fieldTypes);
    argsBlob = ctx.builder->CreateCall(
        mallocFunc,
        {ConstantInt::get(i64Ty, layout.getTypeAllocSize(argsType))},
        "spawn.args");
    for (size_t i = 1; i < argValues.size(); ++i) {
      ctx.builder->CreateStore(
          argValues[i],
          ctx.builder->CreateStructGEP(argsType, argsBlob, i - 1,
                                       "spawn.arg." + std::to_string(i - 1)));
    }
  }

  // The context outlives this frame, so it is heap memory the joiner frees.
  Value* contextPtr = ctx.builder->CreateCall(
      mallocFunc,
      {ConstantInt::get(i64Ty, layout.getTypeAllocSize(contextType))},
      "spawn.context");

  ctx.builder->CreateStore(
      funcPtr, ctx.builder->CreateStructGEP(contextType, contextPtr, 0));
  ctx.builder->CreateStore(
      envPtr, ctx.builder->CreateStructGEP(contextType, contextPtr, 1));

  // Where the thread leaves its result. A void thread produces none, so its
  // slot stays null and the trampoline skips the store.
  Value* resultSlot = ConstantPointerNull::get(cast<PointerType>(ptrTy));
  if (!resultLLVMType->isVoidTy()) {
    resultSlot = ctx.builder->CreateCall(
        mallocFunc,
        {ConstantInt::get(i64Ty, layout.getTypeAllocSize(resultLLVMType))},
        "spawn.result");
  }
  ctx.builder->CreateStore(
      resultSlot, ctx.builder->CreateStructGEP(contextType, contextPtr, 2));
  ctx.builder->CreateStore(
      argsBlob, ctx.builder->CreateStructGEP(contextType, contextPtr, 4));

  // Spawned threads get an explicit 8 MiB stack. Libc defaults differ
  // wildly — glibc ~8 MiB, musl 128 KiB, macOS 512 KiB — so recursion that
  // works on the main thread would overflow a worker on some targets. An
  // explicit size makes spawn behave the same however the binary is linked.
  // pthread_attr_t is at most 64 bytes on every supported libc.
  Value* attr = ctx.builder->CreateAlloca(
      ArrayType::get(Type::getInt8Ty(module->getContext()), 64), nullptr,
      "spawn.attr");
  ctx.builder->CreateCall(sun::libc::pthreadAttrInit(module), {attr});
  ctx.builder->CreateCall(
      sun::libc::pthreadAttrSetstacksize(module),
      {attr, ConstantInt::get(i64Ty, 8ull * 1024 * 1024)});

  // pthread_create writes the thread id straight into the context (field 3)
  Value* tidFieldPtr =
      ctx.builder->CreateStructGEP(contextType, contextPtr, 3, "ctx.tid");
  Function* trampoline = threadUtils.getOrCreateThreadTrampoline(
      calleeFuncType, fatType, resultLLVMType, contextType, argsType);
  ctx.builder->CreateCall(sun::libc::pthreadCreate(module),
                          {tidFieldPtr, attr, trampoline, contextPtr},
                          "pthread_create_result");
  ctx.builder->CreateCall(sun::libc::pthreadAttrDestroy(module), {attr});

  return contextPtr;
}

// -------------------------------------------------------------------
// _thread_join<T>(ctx) and _thread_join_drop<T>(ctx)
// -------------------------------------------------------------------

// Block until the thread has exited, then release its context. With a
// non-void result type the result is read out of the slot before the slot is
// freed and handed back; otherwise the (void-typed) free call is, so callers
// see a non-null value.
//
// The slot is raw memory, so freeing it releases the result's own bytes and
// nothing they point at. Reading the result out is therefore a move: the
// caller takes over whatever it owns. When nobody reads it — Thread<T>'s
// deinit joining a handle that was never joined by hand — `dropResultType`
// says what the slot holds so its deinit still runs.
Value* IntrinsicsGenerator::codegenThreadJoinIntrinsic(
    const sun::TypePtr& resultType,
    const std::vector<std::unique_ptr<ExprAST>>& args, bool dropResult) {
  LLVMContext& llvmCtx = ctx.getContext();
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  if (args.size() != 1) {
    logAndThrowError("_thread_join<T>() takes the thread context");
  }
  StructType* contextType = getThreadContextStruct(args[0]->getResolvedType());
  Value* contextPtr = codegen(*args[0]);
  if (!contextPtr) {
    logAndThrowError("Failed to generate the context for _thread_join");
  }

  // pthread_join blocks until the thread has fully exited
  Value* tid = ctx.builder->CreateLoad(
      i64Ty, ctx.builder->CreateStructGEP(contextType, contextPtr, 3),
      "join.tid");
  ctx.builder->CreateCall(
      sun::libc::pthreadJoin(module),
      {tid, ConstantPointerNull::get(cast<PointerType>(ptrTy))},
      "pthread_join_result");

  // The thread is done: take the result (a void thread has no slot)
  Value* resultSlotPtr = ctx.builder->CreateLoad(
      ptrTy, ctx.builder->CreateStructGEP(contextType, contextPtr, 2),
      "join.result_ptr");

  Value* result = nullptr;
  Type* resultLLVMType =
      resultType ? typeResolver.resolveForReturn(resultType) : nullptr;
  if (!dropResult && resultLLVMType && !resultLLVMType->isVoidTy()) {
    result =
        ctx.builder->CreateLoad(resultLLVMType, resultSlotPtr, "join.result");
  } else if (dropResult && sun::typeNeedsDrop(resultType)) {
    scopes().emitDropInPlace(resultType, resultSlotPtr, "join.result");
  }

  // Free the result slot (null for void threads — free(null) is a no-op) and
  // the context itself
  FunctionCallee freeFunc = sun::libc::free(module);
  ctx.builder->CreateCall(freeFunc, {resultSlotPtr});
  Value* freeContext = ctx.builder->CreateCall(freeFunc, {contextPtr});

  return result ? result : freeContext;
}
