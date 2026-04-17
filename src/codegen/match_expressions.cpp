// match_expressions.cpp - Match expression codegen

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"

using namespace llvm;

Value* CodegenVisitor::codegen(const MatchExprAST& expr) {
  // Generate code for the discriminant (value being matched)
  Value* discVal = codegen(*expr.getDiscriminant());
  if (!discVal) {
    logAndThrowError("Failed to generate code for match discriminant");
    return nullptr;
  }

  Function* TheFunction = ctx.builder->GetInsertBlock()->getParent();
  const auto& arms = expr.getArms();

  if (arms.empty()) {
    logAndThrowError("Match expression must have at least one arm");
    return nullptr;
  }

  // Create merge block (where all arms converge)
  BasicBlock* MergeBB =
      BasicBlock::Create(ctx.getContext(), "match.end", TheFunction);

  // Track arm bodies and their values for PHI node
  std::vector<std::pair<Value*, BasicBlock*>> armResults;

  // Generate code for each arm
  for (size_t i = 0; i < arms.size(); ++i) {
    const auto& arm = arms[i];
    bool isLast = (i == arms.size() - 1);

    if (arm.isWildcard) {
      // Wildcard arm: always matches, no condition needed
      // Just generate the body and branch to merge
      Value* bodyVal = codegen(*arm.body);

      // Check if this block was terminated (e.g., by return)
      bool terminated =
          ctx.builder->GetInsertBlock()->getTerminator() != nullptr;

      if (!terminated) {
        if (bodyVal) {
          armResults.push_back({bodyVal, ctx.builder->GetInsertBlock()});
        }
        ctx.builder->CreateBr(MergeBB);
      }
      // After wildcard, no more arms can be reached
      break;
    }

    // Generate code for the pattern (should be a constant or literal)
    Value* patternVal = codegen(*arm.pattern);
    if (!patternVal) {
      logAndThrowError("Failed to generate code for match pattern");
      return nullptr;
    }

    // Create blocks for this arm
    BasicBlock* ArmBB = BasicBlock::Create(
        ctx.getContext(), "match.arm." + std::to_string(i), TheFunction);
    BasicBlock* NextBB = nullptr;

    if (!isLast) {
      NextBB = BasicBlock::Create(ctx.getContext(),
                                  "match.next." + std::to_string(i + 1));
    } else {
      // Last arm without wildcard - if doesn't match, go to merge with undef
      NextBB = MergeBB;
    }

    // Generate equality comparison
    Value* cmp = nullptr;
    if (discVal->getType()->isIntegerTy() &&
        patternVal->getType()->isIntegerTy()) {
      // Integer comparison
      // Handle potential type mismatch (i32 vs i64, etc.)
      if (discVal->getType() != patternVal->getType()) {
        // Extend the smaller type to match the larger one
        unsigned discBits = discVal->getType()->getIntegerBitWidth();
        unsigned patBits = patternVal->getType()->getIntegerBitWidth();
        if (discBits < patBits) {
          discVal = ctx.builder->CreateSExt(discVal, patternVal->getType(),
                                            "disc.ext");
        } else {
          patternVal = ctx.builder->CreateSExt(patternVal, discVal->getType(),
                                               "pat.ext");
        }
      }
      cmp = ctx.builder->CreateICmpEQ(discVal, patternVal, "match.cmp");
    } else if (discVal->getType()->isFloatingPointTy() &&
               patternVal->getType()->isFloatingPointTy()) {
      // Float comparison
      // Handle potential type mismatch (f32 vs f64)
      if (discVal->getType() != patternVal->getType()) {
        if (discVal->getType()->isFloatTy()) {
          discVal = ctx.builder->CreateFPExt(discVal, patternVal->getType(),
                                             "disc.ext");
        } else {
          patternVal = ctx.builder->CreateFPExt(patternVal, discVal->getType(),
                                                "pat.ext");
        }
      }
      cmp = ctx.builder->CreateFCmpOEQ(discVal, patternVal, "match.cmp");
    } else if (discVal->getType()->isPointerTy() &&
               patternVal->getType()->isPointerTy()) {
      // Pointer comparison (e.g., for strings - compare addresses)
      cmp = ctx.builder->CreateICmpEQ(discVal, patternVal, "match.cmp");
    } else {
      logAndThrowError(
          "Match expression: unsupported type for pattern matching");
      return nullptr;
    }

    // Branch: if pattern matches go to ArmBB, else go to NextBB
    ctx.builder->CreateCondBr(cmp, ArmBB, NextBB);

    // Generate arm body
    ctx.builder->SetInsertPoint(ArmBB);
    Value* bodyVal = codegen(*arm.body);

    // Check if this block was terminated (e.g., by return)
    bool terminated = ctx.builder->GetInsertBlock()->getTerminator() != nullptr;

    if (!terminated) {
      if (bodyVal) {
        armResults.push_back({bodyVal, ctx.builder->GetInsertBlock()});
      }
      ctx.builder->CreateBr(MergeBB);
    }

    // Set up for next arm
    if (!isLast && NextBB != MergeBB) {
      TheFunction->insert(TheFunction->end(), NextBB);
      ctx.builder->SetInsertPoint(NextBB);
    }
  }

  // Set insert point to merge block
  ctx.builder->SetInsertPoint(MergeBB);

  // If all arms terminated (e.g., all return), merge block is unreachable
  if (armResults.empty()) {
    // Add unreachable instruction if no arm reached merge
    // But MergeBB may still have incoming edges from failed matches
    // Return a dummy value
    return ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
  }

  // If only one arm produced a value, return it directly
  if (armResults.size() == 1) {
    return armResults[0].first;
  }

  // Multiple arms with values - create PHI node
  // First, verify all values have the same type
  Type* resultType = armResults[0].first->getType();
  for (const auto& [val, bb] : armResults) {
    if (val->getType() != resultType) {
      // Type mismatch - return void/i32 as fallback
      return ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
    }
  }

  PHINode* PN =
      ctx.builder->CreatePHI(resultType, armResults.size(), "match.result");
  for (const auto& [val, bb] : armResults) {
    PN->addIncoming(val, bb);
  }

  // Add undef incoming values for any predecessor of MergeBB not covered by
  // armResults (e.g., fallthrough from last non-wildcard arm's comparison)
  for (auto it = llvm::pred_begin(MergeBB), et = llvm::pred_end(MergeBB);
       it != et; ++it) {
    if (PN->getBasicBlockIndex(*it) == -1) {
      PN->addIncoming(UndefValue::get(resultType), *it);
    }
  }

  return PN;
}
