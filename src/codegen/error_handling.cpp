// error_handling.cpp — Codegen for error handling expressions (try/catch/throw)

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include "codegen_visitor.h"

using namespace llvm;

namespace {
// Stable, module-independent type identity for an error class: FNV-1a 64-bit
// hash of its (canonical) mangled name. Computed identically at the throw site
// and every catch site, so typed catches can match without RTTI and without
// relying on per-module vtable pointer identity.
uint64_t sunTypeId(const std::string& mangledName) {
  uint64_t h = 1469598103934665603ull;  // FNV offset basis
  for (unsigned char c : mangledName) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ull;  // FNV prime
  }
  return h;
}
}  // namespace

// Safe division/modulo: check for zero divisor and throw instead of crashing.
// This is only called when currentFunctionCanError is true.
Value* CodegenVisitor::codegenSafeDivision(Value* L, Value* R, bool isModulo,
                                           bool isUnsigned) {
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
  Value* result;
  if (isModulo) {
    result = isUnsigned ? ctx.builder->CreateURem(L, R, "modtmp")
                        : ctx.builder->CreateSRem(L, R, "modtmp");
  } else {
    result = isUnsigned ? ctx.builder->CreateUDiv(L, R, "divtmp")
                        : ctx.builder->CreateSDiv(L, R, "divtmp");
  }
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
// Exception buffer layout: { i64 typeId, InterfaceFat fat, <object bytes> }
// where fat.data points at the embedded object copy so it survives unwinding,
// and typeId (FNV-1a of the concrete class's mangled name) lets typed catches
// match without RTTI. See the matching landing pad in codegen(TryCatchExprAST).
Value* CodegenVisitor::codegen(const ThrowExprAST& expr) {
  if (!currentFunctionCanError) {
    logAndThrowError(
        "throw can only be used in functions declared with ', IError'");
    return nullptr;
  }

  llvm::StructType* fatTy =
      sun::InterfaceType::getFatPointerType(ctx.getContext());
  const DataLayout& DL = module->getDataLayout();
  auto* i64Ty = Type::getInt64Ty(ctx.getContext());
  auto* i8Ty = Type::getInt8Ty(ctx.getContext());
  uint64_t idSize = DL.getTypeAllocSize(i64Ty);   // header: typeId
  uint64_t fatSize = DL.getTypeAllocSize(fatTy);  // header: fat pointer
  uint64_t fatOffset = idSize;
  uint64_t objOffset = idSize + fatSize;

  auto storeAt = [&](Value* base, uint64_t off, Value* val) {
    Value* slot = ctx.builder->CreateGEP(i8Ty, base,
                                         {ConstantInt::get(i64Ty, off)});
    ctx.builder->CreateStore(val, slot);
  };

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
        {ConstantInt::get(i64Ty, objOffset + objSize)}, "exc");
    // typeId at offset 0.
    ctx.builder->CreateStore(
        ConstantInt::get(i64Ty, sunTypeId(classType->getMangledName())), exc);
    // Object copy after the header; fat.data references it.
    Value* objSlot = ctx.builder->CreateGEP(
        i8Ty, exc, {ConstantInt::get(i64Ty, objOffset)}, "exc.obj");
    Value* objVal = ctx.builder->CreateLoad(classStruct, objPtr, "throw.obj");
    ctx.builder->CreateStore(objVal, objSlot);

    auto ierror = typeRegistry->getInterface("IError");
    Value* fat = createInterfaceFatPointer(objSlot, classType, ierror.get());
    storeAt(exc, fatOffset, fat);

    emitCxaThrowAndUnreachable(exc);
  } else {
    // Interface value (e.g. rethrow of a caught `e`) or unknown: box the fat
    // pointer as-is with typeId 0 (concrete type unknown statically → only an
    // 'IError' catch-all can re-catch it). The underlying object outlives the
    // throw (its owning exception is released only by __cxa_end_catch, which
    // the unreachable throw path skips).
    Value* fatVal = expr.hasErrorExpr() ? codegen(expr.getErrorExpr()) : nullptr;
    Value* exc = ctx.builder->CreateCall(
        getCxaAllocateException(), {ConstantInt::get(i64Ty, objOffset)}, "exc");
    ctx.builder->CreateStore(ConstantInt::get(i64Ty, 0), exc);
    Value* fatToStore;
    if (fatVal && fatVal->getType() == fatTy) {
      fatToStore = fatVal;
    } else if (fatVal && fatVal->getType()->isPointerTy()) {
      fatToStore = ctx.builder->CreateLoad(fatTy, fatVal, "throw.fat");
    } else {
      fatToStore = Constant::getNullValue(fatTy);
    }
    storeAt(exc, fatOffset, fatToStore);
    emitCxaThrowAndUnreachable(exc);
  }

  return nullptr;
}

