// error_handling.cpp — Codegen for error handling expressions (try/catch/throw)

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include "codegen_visitor.h"

using namespace llvm;

// Safe division/modulo: check for zero divisor and throw instead of crashing.
// This is only called when currentFunctionCanError is true.
Value* CodegenVisitor::codegenSafeDivision(Value* L, Value* R, bool isModulo) {
  Function* func = ctx.builder->GetInsertBlock()->getParent();

  // Check if R == 0
  Value* isZero = ctx.builder->CreateICmpEQ(
      R, ConstantInt::get(R->getType(), 0), "div.zero.check");

  BasicBlock* zeroBB = BasicBlock::Create(ctx.getContext(), "div.zero", func);
  BasicBlock* safeBB = BasicBlock::Create(ctx.getContext(), "div.safe", func);

  ctx.builder->CreateCondBr(isZero, zeroBB, safeBB);

  // Zero case: throw a division-by-zero exception. We don't have a concrete
  // stdlib error object here, so we throw a bare exception carrying a null
  // IError fat pointer — enough to unwind into the caller's catch.
  ctx.builder->SetInsertPoint(zeroBB);
  llvm::StructType* fatTy =
      sun::InterfaceType::getFatPointerType(ctx.getContext());
  const DataLayout& DL = module->getDataLayout();
  uint64_t fatSize = DL.getTypeAllocSize(fatTy);
  Value* exc = ctx.builder->CreateCall(
      getCxaAllocateException(),
      {ConstantInt::get(Type::getInt64Ty(ctx.getContext()), fatSize)}, "exc");
  ctx.builder->CreateStore(Constant::getNullValue(fatTy), exc);
  emitCxaThrowAndUnreachable(exc);

  // Safe case: perform division or modulo and continue.
  ctx.builder->SetInsertPoint(safeBB);
  Value* result = isModulo ? ctx.builder->CreateSRem(L, R, "modtmp")
                           : ctx.builder->CreateSDiv(L, R, "divtmp");
  return result;
}

// Short-circuit logical operators (and, or)
// 'and' evaluates to: LHS ? RHS : false
// 'or' evaluates to: LHS ? true : RHS
Value* CodegenVisitor::codegenLogicalOp(const BinaryExprAST& expr) {
  bool isAnd = (expr.getOp().kind == TokenKind::AND);

  // Evaluate LHS
  Value* L = codegen(*expr.getLHS());
  if (!L) return nullptr;

  // Convert LHS to bool (i1) if not already
  if (!L->getType()->isIntegerTy(1)) {
    if (L->getType()->isFloatingPointTy()) {
      L = ctx.builder->CreateFCmpONE(L, ConstantFP::get(L->getType(), 0.0),
                                      "tobool");
    } else if (L->getType()->isIntegerTy()) {
      L = ctx.builder->CreateICmpNE(L, ConstantInt::get(L->getType(), 0),
                                     "tobool");
    } else {
      logAndThrowError("Logical operator requires boolean-compatible operand",
                       expr.getLocation());
    }
  }

  Function* TheFunction = ctx.builder->GetInsertBlock()->getParent();

  // For 'and': if LHS is false, result is false (don't evaluate RHS)
  // For 'or': if LHS is true, result is true (don't evaluate RHS)
  BasicBlock* EvalRhsBB =
      BasicBlock::Create(ctx.getContext(), isAnd ? "and.rhs" : "or.rhs", TheFunction);
  BasicBlock* MergeBB =
      BasicBlock::Create(ctx.getContext(), isAnd ? "and.end" : "or.end", TheFunction);

  // Remember the block where LHS was evaluated (for PHI)
  BasicBlock* LhsBB = ctx.builder->GetInsertBlock();

  // For 'and': branch to RHS evaluation if LHS is true, otherwise short-circuit to merge
  // For 'or': branch to RHS evaluation if LHS is false, otherwise short-circuit to merge
  if (isAnd) {
    ctx.builder->CreateCondBr(L, EvalRhsBB, MergeBB);
  } else {
    ctx.builder->CreateCondBr(L, MergeBB, EvalRhsBB);
  }

  // Evaluate RHS
  ctx.builder->SetInsertPoint(EvalRhsBB);
  Value* R = codegen(*expr.getRHS());
  if (!R) return nullptr;

  // Convert RHS to bool (i1) if not already
  if (!R->getType()->isIntegerTy(1)) {
    if (R->getType()->isFloatingPointTy()) {
      R = ctx.builder->CreateFCmpONE(R, ConstantFP::get(R->getType(), 0.0),
                                      "tobool");
    } else if (R->getType()->isIntegerTy()) {
      R = ctx.builder->CreateICmpNE(R, ConstantInt::get(R->getType(), 0),
                                     "tobool");
    } else {
      logAndThrowError("Logical operator requires boolean-compatible operand",
                       expr.getLocation());
    }
  }

  // Branch to merge block after evaluating RHS
  ctx.builder->CreateBr(MergeBB);

  // Update RhsBB (codegen of RHS might have changed the current block)
  BasicBlock* RhsBB = ctx.builder->GetInsertBlock();

  // Emit merge block with PHI node
  ctx.builder->SetInsertPoint(MergeBB);
  PHINode* PN = ctx.builder->CreatePHI(Type::getInt1Ty(ctx.getContext()), 2,
                                        isAnd ? "and.result" : "or.result");

  // For 'and': short-circuit value is false, evaluated value is RHS
  // For 'or': short-circuit value is true, evaluated value is RHS
  Value* ShortCircuitVal = isAnd ? ConstantInt::getFalse(ctx.getContext())
                                  : ConstantInt::getTrue(ctx.getContext());
  PN->addIncoming(ShortCircuitVal, LhsBB);
  PN->addIncoming(R, RhsBB);

  return PN;
}

