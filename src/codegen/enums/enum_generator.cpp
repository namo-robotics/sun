// Enum definitions, variant construction, access, and matching.

#include "codegen/enums/enum_generator.h"

#include <set>

#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "codegen/support/scalar_ops.h"

using namespace llvm;

ScopeManager& EnumGenerator::scopes() { return gen_.scopeManager(); }

// -------------------------------------------------------------------
// Enum variant construction: EnumName.Variant(args...)
// -------------------------------------------------------------------

Value* EnumGenerator::codegenVariantConstruction(
    const CallExprAST& expr, sun::EnumType& enumType,
    const sun::EnumVariant& variant) {
  StructType* storageTy = typeResolver.getEnumStorageType(enumType);
  StructType* variantTy =
      typeResolver.getEnumVariantStruct(enumType, variant.name);

  Function* func = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* storage = gen_.createEntryBlockAlloca(
      func, enumType.getBaseName() + "." + variant.name, storageTy);

  // Store the tag (field 0 has the same offset in storage and variant view)
  Value* tagPtr =
      ctx.builder->CreateStructGEP(storageTy, storage, 0, "tag.ptr");
  ctx.builder->CreateStore(
      ConstantInt::get(Type::getInt32Ty(ctx.getContext()), variant.value),
      tagPtr);

  // Store each payload value through the variant view struct
  const auto& args = expr.getArgs();
  for (size_t i = 0; i < args.size(); ++i) {
    unsigned idx =
        typeResolver.enumPayloadFieldIndex(enumType, variant.name, i);
    llvm::Type* fieldTy = variantTy->getElementType(idx);
    Value* fieldPtr = ctx.builder->CreateStructGEP(variantTy, storage, idx,
                                                   "payload." + variant.name);
    const sun::TypePtr& payloadType = variant.payloadTypes[i];

    // A reference payload stores the referent's ADDRESS: the variant borrows,
    // it does not own, so nothing moves and nothing is dropped later.
    if (payloadType->isReference()) {
      Value* addr = gen_.tryCodegenAddress(*args[i]);
      if (!addr) {
        logAndThrowError(
            "Payload of variant '" + variant.name +
                "' is a reference, but the argument has no address to borrow",
            expr.getLocation());
      }
      ctx.builder->CreateStore(addr, fieldPtr);
      continue;
    }

    Value* argVal = gen_.codegen(*args[i]);
    if (!argVal) {
      logAndThrowError("Failed to generate payload value for variant '" +
                       variant.name + "'");
    }

    if (payloadType->isCompound()) {
      // Class or payload-enum argument arrives as a pointer. Compound values
      // are never implicitly copied: the payload MOVES into the enum. The
      // source is invalidated (zeroed / tag-poisoned) so its own drop is a
      // no-op, and its tracking entry is released — the enum owns it now.
      if (argVal->getType()->isPointerTy()) {
        argVal = gen_.applyMoveSemantics(argVal, payloadType);
      }
      // Interface payloads (fat pointers) are copyable borrowed views: a
      // variable arrives as a pointer to its fat pointer, so load it
      if (argVal->getType()->isPointerTy() && !fieldTy->isPointerTy()) {
        argVal = ctx.builder->CreateLoad(fieldTy, argVal, "payload.load");
      }
      ctx.builder->CreateStore(argVal, fieldPtr);
      continue;
    }

    // Numeric widening (sema allows widening assignability)
    if (argVal->getType() != fieldTy) {
      if (argVal->getType()->isIntegerTy() && fieldTy->isIntegerTy()) {
        argVal = sun::codegen::ops::extendInt(*ctx.builder, argVal, fieldTy,
                                              args[i]->getResolvedType());
      } else if (argVal->getType()->isFloatTy() && fieldTy->isDoubleTy()) {
        argVal = ctx.builder->CreateFPExt(argVal, fieldTy, "payload.ext");
      }
    }
    ctx.builder->CreateStore(argVal, fieldPtr);
  }

  // The fresh storage owns its payloads until moved into a variable/field
  // (the borrow checker marks that move; genLocalVar adopts the alloca).
  if (!expr.isMoved()) {
    scopes().trackClassAllocation(storage, "enum.tmp", expr.getResolvedType());
  }

  return storage;
}

// -------------------------------------------------------------------
// Enum match: switch on the tag; payload variants GEP their payloads into
// binding allocas. Payload-free enums are their integer tag, so the
// discriminant value is the switch operand directly.
// -------------------------------------------------------------------

