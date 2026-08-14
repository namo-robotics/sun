// match_expressions.cpp - Match expression codegen

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"

using namespace llvm;

Value* CodegenVisitor::codegen(const MatchExprAST& expr) {
  // All enum matches lower as a tag switch (sema guarantees variant patterns
  // and exhaustiveness); the equality chain below handles scalar
  // discriminants (ints, floats, strings).
  sun::TypePtr discType =
      sun::unwrapRef(expr.getDiscriminant()->getResolvedType());
  if (discType && discType->isEnum()) {
    return codegenEnumMatch(expr, static_cast<sun::EnumType&>(*discType));
  }

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
          discVal = extendInt(discVal, patternVal->getType(),
                              expr.getDiscriminant()->getResolvedType());
        } else {
          patternVal = extendInt(patternVal, discVal->getType(),
                                 arm.pattern->getResolvedType());
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

// -------------------------------------------------------------------
// Enum match: switch on the tag; payload variants GEP their payloads into
// binding allocas. Payload-free enums ARE their tag (bare i32), so the
// discriminant value is the switch operand directly.
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenEnumMatch(const MatchExprAST& expr,
                                        sun::EnumType& enumType) {
  Value* discVal = codegen(*expr.getDiscriminant());
  if (!discVal) {
    logAndThrowError("Failed to generate code for match discriminant");
  }

  Function* TheFunction = ctx.builder->GetInsertBlock()->getParent();
  const auto& arms = expr.getArms();

  Value* discPtr = nullptr;  // storage pointer (payload enums only)
  Value* tag = nullptr;
  if (enumType.hasPayload()) {
    StructType* storageTy = typeResolver.getEnumStorageType(enumType);
    discPtr = discVal;
    // Compounds flow as pointers; spill defensively if a struct value arrives
    if (!discPtr->getType()->isPointerTy()) {
      AllocaInst* spill =
          createEntryBlockAlloca(TheFunction, "match.disc", storageTy);
      ctx.builder->CreateStore(discVal, spill);
      discPtr = spill;
    }
    Value* tagPtr =
        ctx.builder->CreateStructGEP(storageTy, discPtr, 0, "match.tag.ptr");
    tag = ctx.builder->CreateLoad(Type::getInt32Ty(ctx.getContext()), tagPtr,
                                  "match.tag");
  } else {
    tag = discVal;
    // A ref discriminant may arrive as a pointer to the i32
    if (tag->getType()->isPointerTy()) {
      tag = ctx.builder->CreateLoad(Type::getInt32Ty(ctx.getContext()), tag,
                                    "match.tag");
    }
  }

  BasicBlock* MergeBB =
      BasicBlock::Create(ctx.getContext(), "match.end", TheFunction);

  // Default block: the wildcard arm, or unreachable when exhaustive
  const MatchArm* wildcardArm = nullptr;
  for (const auto& arm : arms) {
    if (arm.isWildcard) {
      wildcardArm = &arm;
      break;
    }
  }
  BasicBlock* DefaultBB = BasicBlock::Create(
      ctx.getContext(), wildcardArm ? "match.wild" : "match.unreachable",
      TheFunction);

  size_t numCases = arms.size() - (wildcardArm ? 1 : 0);
  SwitchInst* switchInst =
      ctx.builder->CreateSwitch(tag, DefaultBB, numCases);

  std::vector<std::pair<Value*, BasicBlock*>> armResults;

  // Target LLVM type for arm results (from the match's resolved type), so
  // arms mixing integer widths (e.g. an i64 binding and literal 0) converge
  // on one PHI type instead of hitting the type-mismatch fallback.
  llvm::Type* resultLLVMType = nullptr;
  if (sun::TypePtr matchType = expr.getResolvedType()) {
    if (!matchType->isVoid() && !matchType->isCompound()) {
      resultLLVMType = typeResolver.resolve(matchType);
    }
  }

  auto convertArmValue = [&](Value* val, const MatchArm& arm) -> Value* {
    if (!val || !resultLLVMType || val->getType() == resultLLVMType) {
      return val;
    }
    llvm::Type* from = val->getType();
    if (from->isIntegerTy() && resultLLVMType->isIntegerTy()) {
      if (from->getIntegerBitWidth() < resultLLVMType->getIntegerBitWidth()) {
        return extendInt(val, resultLLVMType, arm.body->getResolvedType());
      }
      return ctx.builder->CreateTrunc(val, resultLLVMType, "arm.trunc");
    }
    if (from->isFloatTy() && resultLLVMType->isDoubleTy()) {
      return ctx.builder->CreateFPExt(val, resultLLVMType, "arm.ext");
    }
    if (from->isIntegerTy() && resultLLVMType->isFloatingPointTy()) {
      return ctx.builder->CreateSIToFP(val, resultLLVMType, "arm.tofp");
    }
    return val;
  };

  auto emitArmBody = [&](const MatchArm& arm, BasicBlock* armBB) {
    ctx.builder->SetInsertPoint(armBB);
    pushScope();

    // Bind payloads through the variant view struct
    if (!arm.isWildcard && arm.hasPayloadParens) {
      const auto& patternAccess =
          static_cast<const MemberAccessAST&>(*arm.pattern);
      StructType* variantTy = typeResolver.getEnumVariantStruct(
          enumType, patternAccess.getMemberName());
      for (size_t i = 0; i < arm.bindings.size(); ++i) {
        const auto& binding = arm.bindings[i];
        if (binding.isWildcard) continue;
        Value* fieldPtr = ctx.builder->CreateStructGEP(
            variantTy, discPtr, i + 1, binding.name + ".ptr");
        llvm::Type* fieldTy = variantTy->getElementType(i + 1);
        // Fresh local copy of the payload (never aliases the discriminant)
        AllocaInst* alloca =
            createEntryBlockAlloca(TheFunction, binding.name, fieldTy);
        Value* fieldVal =
            ctx.builder->CreateLoad(fieldTy, fieldPtr, binding.name);
        ctx.builder->CreateStore(fieldVal, alloca);
        scopes.back().variables[binding.name] = alloca;
        debugDeclareLocal(alloca, binding.name, binding.resolvedType,
                          binding.location);
      }
    }

    Value* bodyVal = codegen(*arm.body);
    bool terminated =
        ctx.builder->GetInsertBlock()->getTerminator() != nullptr;
    if (!terminated) {
      bodyVal = convertArmValue(bodyVal, arm);
    }
    popScope();
    if (!terminated) {
      if (bodyVal) {
        armResults.push_back({bodyVal, ctx.builder->GetInsertBlock()});
      }
      ctx.builder->CreateBr(MergeBB);
    }
  };

  // Variant arms
  for (size_t i = 0; i < arms.size(); ++i) {
    const auto& arm = arms[i];
    if (arm.isWildcard) continue;
    BasicBlock* ArmBB = BasicBlock::Create(
        ctx.getContext(), "match.arm." + std::to_string(i), TheFunction);
    switchInst->addCase(
        ConstantInt::get(Type::getInt32Ty(ctx.getContext()),
                         arm.resolvedVariantTag),
        ArmBB);
    emitArmBody(arm, ArmBB);
  }

  // Default block
  if (wildcardArm) {
    emitArmBody(*wildcardArm, DefaultBB);
  } else {
    // Sema proved exhaustiveness; an unknown tag is memory corruption
    ctx.builder->SetInsertPoint(DefaultBB);
    ctx.builder->CreateUnreachable();
  }

  ctx.builder->SetInsertPoint(MergeBB);

  if (armResults.empty()) {
    // All arms terminated (e.g. returned); merge block is unreachable
    return ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0);
  }

  Type* resultType = armResults[0].first->getType();
  for (const auto& [val, bb] : armResults) {
    if (val->getType() != resultType) {
      return ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0);
    }
  }

  if (armResults.size() == 1) {
    // Single value-producing arm: still emit a PHI, because MergeBB can have
    // other (terminated-arm) predecessors in general
    PHINode* PN = ctx.builder->CreatePHI(resultType, 1, "match.result");
    PN->addIncoming(armResults[0].first, armResults[0].second);
    for (auto it = llvm::pred_begin(MergeBB), et = llvm::pred_end(MergeBB);
         it != et; ++it) {
      if (PN->getBasicBlockIndex(*it) == -1) {
        PN->addIncoming(UndefValue::get(resultType), *it);
      }
    }
    return PN;
  }

  PHINode* PN =
      ctx.builder->CreatePHI(resultType, armResults.size(), "match.result");
  for (const auto& [val, bb] : armResults) {
    PN->addIncoming(val, bb);
  }
  for (auto it = llvm::pred_begin(MergeBB), et = llvm::pred_end(MergeBB);
       it != et; ++it) {
    if (PN->getBasicBlockIndex(*it) == -1) {
      PN->addIncoming(UndefValue::get(resultType), *it);
    }
  }
  return PN;
}