// Emit __cxa_throw of an already-populated exception buffer, then terminate
// with unreachable. If we're inside a try block, the throw is an `invoke` that
// unwinds to that try's landing pad so the exception is caught in the same
// function; otherwise it's a plain call that unwinds into the caller.
void CodegenVisitor::emitCxaThrowAndUnreachable(Value* excPtr) {
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  FunctionCallee cxaThrow = getCxaThrow();
  Value* tinfo = getSunExceptionTypeInfo();
  Value* nullPtr = ConstantPointerNull::get(PointerType::getUnqual(ctx.getContext()));

  if (!tryStack.empty()) {
    ensurePersonality(func);
    BasicBlock* contBB =
        BasicBlock::Create(ctx.getContext(), "throw.cont", func);
    ctx.builder->CreateInvoke(cxaThrow, contBB, tryStack.back().landingPad,
                              {excPtr, tinfo, nullPtr});
    ctx.builder->SetInsertPoint(contBB);
  } else {
    ctx.builder->CreateCall(cxaThrow, {excPtr, tinfo, nullPtr});
  }
  // __cxa_throw is noreturn: the normal-continuation edge is unreachable.
  ctx.builder->CreateUnreachable();

  // Leave the builder in a dead, already-terminated block so callers that
  // inspect `getInsertBlock()->getTerminator()` see the throw as terminating
  // (matching the old return-based throw), while any dead follow-on code still
  // has a valid insertion point.
  BasicBlock* deadBB =
      BasicBlock::Create(ctx.getContext(), "throw.unreachable", func);
  ctx.builder->SetInsertPoint(deadBB);
  ctx.builder->CreateUnreachable();
}