Value* EnumGenerator::codegenMatch(const MatchExprAST& expr,
                                   sun::EnumType& enumType) {
  Value* discVal = gen_.codegen(*expr.getDiscriminant());
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
          gen_.createEntryBlockAlloca(TheFunction, "match.disc", storageTy);
      ctx.builder->CreateStore(discVal, spill);
      discPtr = spill;
    }
    Value* tagPtr =
        ctx.builder->CreateStructGEP(storageTy, discPtr, 0, "match.tag.ptr");
    tag = ctx.builder->CreateLoad(Type::getInt32Ty(ctx.getContext()), tagPtr,
                                  "match.tag");
  } else {
    tag = discVal;
    // A ref discriminant may arrive as a pointer to the tag
    if (tag->getType()->isPointerTy()) {
      tag = ctx.builder->CreateLoad(enumType.toLLVMType(ctx.getContext()), tag,
                                    "match.tag");
    }
  }

  const bool consuming = expr.getDiscriminant()->isMoved();
  if (consuming && enumType.hasPayload()) {
    StructType* storageTy = typeResolver.getEnumStorageType(enumType);
    Value* owned =
        gen_.createEntryBlockAlloca(TheFunction, "match.input", storageTy);
    ctx.builder->CreateStore(
        gen_.applyMoveSemantics(
            discPtr, sun::unwrapRef(expr.getDiscriminant()->getResolvedType())),
        owned);
    discPtr = owned;
  }
  const auto matchType = expr.getResolvedType();
  AllocaInst* resultStorage = nullptr;
  if (sun::typeMovesOnRead(matchType)) {
    resultStorage = gen_.createEntryBlockAlloca(
        TheFunction, "match.result", typeResolver.resolve(matchType));
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
  SwitchInst* switchInst = ctx.builder->CreateSwitch(tag, DefaultBB, numCases);

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
        return sun::codegen::ops::extendInt(*ctx.builder, val, resultLLVMType,
                                            arm.body->getResolvedType());
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
    // One arm of a branch: a move in here happens on this path only
    ScopeManager::BranchArm branchArm(scopes());
    scopes().push();

    if (consuming && arm.isWildcard && enumType.hasPayload()) {
      scopes().trackClassAllocation(discPtr, "match.ignored",
                                    expr.getDiscriminant()->getResolvedType());
    }

    // Bind payloads through the variant view struct
    if (!arm.isWildcard && arm.hasPayloadParens) {
      const auto& patternAccess =
          static_cast<const MemberAccessAST&>(*arm.pattern);
      StructType* variantTy = typeResolver.getEnumVariantStruct(
          enumType, patternAccess.getMemberName());
      for (size_t i = 0; i < arm.bindings.size(); ++i) {
        const auto& binding = arm.bindings[i];
        if (binding.isWildcard && !consuming) continue;
        unsigned idx = typeResolver.enumPayloadFieldIndex(
            enumType, patternAccess.getMemberName(), i);
        Value* fieldPtr = ctx.builder->CreateStructGEP(variantTy, discPtr, idx,
                                                       binding.name + ".ptr");
        llvm::Type* fieldTy = variantTy->getElementType(idx);
        if (consuming && binding.resolvedType &&
            sun::typeMovesOnRead(binding.resolvedType)) {
          const std::string name =
              binding.isWildcard ? "match.ignored" : binding.name;
          AllocaInst* alloca =
              gen_.createEntryBlockAlloca(TheFunction, name, fieldTy);
          ctx.builder->CreateStore(ctx.builder->CreateLoad(fieldTy, fieldPtr),
                                   alloca);
          if (!binding.isWildcard)
            scopes().back().variables[binding.name] = alloca;
          scopes().trackClassAllocation(alloca, name, binding.resolvedType);
        } else if (binding.isWildcard) {
          continue;
        } else if (binding.resolvedType && binding.resolvedType->isCompound()) {
          // Compound payload: bind BY POINTER (a borrow of the payload slot
          // inside the discriminant — never an implicit copy). The alloca
          // holds the slot address; reads go through the indirection.
          AllocaInst* alloca = gen_.createEntryBlockAlloca(
              TheFunction, binding.name + ".ref",
              PointerType::getUnqual(ctx.getContext()));
          ctx.builder->CreateStore(fieldPtr, alloca);
          scopes().back().variables[binding.name] = alloca;
          scopes().back().indirectBindings.insert(binding.name);
        } else {
          // Scalar payload: fresh local copy
          AllocaInst* alloca =
              gen_.createEntryBlockAlloca(TheFunction, binding.name, fieldTy);
          Value* fieldVal =
              ctx.builder->CreateLoad(fieldTy, fieldPtr, binding.name);
          ctx.builder->CreateStore(fieldVal, alloca);
          scopes().back().variables[binding.name] = alloca;
          state_.debugInfo.declareLocal(*ctx.builder, alloca, binding.name,
                                        binding.resolvedType, binding.location);
        }
      }
    }

    Value* bodyVal = gen_.codegen(*arm.body);
    bool terminated = ctx.builder->GetInsertBlock()->getTerminator() != nullptr;
    if (!terminated) {
      bodyVal = convertArmValue(bodyVal, arm);
      if (resultStorage && bodyVal && !bodyVal->getType()->isVoidTy()) {
        ctx.builder->CreateStore(gen_.applyMoveSemantics(bodyVal, matchType),
                                 resultStorage);
        bodyVal = resultStorage;
      }
    }
    scopes().pop();
    if (!terminated) {
      // Void arms (statement bodies) contribute no value to the merge
      if (bodyVal && !bodyVal->getType()->isVoidTy()) {
        armResults.push_back({bodyVal, ctx.builder->GetInsertBlock()});
      }
      ctx.builder->CreateBr(MergeBB);
    }
  };

  // Only the first reachable arm for a tag participates in the switch.
  std::set<int64_t> emittedTags;
  // Variant arms
  for (size_t i = 0; i < arms.size(); ++i) {
    const auto& arm = arms[i];
    if (arm.isWildcard) break;
    if (!emittedTags.insert(arm.resolvedVariantTag).second) continue;
    BasicBlock* ArmBB = BasicBlock::Create(
        ctx.getContext(), "match.arm." + std::to_string(i), TheFunction);
    switchInst->addCase(ConstantInt::get(cast<IntegerType>(tag->getType()),
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

  if (resultStorage) {
    scopes().trackClassAllocation(resultStorage, "match.result", matchType);
    return resultStorage;
  }

  Type* resultType = armResults[0].first->getType();
  for (const auto& [val, bb] : armResults) {
    if (val->getType() != resultType) {
      return ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0);
    }
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

// -------------------------------------------------------------------
// Variant access without arguments: EnumName.Variant
// -------------------------------------------------------------------

Value* EnumGenerator::codegenVariantAccess(sun::EnumType& enumType,
                                           const sun::EnumVariant& variant) {
  // Unit variant of a payload enum: materialize tagged storage and return
  // the pointer (compound convention). Payload variants are constructed
  // through the call path.
  if (enumType.hasPayload()) {
    StructType* storageTy = typeResolver.getEnumStorageType(enumType);
    Function* func = ctx.builder->GetInsertBlock()->getParent();
    AllocaInst* storage = gen_.createEntryBlockAlloca(
        func, enumType.getBaseName() + "." + variant.name, storageTy);
    Value* tagPtr =
        ctx.builder->CreateStructGEP(storageTy, storage, 0, "tag.ptr");
    ctx.builder->CreateStore(
        ConstantInt::get(Type::getInt32Ty(ctx.getContext()), variant.value),
        tagPtr);
    return storage;
  }
  // Payload-free enums are inline constants of the declared integer type.
  return ConstantInt::get(enumType.toLLVMType(ctx.getContext()), variant.value);
}

// -------------------------------------------------------------------
// Enum definition codegen
// -------------------------------------------------------------------

Value* EnumGenerator::codegen(const EnumDefinitionAST& expr) {
  // Enum definitions are already fully registered by the semantic analyzer
  // in the TypeRegistry. Payload-free enums are represented as integer
  // constants emitted inline when variants are referenced.

  // Generic templates generate no code themselves; walk the specializations
  // recorded by the semantic analyzer (mirrors generic classes) and build
  // their storage structs.
  if (expr.isGeneric()) {
    for (const auto& [mangledName, specialized] : expr.getSpecializations()) {
      if (specialized && specialized->hasPayload()) {
        typeResolver.getEnumStorageType(*specialized);
      }
    }
    return ConstantFP::get(ctx.getContext(), APFloat(0.0));
  }

  // Payload enums: eagerly build the storage struct so any later
  // ClassType::getStructType embedding an enum field (which cannot reach the
  // resolver) can serve it from the EnumType cache.
  if (expr.hasAnyPayload()) {
    if (auto enumType =
            state_.typeRegistry->getEnum(expr.getQualifiedName().mangled())) {
      typeResolver.getEnumStorageType(*enumType);
    }
  }

  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}
