// if_expressions.cpp - If expression codegen

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"

using namespace llvm;

// Convert a condition value to i1 if not already (non-zero test for
// numeric conditions)
static Value* coerceCondToBool(CodegenContext& ctx, Value* CondV) {
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
  pushScope(expr.getThen()->getLocation());
  Value* ThenV = codegen(*expr.getThen());
  popScope();

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
    pushScope(expr.getElse()->getLocation());
    ElseV = codegen(*expr.getElse());
    popScope();

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
  Value* ThenV = codegen(*expr.getThen());
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
  Value* ElseV = codegen(*expr.getElse());
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