// Codegen for throw expression: throw <expr>
// Boxes the thrown IError object into a C++ ABI exception and __cxa_throws it.
// Exception buffer layout: { InterfaceFat fat; <object bytes> } where fat.data
// points at the embedded object copy so it survives stack unwinding.
Value* CodegenVisitor::codegen(const ThrowExprAST& expr) {
  if (!currentFunctionCanError) {
    logAndThrowError(
        "throw can only be used in functions declared with ', IError'");
    return nullptr;
  }

  llvm::StructType* fatTy =
      sun::InterfaceType::getFatPointerType(ctx.getContext());
  const DataLayout& DL = module->getDataLayout();
  uint64_t fatSize = DL.getTypeAllocSize(fatTy);
  auto* i64Ty = Type::getInt64Ty(ctx.getContext());
  auto* i8Ty = Type::getInt8Ty(ctx.getContext());

  sun::TypePtr errType =
      expr.hasErrorExpr() ? expr.getErrorExpr().getResolvedType() : nullptr;

  if (errType && errType->isClass()) {
    // Concrete class: copy the object into the exception buffer and build a
    // fat pointer that references the embedded copy.
    auto* classType = static_cast<sun::ClassType*>(errType.get());
    llvm::StructType* classStruct = classType->getStructType(ctx.getContext());
    uint64_t objSize = DL.getTypeAllocSize(classStruct);

    Value* objPtr = codegen(expr.getErrorExpr());
    if (!objPtr) return nullptr;

    Value* exc = ctx.builder->CreateCall(
        getCxaAllocateException(),
        {ConstantInt::get(i64Ty, fatSize + objSize)}, "exc");
    // Object slot lives right after the fat pointer.
    Value* objSlot = ctx.builder->CreateGEP(
        i8Ty, exc, {ConstantInt::get(i64Ty, fatSize)}, "exc.obj");
    Value* objVal = ctx.builder->CreateLoad(classStruct, objPtr, "throw.obj");
    ctx.builder->CreateStore(objVal, objSlot);

    auto ierror = typeRegistry->getInterface("IError");
    Value* fat = createInterfaceFatPointer(objSlot, classType, ierror.get());
    ctx.builder->CreateStore(fat, exc);

    emitCxaThrowAndUnreachable(exc);
  } else {
    // Interface value (e.g. rethrow of a caught `e`) or unknown: box the fat
    // pointer as-is. For a rethrow the underlying object outlives the throw
    // (its owning exception is not released until __cxa_end_catch, which the
    // unreachable throw path skips).
    Value* fatVal = expr.hasErrorExpr() ? codegen(expr.getErrorExpr()) : nullptr;
    Value* exc = ctx.builder->CreateCall(
        getCxaAllocateException(), {ConstantInt::get(i64Ty, fatSize)}, "exc");
    if (fatVal && fatVal->getType() == fatTy) {
      ctx.builder->CreateStore(fatVal, exc);
    } else if (fatVal && fatVal->getType()->isPointerTy()) {
      Value* loaded = ctx.builder->CreateLoad(fatTy, fatVal, "throw.fat");
      ctx.builder->CreateStore(loaded, exc);
    } else {
      ctx.builder->CreateStore(Constant::getNullValue(fatTy), exc);
    }
    emitCxaThrowAndUnreachable(exc);
  }

  return nullptr;
}

// Codegen for unsafe block: unsafe { ... }
// Simply generates code for the body - safety checks are done at semantic analysis
Value* CodegenVisitor::codegen(const UnsafeBlockAST& expr) {
  return codegen(expr.getBody());
}

