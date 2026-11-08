#include "abi/lowering_descriptors.h"
// Unsafe scopes and call cleanup.

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include <optional>
#include <string>

#include "codegen/codegen_visitor.h"
#include "codegen/errors/error_generator.h"

using namespace llvm;

/** Generates unsafe scopes and call cleanup. */
namespace sun::codegen::errors {

// Codegen for unsafe block: unsafe { ... }
// The body is a scope like any other block body, so names declared inside stay
// inside and owners are dropped on the way out. The safety checks themselves
// are done in semantic analysis.
//
// The block's value is always handed out to the enclosing scope, under the
// name it was tracked with (which is what its drop blocks are labelled with).
// A consumer such as `var s = unsafe { make(); };` adopts the slot from
// there; a discarded value is dropped when the enclosing scope ends, exactly
// like any other unconsumed call temporary.
Value* ErrorGenerator::codegen(const sun::ast::UnsafeBlockAST& expr) {
  scopes().push(expr.getBody().getLocation());
  Value* result = codegen(expr.getBody());
  std::optional<std::string> name = scopes().releaseBlockResult(result);
  scopes().pop();
  if (name) {
    scopes().trackClassAllocation(result, *name, expr.getResolvedType());
  }
  return result;
}

// =============================================================================
// LLVM Exception Handling Helpers
// =============================================================================
// These functions declare/get the C++ ABI exception handling functions
// needed for LLVM's native exception handling mechanism.

// Get the personality function for exception handling.
// We use __gxx_personality_v0 for C++ ABI compatibility.
Constant* ErrorGenerator::getPersonalityFunction() {
  auto* i32Ty = llvm::Type::getInt32Ty(ctx.getContext());
  auto* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  auto* ptrTy = PointerType::getUnqual(ctx.getContext());
  // Pin a concrete (non-varargs) signature: i32(i32, i32, i64, ptr, ptr) so
  // the verifier is happy about the personality reference.
  llvm::FunctionType* fnType = llvm::FunctionType::get(
      i32Ty, {i32Ty, i32Ty, i64Ty, ptrTy, ptrTy}, false);
  auto fn = module->getOrInsertFunction("__gxx_personality_v0", fnType);
  return cast<Constant>(fn.getCallee());
}

// Ensure a function has a personality function (required once it contains an
// invoke/landingpad). Idempotent.
void ErrorGenerator::ensurePersonality(Function* fn) {
  if (!fn->hasPersonalityFn()) {
    fn->setPersonalityFn(getPersonalityFunction());
  }
}

// A call that can unwind needs a cleanup pad when live owners would be skipped.
// Drop those owners and resume unwinding; Sun does not install catch handlers.
// Each pad records the owners that are live at its particular call site.
Value* ErrorGenerator::emitPossiblyThrowingCall(llvm::FunctionType* fnTy,
                                                Value* callee,
                                                ArrayRef<Value*> args,
                                                bool canThrow,
                                                const Twine& name) {
  const auto descriptor = sun::abi::CallDescriptor::forCall(fnTy, callee);
  /** Applies the same ABI to ordinary calls and calls with cleanup edges. */
  auto finish = [&](llvm::CallBase* call) -> llvm::Value* {
    descriptor.apply(*call);
    return call;
  };
  bool isVoid = fnTy->getReturnType()->isVoidTy();
  if (!canThrow) {
    return finish(
        ctx.builder->CreateCall(fnTy, callee, args, isVoid ? "" : name));
  }

  size_t cleanupDepth = scopes().functionBoundaryDepth();
  bool needsCleanup = scopes().hasLiveOwners(cleanupDepth);

  if (!needsCleanup) {
    // Nothing to clean: let the exception unwind straight into the caller.
    return finish(
        ctx.builder->CreateCall(fnTy, callee, args, isVoid ? "" : name));
  }

  Function* curFn = ctx.builder->GetInsertBlock()->getParent();
  ensurePersonality(curFn);
  BasicBlock* contBB =
      BasicBlock::Create(ctx.getContext(), "invoke.cont", curFn);

  BasicBlock* unwindDest;
  {
    // Build the per-call-site cleanup pad.
    auto* ptrTy = PointerType::getUnqual(ctx.getContext());
    auto* i32Ty = llvm::Type::getInt32Ty(ctx.getContext());
    llvm::StructType* lpadTy = StructType::get(ptrTy, i32Ty);

    BasicBlock* padBB =
        BasicBlock::Create(ctx.getContext(), "cleanup.pad", curFn);
    sun::codegen::CodegenState::InsertPointGuard here(state_);
    ctx.builder->SetInsertPoint(padBB);
    llvm::LandingPadInst* lp =
        ctx.builder->CreateLandingPad(lpadTy, 1, "cleanup.lp");
    // Drop owners, then continue unwinding to the caller.
    lp->setCleanup(true);
    scopes().emitCleanupToDepth(cleanupDepth, /*unwinding=*/true);
    ctx.builder->CreateResume(lp);
    unwindDest = padBB;
  }

  Value* inv = finish(ctx.builder->CreateInvoke(
      fnTy, callee, contBB, unwindDest, args, isVoid ? "" : name));
  ctx.builder->SetInsertPoint(contBB);
  return inv;
}

Value* ErrorGenerator::emitPossiblyThrowingCall(FunctionCallee callee,
                                                ArrayRef<Value*> args,
                                                bool canThrow,
                                                const Twine& name) {
  return emitPossiblyThrowingCall(callee.getFunctionType(), callee.getCallee(),
                                  args, canThrow, name);
}

}  // namespace sun::codegen::errors
