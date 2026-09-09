// Cleanup for owning enum payloads.

#include "codegen/codegen_visitor.h"
#include "codegen/enums/enum_generator.h"

using namespace llvm;

// -------------------------------------------------------------------
// Enum drop glue: void __sun_enum_drop$<Enum>(ptr storage)
// Switches on the tag, drops each owning payload (class deinit + field
// recursion, or a nested enum's drop function), then poisons the tag with -1
// so a second drop falls through the switch as a no-op.
// -------------------------------------------------------------------

Function* EnumGenerator::getOrCreateDropFunction(sun::EnumType& enumType) {
  if (!sun::typeNeedsDrop(&enumType)) return nullptr;

  std::string name = "__sun_enum_drop$" + enumType.getName();
  if (Function* existing = state_.module->getFunction(name)) return existing;

  auto* voidTy = llvm::Type::getVoidTy(ctx.getContext());
  auto* ptrTy = PointerType::getUnqual(ctx.getContext());
  auto* i32Ty = llvm::Type::getInt32Ty(ctx.getContext());
  FunctionType* fnTy = FunctionType::get(voidTy, {ptrTy}, false);
  // LinkOnceODR: the same specialization may be emitted by several modules
  // (main program + .moon bundles); identical bodies merge at link/JIT time.
  Function* fn =
      Function::Create(fnTy, Function::LinkOnceODRLinkage, name, state_.module);

  CodegenState::InsertPointGuard here(state_);
  BasicBlock* entry = BasicBlock::Create(ctx.getContext(), "entry", fn);
  ctx.builder->SetInsertPoint(entry);
  Value* storage = fn->getArg(0);

  StructType* storageTy = typeResolver.getEnumStorageType(enumType);
  Value* tagPtr =
      ctx.builder->CreateStructGEP(storageTy, storage, 0, "drop.tag.ptr");
  Value* tag = ctx.builder->CreateLoad(i32Ty, tagPtr, "drop.tag");

  BasicBlock* doneBB = BasicBlock::Create(ctx.getContext(), "drop.done", fn);
  SwitchInst* sw = ctx.builder->CreateSwitch(tag, doneBB);

  for (const auto& variant : enumType.getVariants()) {
    if (!variant.hasPayload()) continue;
    bool owns = false;
    for (const auto& pt : variant.payloadTypes) {
      if (pt && sun::typeNeedsDrop(pt)) {
        owns = true;
        break;
      }
    }
    if (!owns) continue;

    BasicBlock* caseBB =
        BasicBlock::Create(ctx.getContext(), "drop." + variant.name, fn);
    sw->addCase(ConstantInt::get(i32Ty, variant.value), caseBB);
    ctx.builder->SetInsertPoint(caseBB);

    StructType* variantTy =
        typeResolver.getEnumVariantStruct(enumType, variant.name);
    for (size_t i = 0; i < variant.payloadTypes.size(); ++i) {
      const sun::TypePtr& pt = variant.payloadTypes[i];
      if (!pt || !sun::typeNeedsDrop(pt)) continue;
      unsigned idx =
          typeResolver.enumPayloadFieldIndex(enumType, variant.name, i);
      Value* fieldPtr = ctx.builder->CreateStructGEP(
          variantTy, storage, idx, "drop.payload." + variant.name);
      scopes().emitDropInPlace(pt, fieldPtr, "enum.payload");
    }
    ctx.builder->CreateBr(doneBB);
  }

  ctx.builder->SetInsertPoint(doneBB);
  // Poison the tag (never memset: tag 0 is a real variant). Double drops and
  // drops of moved-from storage fall into the switch default above.
  ctx.builder->CreateStore(ConstantInt::get(i32Ty, -1), tagPtr);
  ctx.builder->CreateRetVoid();
  return fn;
}

void EnumGenerator::emitDrop(sun::EnumType& enumType, Value* storagePtr) {
  if (Function* drop = getOrCreateDropFunction(enumType)) {
    ctx.builder->CreateCall(drop, {storagePtr});
  }
}
