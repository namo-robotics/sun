// if_expressions.cpp - If expression codegen

#include "ast.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"

using namespace llvm;

Value* coerceCondToBool(CodegenContext& ctx, Value* CondV) {
  if (CondV->getType()->isIntegerTy(1)) return CondV;
  if (CondV->getType()->isFloatingPointTy()) {
    return ctx.builder->CreateFCmpONE(
        CondV, ConstantFP::get(CondV->getType(), 0.0), "ifcond");
  }
  if (CondV->getType()->isIntegerTy()) {
    return ctx.builder->CreateICmpNE(
        CondV, ConstantInt::get(CondV->getType(), 0), "ifcond");
  }
  return CondV;
}

Value* CodegenVisitor::codegen(const IfExprAST& expr) {
  Value* CondV = codegen(*expr.getCond());
  if (!CondV) return nullptr;

  CondV = coerceCondToBool(ctx, CondV);

  // A condition that folded to a constant (a _target_is check, or arithmetic
  // over one) keeps only its live side. This is required, not an
  // optimization: no optimizer runs on AOT output, and per-OS stdlib code
  // relies on the dead side's extern calls never being emitted — an emitted
  // call to a symbol another libc lacks would fail at link. The branch-arm
  // marking stays so drop-flag decisions match what the borrow checker
  // assumed for a two-armed if.
  if (auto* constCond = dyn_cast<ConstantInt>(CondV)) {
    const ExprAST* live = constCond->isZero() ? expr.getElse() : expr.getThen();
    if (!live) {
      // `if (false) { ... }` with no else: nothing to emit.
      return ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
    }
    Value* liveV = nullptr;
    {
      ScopeManager::BranchArm arm(scopes);
      scopes.push(live->getLocation());
      liveV = codegen(*live);
      scopes.pop();
    }
    if (ctx.builder->GetInsertBlock()->getTerminator() != nullptr) {
      return nullptr;  // the live side returned or threw
    }
    if (!liveV) {
      logAndThrowError("Failed to generate code for the if's live branch");
      return nullptr;
    }
    return liveV;
  }

  Function* TheFunction = ctx.builder->GetInsertBlock()->getParent();
  bool hasElse = expr.getElse() != nullptr;

  // Create blocks for the then and else cases.  Insert the 'then' block at the
  // end of the function.
  BasicBlock* ThenBB =
      BasicBlock::Create(ctx.getContext(), "then", TheFunction);
  BasicBlock* ElseBB =
      hasElse ? BasicBlock::Create(ctx.getContext(), "else") : nullptr;
  // MergeBB must be inserted into the function before creating the conditional
  // branch so LLVM properly tracks the CFG edges
  BasicBlock* MergeBB =
      BasicBlock::Create(ctx.getContext(), "ifcont", TheFunction);

  // Branch to then or else (or merge if no else)
  ctx.builder->CreateCondBr(CondV, ThenBB, hasElse ? ElseBB : MergeBB);

  // Emit then value.
  ctx.builder->SetInsertPoint(ThenBB);

  // Push scope for then block (variables declared here are local to this block)
  Value* ThenV = nullptr;
  {
    ScopeManager::BranchArm arm(scopes);
    scopes.push(expr.getThen()->getLocation());
    ThenV = codegen(*expr.getThen());
    scopes.pop();
  }

  // Check if the Then block was terminated (e.g., by a return statement)
  bool thenTerminated =
      ctx.builder->GetInsertBlock()->getTerminator() != nullptr;

  if (!thenTerminated) {
    if (!ThenV) {
      logAndThrowError("Failed to generate code for 'then' block");
      return nullptr;
    }
    ctx.builder->CreateBr(MergeBB);
  }
  // Codegen of 'Then' can change the current block, update ThenBB for the PHI.
  ThenBB = ctx.builder->GetInsertBlock();

  // Handle else block (if present)
  Value* ElseV = nullptr;
  bool elseTerminated = false;

  if (hasElse) {
    TheFunction->insert(TheFunction->end(), ElseBB);
    ctx.builder->SetInsertPoint(ElseBB);

    // Push scope for else block (variables declared here are local to this
    // block)
    {
      ScopeManager::BranchArm arm(scopes);
      scopes.push(expr.getElse()->getLocation());
      ElseV = codegen(*expr.getElse());
      scopes.pop();
    }

    elseTerminated = ctx.builder->GetInsertBlock()->getTerminator() != nullptr;

    if (!elseTerminated) {
      if (!ElseV) {
        logAndThrowError("Failed to generate code for 'else' block");
        return nullptr;
      }
      ctx.builder->CreateBr(MergeBB);
    }
    ElseBB = ctx.builder->GetInsertBlock();
  }

  // If both branches terminated (returned), we don't need the merge block
  // BUT: if there's no else, MergeBB is the target of the conditional branch's
  // false path so we can only delete it if both then and else explicitly
  // terminated
  if (thenTerminated && elseTerminated) {
    // Remove MergeBB from the function and delete it
    MergeBB->eraseFromParent();
    return nullptr;
  }

  // Set insert point to merge block (it's already in the function)
  ctx.builder->SetInsertPoint(MergeBB);

  // No else block - the if is used as a statement
  if (!hasElse) {
    // Return a dummy value to indicate the if expression completed
    // (the actual return value won't be used since if-without-else is a
    // statement)
    return ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
  }

  // If only one branch terminated, return the other's value
  if (thenTerminated || elseTerminated) {
    return thenTerminated ? ElseV : ThenV;
  }

  // Both branches have values - if types differ or are void, don't create PHI
  if (ThenV->getType() != ElseV->getType() || ThenV->getType()->isVoidTy()) {
    // If expression used as statement - return a dummy value
    return ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
  }

  // Both branches have values with matching types - create PHI node
  PHINode* PN = ctx.builder->CreatePHI(ThenV->getType(), 2, "iftmp");

  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}

