// scope_manager.cpp — The scope stack and the drop code it emits
//
// Opening and closing scopes, finding variables in them, recording what each
// one owns, and writing the releases: class deinit plus field recursion, the
// synthesized drop function for a payload enum, and free() for heap
// allocations. See scope_manager.h.

#include "codegen/scopes/scope_manager.h"

#include "codegen/codegen_visitor.h"
#include "semantic_analysis/packed_layout.h"

using namespace llvm;

// -------------------------------------------------------------------
// The stack itself
// -------------------------------------------------------------------

CodegenScope& ScopeManager::push(const Position& loc) {
  auto& scope = push();
  scope.hasDebugScope = state_.debugInfo.pushLexicalBlock(*ctx.builder, loc);
  return scope;
}

void ScopeManager::pop() {
  if (scopes_.empty()) return;
  // Run this scope's pending drops unless the block already terminated
  // (return/break/throw paths emit their own multi-scope cleanup first).
  llvm::BasicBlock* bb = ctx.builder->GetInsertBlock();
  if (bb && !bb->getTerminator()) {
    emitCleanupForScope(scopes_.back());
  }
  if (scopes_.back().hasDebugScope) state_.debugInfo.popLexicalBlock();
  scopes_.pop_back();
}

// -------------------------------------------------------------------
// Finding variables
// -------------------------------------------------------------------

AllocaInst* ScopeManager::findVariable(const std::string& name) {
  // Search from innermost scope to outermost
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto found = it->variables.find(name);
    if (found != it->variables.end()) {
      return found->second;
    }
    // Stop at function boundary - outer function scopes are inaccessible
    // (captured variables should be accessed via closure stack)
    if (it->isFunctionBoundary) {
      break;
    }
  }
  return nullptr;
}

bool ScopeManager::isIndirectBinding(const std::string& name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    if (it->variables.count(name)) return it->indirectBindings.count(name);
    if (it->isFunctionBoundary) break;
  }
  return false;
}

Value* ScopeManager::compoundStorageAddress(const std::string& name) {
  AllocaInst* alloca = findVariable(name);
  if (!alloca) return nullptr;
  if (isIndirectBinding(name)) {
    return ctx.builder->CreateLoad(PointerType::getUnqual(ctx.getContext()),
                                   alloca, name + ".borrow");
  }
  return alloca;
}

// -------------------------------------------------------------------
// Taking and giving up ownership
// -------------------------------------------------------------------

void ScopeManager::trackClassAllocation(Value* alloca, const std::string& name,
                                        sun::TypePtr type) {
  if (scopes_.empty()) return;
  if (type && type->isEnum() && !sun::typeNeedsDrop(type)) return;
  for (auto& scope : scopes_) {
    for (auto& alloc : scope.classAllocations) {
      if (alloc.alloca == alloca) {
        alloc.varName = name;  // adopt the variable's name for diagnostics
        return;
      }
    }
  }
  scopes_.back().classAllocations.push_back(
      {alloca, name, false, std::move(type)});
}

void ScopeManager::markClassAllocationAsDeinited(Value* alloca) {
  for (auto& scope : scopes_) {
    for (auto& alloc : scope.classAllocations) {
      if (alloc.alloca == alloca) {
        alloc.moved = true;
        return;
      }
    }
  }
}

void ScopeManager::markAsMoved(const std::string& name) {
  for (auto& scope : scopes_) {
    for (auto& alloc : scope.ownedAllocations) {
      if (alloc.varName == name) {
        alloc.moved = true;
        return;
      }
    }
  }
}

bool ScopeManager::hasLiveOwners(size_t depth) const {
  for (size_t i = depth; i < scopes_.size(); ++i) {
    for (const auto& a : scopes_[i].classAllocations)
      if (!a.moved) return true;
    for (const auto& a : scopes_[i].ownedAllocations)
      if (!a.moved) return true;
  }
  return false;
}

// -------------------------------------------------------------------
// Emitting drops
// -------------------------------------------------------------------