// Codegen for try-catch expression: try { ... } catch (e: IError) { ... }
// Throwing calls in the try block are emitted as `invoke`s unwinding to a
// landing pad, which enters a C++ ABI catch (__cxa_begin_catch), binds the
// caught IError object to the catch variable, runs the catch body, and calls
// __cxa_end_catch.
Value* CodegenVisitor::codegen(const TryCatchExprAST& expr) {
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  ensurePersonality(func);

  BasicBlock* lpadBB = BasicBlock::Create(ctx.getContext(), "try.lpad", func);
  BasicBlock* mergeBB = BasicBlock::Create(ctx.getContext(), "try.merge", func);

  // Push try context so throwing calls in the body invoke to this landing pad.
  tryStack.push_back({lpadBB});

  pushScope();
  Value* tryResult = codegen(expr.getTryBlock());
  popScope();

  tryStack.pop_back();

  // Fallthrough of the try body (no exception): branch to merge, carrying the
  // try block's value so `try { expr }` can be used as an expression.
  std::vector<std::pair<Value*, BasicBlock*>> results;
  if (!ctx.builder->GetInsertBlock()->getTerminator()) {
    BasicBlock* tryEndBB = ctx.builder->GetInsertBlock();
    ctx.builder->CreateBr(mergeBB);
    if (tryResult) results.push_back({tryResult, tryEndBB});
  }

  // ---- Landing pad ----
  ctx.builder->SetInsertPoint(lpadBB);
  auto* ptrTy = PointerType::getUnqual(ctx.getContext());
  auto* i32Ty = Type::getInt32Ty(ctx.getContext());
  llvm::StructType* lpadTy = StructType::get(ptrTy, i32Ty);
  llvm::LandingPadInst* lp = ctx.builder->CreateLandingPad(lpadTy, 1, "lpad");
  // Single catch-all clause (catch(...)) — every Sun catch is `catch(e:IError)`.
  lp->addClause(ConstantPointerNull::get(ptrTy));
  Value* excPtr = ctx.builder->CreateExtractValue(lp, 0, "exc.ptr");
  Value* obj = ctx.builder->CreateCall(getCxaBeginCatch(), {excPtr}, "exc.obj");

  // Reconstruct the IError fat pointer from the exception buffer (layout:
  // { InterfaceFat fat; <object bytes> }) — the fat pointer sits at offset 0.
  llvm::StructType* fatTy =
      sun::InterfaceType::getFatPointerType(ctx.getContext());

  pushScope();
  const auto& catchClause = expr.getCatchClause();
  if (!catchClause.bindingName.empty()) {
    IRBuilder<> tmpBuilder(&func->getEntryBlock(),
                           func->getEntryBlock().begin());
    AllocaInst* alloca =
        tmpBuilder.CreateAlloca(fatTy, nullptr, catchClause.bindingName);
    Value* fat = ctx.builder->CreateLoad(fatTy, obj, "err.fat");
    ctx.builder->CreateStore(fat, alloca);
    scopes.back().variables[catchClause.bindingName] = alloca;
  }

  Value* catchResult = codegen(*catchClause.body);
  popScope();

  // End the catch scope on the fallthrough path, then merge, carrying the
  // catch value.
  if (!ctx.builder->GetInsertBlock()->getTerminator()) {
    ctx.builder->CreateCall(getCxaEndCatch(), {});
    BasicBlock* catchEndBB = ctx.builder->GetInsertBlock();
    ctx.builder->CreateBr(mergeBB);
    if (catchResult) results.push_back({catchResult, catchEndBB});
  }

  // ---- Merge ----
  ctx.builder->SetInsertPoint(mergeBB);

  // If both paths reach merge with matching, non-void value types, produce a
  // phi so try/catch yields a value. Otherwise it is used as a statement.
  if (results.size() == 2 &&
      results[0].first->getType() == results[1].first->getType() &&
      !results[0].first->getType()->isVoidTy()) {
    PHINode* phi = ctx.builder->CreatePHI(results[0].first->getType(), 2,
                                          "try.result");
    for (auto& [val, block] : results) phi->addIncoming(val, block);
    return phi;
  }
  if (results.size() == 1 && !results[0].first->getType()->isVoidTy()) {
    return results[0].first;
  }
  // Statement use (void try block, or divergent branch types): yield a
  // non-null dummy so the enclosing block continues generating subsequent
  // statements (block codegen aborts on a null statement value).
  return ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0);
}

// =============================================================================
// LLVM Exception Handling Helpers
// =============================================================================
// These functions declare/get the C++ ABI exception handling functions
// needed for LLVM's native exception handling mechanism.

// Get or declare: void* __cxa_allocate_exception(size_t)
FunctionCallee CodegenVisitor::getCxaAllocateException() {
  auto* i8PtrTy = PointerType::getUnqual(ctx.getContext());
  auto* i64Ty = Type::getInt64Ty(ctx.getContext());
  FunctionType* fnType = FunctionType::get(i8PtrTy, {i64Ty}, false);
  return module->getOrInsertFunction("__cxa_allocate_exception", fnType);
}

// Get or declare: void __cxa_throw(void* exception, void* tinfo, void* dest)
FunctionCallee CodegenVisitor::getCxaThrow() {
  auto* voidTy = Type::getVoidTy(ctx.getContext());
  auto* i8PtrTy = PointerType::getUnqual(ctx.getContext());
  FunctionType* fnType =
      FunctionType::get(voidTy, {i8PtrTy, i8PtrTy, i8PtrTy}, false);
  auto fn = module->getOrInsertFunction("__cxa_throw", fnType);
  // Mark __cxa_throw as noreturn
  if (auto* func = dyn_cast<Function>(fn.getCallee())) {
    func->addFnAttr(Attribute::NoReturn);
  }
  return fn;
}