Value* CodegenVisitor::codegen(const TernaryExprAST& expr) {
  Value* CondV = codegen(*expr.getCond());
  if (!CondV) return nullptr;

  CondV = coerceCondToBool(ctx, CondV);

  // Constant condition: emit only the live arm, with no blocks at all. This
  // must come before any block machinery — a module-level
  // `var X: i32 = _target_is("macos") ? 6 : 1;` folds here into the plain
  // constant a global initializer needs, where there is no current function.
  if (auto* constCond = dyn_cast<ConstantInt>(CondV)) {
    const ExprAST* live = constCond->isZero() ? expr.getElse() : expr.getThen();
    Value* liveV = nullptr;
    {
      ScopeManager::BranchArm arm(scopes);
      liveV = codegen(*live);
    }
    if (!liveV) {
      logAndThrowError("Failed to generate code for the ternary's live arm");
      return nullptr;
    }
    // Widening a constant folds without emitting instructions, so this is
    // safe at module scope too.
    return widenNumericIfNeeded(liveV, expr.getResolvedType(),
                                live->getResolvedType());
  }

  Function* TheFunction = ctx.builder->GetInsertBlock()->getParent();

  BasicBlock* ThenBB =
      BasicBlock::Create(ctx.getContext(), "ternary.then", TheFunction);
  BasicBlock* ElseBB = BasicBlock::Create(ctx.getContext(), "ternary.else");
  // MergeBB must be inserted into the function before creating the conditional
  // branch so LLVM properly tracks the CFG edges
  BasicBlock* MergeBB =
      BasicBlock::Create(ctx.getContext(), "ternary.cont", TheFunction);

  ctx.builder->CreateCondBr(CondV, ThenBB, ElseBB);

  // Unified result type of the whole expression (set by semantic analysis).
  // Branches are pure expressions, so no per-branch scope: class temporaries
  // must survive until the enclosing scope ends, past the merge.
  sun::TypePtr resultType = expr.getResolvedType();

  ctx.builder->SetInsertPoint(ThenBB);
  Value* ThenV = nullptr;
  {
    ScopeManager::BranchArm arm(scopes);
    ThenV = codegen(*expr.getThen());
  }
  bool thenTerminated =
      ctx.builder->GetInsertBlock()->getTerminator() != nullptr;
  if (!thenTerminated) {
    if (!ThenV) {
      logAndThrowError("Failed to generate code for ternary 'then' branch");
    }
    ThenV = widenNumericIfNeeded(ThenV, resultType,
                                 expr.getThen()->getResolvedType());
    ctx.builder->CreateBr(MergeBB);
  }
  ThenBB = ctx.builder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), ElseBB);
  ctx.builder->SetInsertPoint(ElseBB);
  Value* ElseV = nullptr;
  {
    ScopeManager::BranchArm arm(scopes);
    ElseV = codegen(*expr.getElse());
  }
  bool elseTerminated =
      ctx.builder->GetInsertBlock()->getTerminator() != nullptr;
  if (!elseTerminated) {
    if (!ElseV) {
      logAndThrowError("Failed to generate code for ternary 'else' branch");
    }
    ElseV = widenNumericIfNeeded(ElseV, resultType,
                                 expr.getElse()->getResolvedType());
    ctx.builder->CreateBr(MergeBB);
  }
  ElseBB = ctx.builder->GetInsertBlock();

  if (thenTerminated && elseTerminated) {
    MergeBB->eraseFromParent();
    return nullptr;
  }

  ctx.builder->SetInsertPoint(MergeBB);

  // If one branch terminated (e.g. a throw), the result is the other's value
  if (thenTerminated || elseTerminated) {
    return thenTerminated ? ElseV : ThenV;
  }

  if (ThenV->getType() != ElseV->getType()) {
    logAndThrowError("Ternary branches produce incompatible values: '" +
                         expr.getThen()->toString() + "' vs '" +
                         expr.getElse()->toString() + "'",
                     expr.getLocation());
  }

  PHINode* PN = ctx.builder->CreatePHI(ThenV->getType(), 2, "ternarytmp");
  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}