void ScopeManager::emitFieldCleanup(Value* objectPtr,
                                    const sun::ClassType* classType,
                                    const std::string& baseName,
                                    FunctionCallee freeFunc) {
  if (!classType) return;

  StructType* structType = classType->getStructType(ctx.getContext());

  auto* nullPtr =
      ConstantPointerNull::get(PointerType::getUnqual(ctx.getContext()));
  Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();

  for (const auto& field : classType->getFields()) {
    if (field.type->isRawPointer()) {
      // raw_ptr<T> fields are also freed - used for dynamic data allocations
      // in classes that manage their own memory
      Value* fieldPtr =
          ctx.builder->CreateStructGEP(structType, objectPtr, field.index,
                                       baseName + "." + field.name + ".ptr");

      llvm::Type* ptrTy = PointerType::getUnqual(ctx.getContext());
      Align ptrAlign = sun::packed::fieldAlign(classType, ptrTy,
                                               state_.module->getDataLayout());
      Value* fieldValue = ctx.builder->CreateAlignedLoad(
          ptrTy, fieldPtr, ptrAlign, baseName + "." + field.name + ".value");

      // Null-check raw_ptr fields too
      BasicBlock* freeRawBB = BasicBlock::Create(
          ctx.getContext(), baseName + "." + field.name + ".free_raw",
          currentFunc);
      BasicBlock* skipRawBB = BasicBlock::Create(
          ctx.getContext(), baseName + "." + field.name + ".skip_raw",
          currentFunc);

      Value* isRawNull = ctx.builder->CreateICmpEQ(
          fieldValue, nullPtr, baseName + "." + field.name + ".raw_is_null");
      ctx.builder->CreateCondBr(isRawNull, skipRawBB, freeRawBB);

      ctx.builder->SetInsertPoint(freeRawBB);
      ctx.builder->CreateCall(freeFunc, {fieldValue});
      ctx.builder->CreateAlignedStore(nullPtr, fieldPtr, ptrAlign);
      ctx.builder->CreateBr(skipRawBB);

      ctx.builder->SetInsertPoint(skipRawBB);
    } else if (auto* nestedClass = sun::tryGetType<sun::ClassType>(field.type)) {
      // Embedded class field - recursively call deinit on it if it has one
      // Generate GEP to access the embedded struct field
      Value* fieldPtr = ctx.builder->CreateStructGEP(
          structType, objectPtr, field.index, baseName + "." + field.name);

      // Recursively emit field cleanup and deinit for the nested class
      emitFieldCleanup(fieldPtr, nestedClass, baseName + "." + field.name,
                       freeFunc);
    }
  }
}

void ScopeManager::emitDeinitCall(const sun::ClassType* classType,
                                  Value* receiver) {
  const sun::ClassMethod* deinitMethod = classType->getMethod("deinit");
  if (!deinitMethod) return;

  Function* deinitFunc = gen_.functionRegistry().getOrDeclareMethodFunction(
      classType->getMangledMethodName("deinit"), deinitMethod->paramTypes,
      deinitMethod->returnType, deinitMethod->canThrow);
  ctx.builder->CreateCall(
      deinitFunc,
      {gen_.materializeMethodClosure(deinitFunc, receiver, "deinit.closure")});
}

void ScopeManager::emitFieldDeinit(Value* objectPtr,
                                   const sun::ClassType* classType,
                                   const std::string& baseName) {
  if (!classType) return;

  StructType* structType = classType->getStructType(ctx.getContext());

  for (const auto& field : classType->getFields()) {
    if (auto* nestedClass = sun::tryGetType<sun::ClassType>(field.type)) {
      // Generate GEP to access the embedded struct field
      Value* fieldPtr = ctx.builder->CreateStructGEP(
          structType, objectPtr, field.index, baseName + "." + field.name);

      emitDeinitCall(nestedClass, fieldPtr);

      // Recursively deinit nested class fields
      emitFieldDeinit(fieldPtr, nestedClass, baseName + "." + field.name);
    } else if (field.type->isEnum() && sun::typeNeedsDrop(field.type)) {
      Value* fieldPtr = ctx.builder->CreateStructGEP(
          structType, objectPtr, field.index, baseName + "." + field.name);
      emitEnumDrop(static_cast<sun::EnumType&>(*field.type), fieldPtr);
    }
  }
}