// Get or declare: void* __cxa_begin_catch(void* exception)
FunctionCallee CodegenVisitor::getCxaBeginCatch() {
  auto* i8PtrTy = PointerType::getUnqual(ctx.getContext());
  FunctionType* fnType = FunctionType::get(i8PtrTy, {i8PtrTy}, false);
  return module->getOrInsertFunction("__cxa_begin_catch", fnType);
}

// Get or declare: void __cxa_end_catch()
FunctionCallee CodegenVisitor::getCxaEndCatch() {
  auto* voidTy = Type::getVoidTy(ctx.getContext());
  FunctionType* fnType = FunctionType::get(voidTy, {}, false);
  return module->getOrInsertFunction("__cxa_end_catch", fnType);
}

// Get the personality function for exception handling.
// We use __gxx_personality_v0 for C++ ABI compatibility.
Constant* CodegenVisitor::getPersonalityFunction() {
  auto* i32Ty = Type::getInt32Ty(ctx.getContext());
  auto* i64Ty = Type::getInt64Ty(ctx.getContext());
  auto* ptrTy = PointerType::getUnqual(ctx.getContext());
  // Pin a concrete (non-varargs) signature: i32(i32, i32, i64, ptr, ptr) so
  // the verifier is happy about the personality reference.
  FunctionType* fnType =
      FunctionType::get(i32Ty, {i32Ty, i32Ty, i64Ty, ptrTy, ptrTy}, false);
  auto fn = module->getOrInsertFunction("__gxx_personality_v0", fnType);
  return cast<Constant>(fn.getCallee());
}

// Get the type info used for Sun exceptions. Every Sun catch is `catch(e:
// IError)` and every throw uses this same token, and landing pads use a
// catch-all clause, so any single valid C++ RTTI symbol works here. We reuse
// libstdc++'s typeinfo for `int` (resolved from the host process at JIT time).
Constant* CodegenVisitor::getSunExceptionTypeInfo() {
  auto* ptrTy = PointerType::getUnqual(ctx.getContext());
  return cast<Constant>(module->getOrInsertGlobal("_ZTIi", ptrTy));
}

// Ensure a function has a personality function (required once it contains an
// invoke/landingpad). Idempotent.
void CodegenVisitor::ensurePersonality(Function* fn) {
  if (!fn->hasPersonalityFn()) {
    fn->setPersonalityFn(getPersonalityFunction());
  }
}

// Emit a call that may unwind. Inside a try block a throwing callee must be
// `invoke`d so the exception routes to the local landing pad; otherwise a
// plain call lets the exception propagate out of the current function.
Value* CodegenVisitor::emitPossiblyThrowingCall(FunctionType* fnTy,
                                                Value* callee,
                                                ArrayRef<Value*> args,
                                                bool canThrow,
                                                const Twine& name) {
  bool isVoid = fnTy->getReturnType()->isVoidTy();
  if (canThrow && !tryStack.empty()) {
    Function* curFn = ctx.builder->GetInsertBlock()->getParent();
    ensurePersonality(curFn);
    BasicBlock* contBB =
        BasicBlock::Create(ctx.getContext(), "invoke.cont", curFn);
    Value* inv = ctx.builder->CreateInvoke(fnTy, callee, contBB,
                                           tryStack.back().landingPad, args,
                                           isVoid ? "" : name);
    ctx.builder->SetInsertPoint(contBB);
    return inv;
  }
  return ctx.builder->CreateCall(fnTy, callee, args, isVoid ? "" : name);
}

Value* CodegenVisitor::emitPossiblyThrowingCall(FunctionCallee callee,
                                                ArrayRef<Value*> args,
                                                bool canThrow,
                                                const Twine& name) {
  return emitPossiblyThrowingCall(callee.getFunctionType(), callee.getCallee(),
                                  args, canThrow, name);
}