// Codegen for unsafe block: unsafe { ... }
// Simply generates code for the body - safety checks are done at semantic analysis
Value* CodegenVisitor::codegen(const UnsafeBlockAST& expr) {
  return codegen(expr.getBody());
}

// Codegen for try-catch: try { ... } catch (e: A) { ... } catch (e: IError) {}
// Throwing calls in the try block `invoke` to a single catch-all landing pad.
// The pad enters the C++ ABI catch, reads the exception's typeId header, and
// dispatches to the first clause whose type matches (concrete class → id
// compare; `IError` → unconditional catch-all). If nothing matches, the
// exception is __cxa_rethrow'd to the nearest outer handler.
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
  auto* i64Ty = Type::getInt64Ty(ctx.getContext());
  llvm::StructType* lpadTy = StructType::get(ptrTy, i32Ty);
  llvm::LandingPadInst* lp = ctx.builder->CreateLandingPad(lpadTy, 1, "lpad");
  lp->addClause(ConstantPointerNull::get(ptrTy));  // catch(...)
  Value* excPtr = ctx.builder->CreateExtractValue(lp, 0, "exc.ptr");
  Value* obj = ctx.builder->CreateCall(getCxaBeginCatch(), {excPtr}, "exc.obj");

  // Exception header (see codegen(ThrowExprAST)): { i64 typeId, InterfaceFat }.
  llvm::StructType* fatTy =
      sun::InterfaceType::getFatPointerType(ctx.getContext());
  const DataLayout& DL = module->getDataLayout();
  uint64_t fatOffset = DL.getTypeAllocSize(i64Ty);
  Value* typeId = ctx.builder->CreateLoad(i64Ty, obj, "exc.typeId");
  Value* fatSlot = ctx.builder->CreateGEP(
      Type::getInt8Ty(ctx.getContext()), obj,
      {ConstantInt::get(i64Ty, fatOffset)}, "exc.fat.slot");
  Value* fat = ctx.builder->CreateLoad(fatTy, fatSlot, "exc.fat");

  const auto& clauses = expr.getCatchClauses();
  size_t n = clauses.size();

  // Per-clause test and body blocks; branch from the pad into the first test.
  std::vector<BasicBlock*> testBBs(n), bodyBBs(n);
  for (size_t i = 0; i < n; ++i) {
    testBBs[i] = BasicBlock::Create(ctx.getContext(), "catch.test", func);
    bodyBBs[i] = BasicBlock::Create(ctx.getContext(), "catch.body", func);
  }
  bool hasCatchAll = n > 0 && clauses.back().isCatchAll;
  BasicBlock* nomatchBB =
      hasCatchAll ? nullptr
                  : BasicBlock::Create(ctx.getContext(), "catch.nomatch", func);
  ctx.builder->CreateBr(testBBs[0]);

  for (size_t i = 0; i < n; ++i) {
    const auto& clause = clauses[i];

    // ---- test ----
    ctx.builder->SetInsertPoint(testBBs[i]);
    if (clause.isCatchAll) {
      ctx.builder->CreateBr(bodyBBs[i]);
    } else {
      Value* want = ConstantInt::get(i64Ty, sunTypeId(clause.resolvedMangledName));
      Value* m = ctx.builder->CreateICmpEQ(typeId, want, "catch.match");
      BasicBlock* elseBB = (i + 1 < n) ? testBBs[i + 1] : nomatchBB;
      ctx.builder->CreateCondBr(m, bodyBBs[i], elseBB);
    }

    // ---- body ----
    ctx.builder->SetInsertPoint(bodyBBs[i]);
    pushScope();
    if (!clause.bindingName.empty()) {
      IRBuilder<> tmpBuilder(&func->getEntryBlock(),
                             func->getEntryBlock().begin());
      if (clause.isCatchAll) {
        // Bind the IError interface fat pointer.
        AllocaInst* alloca =
            tmpBuilder.CreateAlloca(fatTy, nullptr, clause.bindingName);
        ctx.builder->CreateStore(fat, alloca);
        scopes.back().variables[clause.bindingName] = alloca;
      } else {
        // Concrete type: copy the object out of the exception buffer into a
        // fresh stack slot so it survives __cxa_end_catch, then bind e to it.
        auto classType = typeRegistry->getClass(clause.resolvedMangledName);
        llvm::StructType* classStruct =
            classType->getStructType(ctx.getContext());
        Value* dataPtr = ctx.builder->CreateExtractValue(fat, 0, "err.data");
        AllocaInst* alloca =
            tmpBuilder.CreateAlloca(classStruct, nullptr, clause.bindingName);
        Value* objVal =
            ctx.builder->CreateLoad(classStruct, dataPtr, "err.obj");
        ctx.builder->CreateStore(objVal, alloca);
        scopes.back().variables[clause.bindingName] = alloca;
      }
    }
    Value* catchResult = codegen(*clause.body);
    popScope();

    if (!ctx.builder->GetInsertBlock()->getTerminator()) {
      ctx.builder->CreateCall(getCxaEndCatch(), {});
      BasicBlock* bodyEndBB = ctx.builder->GetInsertBlock();
      ctx.builder->CreateBr(mergeBB);
      if (catchResult) results.push_back({catchResult, bodyEndBB});
    }
  }

  // No clause matched → rethrow to the nearest outer handler (the current try
  // was already popped from tryStack, so tryStack.back() is the outer one).
  if (nomatchBB) {
    ctx.builder->SetInsertPoint(nomatchBB);
    FunctionCallee rethrow = getCxaRethrow();
    if (!tryStack.empty()) {
      ensurePersonality(func);
      BasicBlock* rcont =
          BasicBlock::Create(ctx.getContext(), "rethrow.cont", func);
      ctx.builder->CreateInvoke(rethrow, rcont, tryStack.back().landingPad, {});
      ctx.builder->SetInsertPoint(rcont);
    } else {
      ctx.builder->CreateCall(rethrow, {});
    }
    ctx.builder->CreateUnreachable();
  }

  // ---- Merge ----
  ctx.builder->SetInsertPoint(mergeBB);

  // Produce a phi if every path reaching merge carries the same non-void type
  // (try/catch used as an expression); otherwise it is a statement.
  if (!results.empty()) {
    llvm::Type* t = results[0].first->getType();
    bool uniform = !t->isVoidTy();
    for (auto& [v, b] : results)
      if (v->getType() != t) uniform = false;
    if (uniform) {
      PHINode* phi = ctx.builder->CreatePHI(t, results.size(), "try.result");
      for (auto& [v, b] : results) phi->addIncoming(v, b);
      return phi;
    }
  }
  // Non-null dummy so the enclosing block keeps generating statements
  // (block codegen aborts on a null statement value).
  return ConstantInt::get(i32Ty, 0);
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

// Get or declare: void __cxa_rethrow() — rethrows the exception currently being
// handled (must be called between __cxa_begin_catch and __cxa_end_catch).
FunctionCallee CodegenVisitor::getCxaRethrow() {
  auto* voidTy = Type::getVoidTy(ctx.getContext());
  FunctionType* fnType = FunctionType::get(voidTy, {}, false);
  auto fn = module->getOrInsertFunction("__cxa_rethrow", fnType);
  if (auto* func = dyn_cast<Function>(fn.getCallee())) {
    func->addFnAttr(Attribute::NoReturn);
  }
  return fn;
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