void ScopeManager::emitDropInPlace(const sun::TypePtr& type, Value* ptr,
                                   const std::string& name) {
  if (!type || !ptr) return;
  if (auto* classType = sun::tryGetType<sun::ClassType>(type)) {
    emitDeinitCall(classType, ptr);
    emitFieldDeinit(ptr, classType, name);
  } else if (type->isEnum()) {
    emitEnumDrop(static_cast<sun::EnumType&>(*type), ptr);
  }
}

void ScopeManager::emitDropUnlessZeroed(const sun::TypePtr& type, Value* ptr,
                                        const std::string& name) {
  if (!type || !ptr) return;
  llvm::Type* storage = type->toLLVMType(ctx.getContext());
  if (!storage) return;
  const DataLayout& DL = state_.module->getDataLayout();
  uint64_t size = DL.getTypeAllocSize(storage);
  if (size == 0) return;

  // Compare the storage against a run of zero bytes
  llvm::Type* byteArray =
      llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx.getContext()), size);
  auto* zeros = new GlobalVariable(
      *state_.module, byteArray, /*isConstant=*/true, GlobalValue::PrivateLinkage,
      llvm::ConstantAggregateZero::get(byteArray), name + ".zeroes");
  zeros->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

  llvm::PointerType* bytePtr = PointerType::getUnqual(ctx.getContext());
  FunctionCallee memcmpFn = state_.module->getOrInsertFunction(
      "memcmp", FunctionType::get(Type::getInt32Ty(ctx.getContext()),
                                  {bytePtr, bytePtr,
                                   Type::getInt64Ty(ctx.getContext())},
                                  false));
  Value* difference = ctx.builder->CreateCall(
      memcmpFn,
      {ptr, zeros,
       ConstantInt::get(Type::getInt64Ty(ctx.getContext()), size)},
      name + ".cmp");
  Value* holdsSomething = ctx.builder->CreateICmpNE(
      difference, ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0),
      name + ".live");

  Function* parent = ctx.builder->GetInsertBlock()->getParent();
  BasicBlock* dropBlock =
      BasicBlock::Create(ctx.getContext(), name + ".drop", parent);
  BasicBlock* afterBlock =
      BasicBlock::Create(ctx.getContext(), name + ".dropped", parent);
  ctx.builder->CreateCondBr(holdsSomething, dropBlock, afterBlock);

  ctx.builder->SetInsertPoint(dropBlock);
  emitDropInPlace(type, ptr, name);
  ctx.builder->CreateBr(afterBlock);

  ctx.builder->SetInsertPoint(afterBlock);
}

void ScopeManager::emitCleanupToDepth(size_t depth) {
  if (scopes_.empty()) return;
  for (size_t i = scopes_.size(); i-- > depth;) {
    emitCleanupForScope(scopes_[i]);
  }
}

