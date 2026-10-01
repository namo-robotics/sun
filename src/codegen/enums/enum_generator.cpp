// Enum definitions, variant construction, access, and matching.

#include "codegen/enums/enum_generator.h"

#include <set>

#include "ast/control_flow.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "codegen/support/scalar_ops.h"
#include "semantic_analysis/type_registry.h"

using sun::types::EnumType;

using sun::ast::MatchArm;
using sun::support::logAndThrowError;

using namespace llvm;

/** Generates enum representations and operations on enum values. */
namespace sun::codegen::enums {

sun::codegen::scopes::ScopeManager& EnumGenerator::scopes() {
  return gen_.scopeManager();
}

// -------------------------------------------------------------------
// Enum variant construction: EnumName.Variant(args...)
// -------------------------------------------------------------------

Value* EnumGenerator::codegenVariantConstruction(
    const sun::ast::CallExprAST& expr, EnumType& enumType,
    const sun::types::EnumVariant& variant) {
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
      ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), variant.value),
      tagPtr);

  // Store each payload value through the variant view struct
  const auto& args = expr.getArgs();
  std::vector<std::pair<Value*, sun::types::TypePtr>> pendingPayloads;
  for (size_t i = 0; i < args.size(); ++i) {
    unsigned idx =
        typeResolver.enumPayloadFieldIndex(enumType, variant.name, i);
    llvm::Type* fieldTy = variantTy->getElementType(idx);
    Value* fieldPtr = ctx.builder->CreateStructGEP(variantTy, storage, idx,
                                                   "payload." + variant.name);
    const sun::types::TypePtr& payloadType = variant.payloadTypes[i];

    // A reference payload stores an address or an inline array/interface view.
    // The variant borrows its storage, so it never moves or drops the referent.
    if (payloadType->isReference()) {
      Value* addr = gen_.tryCodegenAddress(*args[i]);
      if (!addr) {
        logAndThrowError(
            "Payload of variant '" + variant.name +
                "' is a reference, but the argument has no address to borrow",
            expr.getLocation());
      }
      auto target = sun::types::unwrapRef(payloadType);
      if (auto* array = dynamic_cast<sun::types::ArrayType*>(target.get());
          array && array->isUnsized()) {
        auto source = sun::types::unwrapRef(args[i]->getResolvedType());
        auto* sourceArray = static_cast<sun::types::ArrayType*>(source.get());
        addr = sourceArray->isUnsized()
                   ? gen_.loadArrayView(addr)
                   : gen_.emitArrayView(addr, sourceArray->getDimensions());
      }
      if (target->isInterface()) {
        auto source = sun::types::unwrapRef(args[i]->getResolvedType());
        auto* iface = static_cast<sun::types::InterfaceType*>(target.get());
        if (source->isClass()) {
          addr = gen_.classGenerator().createInterfaceFatPointer(
              addr, static_cast<sun::types::ClassType*>(source.get()), iface);
        } else {
          addr = gen_.classGenerator().upcastInterface(
              addr, static_cast<sun::types::InterfaceType*>(source.get()),
              iface);
        }
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
      if (i + 1 < args.size() && sun::types::typeNeedsDrop(payloadType)) {
        scopes().trackClassAllocation(fieldPtr, "pending.payload", payloadType);
        pendingPayloads.emplace_back(fieldPtr, payloadType);
      }
      continue;
    }

    // Numeric widening (sema allows widening assignability)
    if (argVal->getType() != fieldTy) {
      if (argVal->getType()->isIntegerTy() && fieldTy->isIntegerTy()) {
        argVal = sun::codegen::support::extendInt(*ctx.builder, argVal, fieldTy,
                                                  args[i]->getResolvedType());
      } else if (argVal->getType()->isFloatTy() && fieldTy->isDoubleTy()) {
        argVal = ctx.builder->CreateFPExt(argVal, fieldTy, "payload.ext");
      }
    }
    ctx.builder->CreateStore(argVal, fieldPtr);
  }

  for (const auto& [address, type] : pendingPayloads)
    scopes().markClassAllocationAsDeinited(address, type);

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

Value* EnumGenerator::codegenMatch(const sun::ast::MatchExprAST& expr,
                                   EnumType& enumType) {
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
    tag = ctx.builder->CreateLoad(llvm::Type::getInt32Ty(ctx.getContext()),
                                  tagPtr, "match.tag");
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
            discPtr,
            sun::types::unwrapRef(expr.getDiscriminant()->getResolvedType())),
        owned);
    discPtr = owned;
  }
  const auto matchType = expr.getResolvedType();
  AllocaInst* resultStorage = nullptr;
  if (sun::types::typeMovesOnRead(matchType)) {
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
  if (sun::types::TypePtr matchType = expr.getResolvedType()) {
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
        return sun::codegen::support::extendInt(
            *ctx.builder, val, resultLLVMType, arm.body->getResolvedType());
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

  auto emitArmBody = [&](const MatchArm& arm, BasicBlock* armBB,
                         const std::string& variantName) {
    ctx.builder->SetInsertPoint(armBB);
    scopes().push();

    if (consuming && (arm.isWildcard || arm.bindingType) &&
        enumType.hasPayload()) {
      scopes().trackClassAllocation(discPtr, "match.ignored",
                                    expr.getDiscriminant()->getResolvedType());
    }

    // Bind payloads through the variant view struct
    if (!arm.isWildcard && arm.hasPayloadParens) {
      StructType* variantTy =
          typeResolver.getEnumVariantStruct(enumType, variantName);
      for (size_t i = 0; i < arm.bindings.size(); ++i) {
        const auto& binding = arm.bindings[i];
        if (binding.isWildcard && !consuming) continue;
        unsigned idx =
            typeResolver.enumPayloadFieldIndex(enumType, variantName, i);
        Value* fieldPtr = ctx.builder->CreateStructGEP(variantTy, discPtr, idx,
                                                       binding.name + ".ptr");
        llvm::Type* fieldTy = variantTy->getElementType(idx);
        auto payload = enumType.getVariant(variantName)->payloadTypes[i];
        if (arm.bindingType ||
            (payload->isReference() &&
             sun::types::unwrapRef(payload)->isInterface())) {
          if (binding.isWildcard) continue;
          Value* address = fieldPtr;
          auto source = sun::types::unwrapRef(payload);
          auto target = sun::types::unwrapRef(binding.resolvedType);
          if (payload->isReference() && !source->isInterface())
            address = ctx.builder->CreateLoad(fieldTy, fieldPtr);
          if (target->isInterface() && source->isClass()) {
            auto* view = gen_.classGenerator().createInterfaceFatPointer(
                address, static_cast<sun::types::ClassType*>(source.get()),
                static_cast<sun::types::InterfaceType*>(target.get()));
            auto* storage = gen_.createEntryBlockAlloca(
                TheFunction, "match.interface", view->getType());
            ctx.builder->CreateStore(view, storage);
            address = storage;
          }
          if (target->isInterface() && source->isInterface() &&
              !source->equals(*target)) {
            auto* view = gen_.classGenerator().upcastInterface(
                address, static_cast<sun::types::InterfaceType*>(source.get()),
                static_cast<sun::types::InterfaceType*>(target.get()));
            auto* storage = gen_.createEntryBlockAlloca(
                TheFunction, "match.parent", view->getType());
            ctx.builder->CreateStore(view, storage);
            address = storage;
          }
          auto* local = gen_.createEntryBlockAlloca(
              TheFunction, binding.name,
              PointerType::getUnqual(ctx.getContext()));
          ctx.builder->CreateStore(address, local);
          scopes().back().variables[binding.declaration.id] = local;
        } else if (consuming && binding.resolvedType &&
                   sun::types::typeMovesOnRead(binding.resolvedType)) {
          const std::string name =
              binding.isWildcard ? "match.ignored" : binding.name;
          AllocaInst* alloca =
              gen_.createEntryBlockAlloca(TheFunction, name, fieldTy);
          ctx.builder->CreateStore(ctx.builder->CreateLoad(fieldTy, fieldPtr),
                                   alloca);
          if (!binding.isWildcard)
            scopes().back().variables[binding.declaration.id] = alloca;
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
          scopes().back().variables[binding.declaration.id] = alloca;
          scopes().back().indirectBindings.insert(binding.declaration.id);
        } else {
          // Scalar payload: fresh local copy
          AllocaInst* alloca =
              gen_.createEntryBlockAlloca(TheFunction, binding.name, fieldTy);
          Value* fieldVal =
              ctx.builder->CreateLoad(fieldTy, fieldPtr, binding.name);
          ctx.builder->CreateStore(fieldVal, alloca);
          scopes().back().variables[binding.declaration.id] = alloca;
          state_.debugInfo.declareLocal(*ctx.builder, alloca, binding.name,
                                        binding.resolvedType, binding.location);
        }
      }
    }

    Value* bodyVal = matchType && matchType->isReference() &&
                             !sun::ast::exprDiverges(*arm.body)
                         ? gen_.tryCodegenAddress(*arm.body)
                         : gen_.codegen(*arm.body);
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
    const auto tags = arm.bindingType
                          ? arm.matchedVariantTags
                          : std::vector<int64_t>{arm.resolvedVariantTag};
    for (auto variantTag : tags) {
      if (!emittedTags.insert(variantTag).second) continue;
      const sun::types::EnumVariant* variant = nullptr;
      for (const auto& candidate : enumType.getVariants())
        if (candidate.value == variantTag) variant = &candidate;
      BasicBlock* ArmBB = BasicBlock::Create(
          ctx.getContext(), "match.arm." + std::to_string(i), TheFunction);
      switchInst->addCase(
          ConstantInt::get(cast<IntegerType>(tag->getType()), variantTag),
          ArmBB);
      emitArmBody(arm, ArmBB, variant->name);
    }
  }

  // Default block
  if (wildcardArm) {
    emitArmBody(*wildcardArm, DefaultBB, "");
  } else {
    // Sema proved exhaustiveness; an unknown tag is memory corruption
    ctx.builder->SetInsertPoint(DefaultBB);
    ctx.builder->CreateUnreachable();
  }

  ctx.builder->SetInsertPoint(MergeBB);

  if (armResults.empty()) {
    // All arms terminated (e.g. returned); merge block is unreachable
    return ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
  }

  if (resultStorage) {
    scopes().trackClassAllocation(resultStorage, "match.result", matchType);
    return resultStorage;
  }

  llvm::Type* resultType = armResults[0].first->getType();
  for (const auto& [val, bb] : armResults) {
    if (val->getType() != resultType) {
      return ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
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

Value* EnumGenerator::codegenVariantAccess(
    EnumType& enumType, const sun::types::EnumVariant& variant) {
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
        ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()),
                         variant.value),
        tagPtr);
    return storage;
  }
  // Payload-free enums are inline constants of the declared integer type.
  return ConstantInt::get(enumType.toLLVMType(ctx.getContext()), variant.value);
}

// -------------------------------------------------------------------
// Enum definition codegen
// -------------------------------------------------------------------

Value* EnumGenerator::codegen(const sun::ast::EnumDefinitionAST& expr) {
  // Enum definitions are already fully registered by the semantic analyzer
  // in the TypeRegistry. Payload-free enums are represented as integer
  // constants emitted inline when variants are referenced.

  // Generic templates generate no code themselves; walk the specializations
  // recorded by the semantic analyzer (mirrors generic classes) and build
  // their storage structs.
  if (expr.isGeneric()) {
    for (const auto& [instanceId, specialized] : expr.getSpecializations()) {
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
    if (auto enumType = state_.typeRegistry->getEnum(expr.getDeclarationId())) {
      typeResolver.getEnumStorageType(*enumType);
    }
  }

  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}

}  // namespace sun::codegen::enums