void ScopeManager::emitCleanupForScope(CodegenScope& scope) {
  // First, cleanup class allocations (call deinit methods)
  auto& currentClassScope = scope.classAllocations;

  // Drop all non-moved allocations in reverse order (LIFO)
  if (!currentClassScope.empty()) {
    for (auto it = currentClassScope.rbegin(); it != currentClassScope.rend();
         ++it) {
      if (!it->moved && it->alloca && it->type) {
        emitDropInPlace(it->type, it->alloca, it->varName);
      }
    }
  }

  // Then, cleanup owned pointer allocations (ptr<T>)
  auto& currentScope = scope.ownedAllocations;
  if (currentScope.empty()) return;

  // Get or declare free function: void free(ptr)
  FunctionType* freeType =
      FunctionType::get(llvm::Type::getVoidTy(ctx.getContext()),
                        {PointerType::getUnqual(ctx.getContext())}, false);
  FunctionCallee freeFunc =
      state_.module->getOrInsertFunction("free", freeType);

  auto* ptrTy = PointerType::getUnqual(ctx.getContext());

  auto* nullPtr = ConstantPointerNull::get(ptrTy);
  Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();

  // Free all non-moved allocations in reverse order (LIFO)
  for (auto it = currentScope.rbegin(); it != currentScope.rend(); ++it) {
    if (!it->moved && it->ptrAlloca) {
      // Load the pointer from the alloca
      Value* ptrToFree =
          ctx.builder->CreateLoad(PointerType::getUnqual(ctx.getContext()),
                                  it->ptrAlloca, it->varName + ".ptr_to_free");

      // Null-check: skip freeing if the pointer is null
      BasicBlock* freeBB = BasicBlock::Create(
          ctx.getContext(), it->varName + ".cleanup", currentFunc);
      BasicBlock* skipBB = BasicBlock::Create(
          ctx.getContext(), it->varName + ".skip_cleanup", currentFunc);

      Value* isNull = ctx.builder->CreateICmpEQ(ptrToFree, nullPtr,
                                                it->varName + ".is_null");
      ctx.builder->CreateCondBr(isNull, skipBB, freeBB);

      ctx.builder->SetInsertPoint(freeBB);

      if (auto* classType = sun::tryGetType<sun::ClassType>(it->pointeeType)) {
        emitDeinitCall(classType, ptrToFree);
        // Recursively deinit class fields and free nested ptr<T> fields
        emitFieldDeinit(ptrToFree, classType, it->varName);
        emitFieldCleanup(ptrToFree, classType, it->varName, freeFunc);
      }
      // Then free the object itself
      ctx.builder->CreateCall(freeFunc, {ptrToFree});

      // Null out the pointer to prevent double-free
      ctx.builder->CreateStore(nullPtr, it->ptrAlloca);

      ctx.builder->CreateBr(skipBB);
      ctx.builder->SetInsertPoint(skipBB);
    }
  }
}

// -------------------------------------------------------------------
// Enum drop glue: void __sun_enum_drop$<Enum>(ptr storage)
// Switches on the tag, drops each owning payload (class deinit + field
// recursion, or a nested enum's drop function), then poisons the tag with -1
// so a second drop falls through the switch as a no-op.
// -------------------------------------------------------------------

Function* ScopeManager::getOrCreateEnumDropFunction(sun::EnumType& enumType) {
  if (!sun::typeNeedsDrop(&enumType)) return nullptr;

  std::string name = "__sun_enum_drop$" + enumType.getName();
  if (Function* existing = state_.module->getFunction(name)) return existing;

  auto* voidTy = llvm::Type::getVoidTy(ctx.getContext());
  auto* ptrTy = PointerType::getUnqual(ctx.getContext());
  auto* i32Ty = llvm::Type::getInt32Ty(ctx.getContext());
  FunctionType* fnTy = FunctionType::get(voidTy, {ptrTy}, false);
  // LinkOnceODR: the same specialization may be emitted by several modules
  // (main program + .moon bundles); identical bodies merge at link/JIT time.
  Function* fn = Function::Create(fnTy, Function::LinkOnceODRLinkage, name,
                                  state_.module);

  CodegenState::InsertPointGuard here(state_);
  BasicBlock* entry = BasicBlock::Create(ctx.getContext(), "entry", fn);
  ctx.builder->SetInsertPoint(entry);
  Value* storage = fn->getArg(0);

  StructType* storageTy = state_.typeResolver.getEnumStorageType(enumType);
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
        state_.typeResolver.getEnumVariantStruct(enumType, variant.name);
    for (size_t i = 0; i < variant.payloadTypes.size(); ++i) {
      const sun::TypePtr& pt = variant.payloadTypes[i];
      if (!pt || !sun::typeNeedsDrop(pt)) continue;
      unsigned idx =
          state_.typeResolver.enumPayloadFieldIndex(enumType, variant.name, i);
      Value* fieldPtr = ctx.builder->CreateStructGEP(
          variantTy, storage, idx, "drop.payload." + variant.name);
      emitDropInPlace(pt, fieldPtr, "enum.payload");
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

void ScopeManager::emitEnumDrop(sun::EnumType& enumType, Value* storagePtr) {
  if (Function* drop = getOrCreateEnumDropFunction(enumType)) {
    ctx.builder->CreateCall(drop, {storagePtr});
  }
}
