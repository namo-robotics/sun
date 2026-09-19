// scope_manager.cpp — The scope stack and the drop code it emits
//
// Opening and closing scopes, finding variables in them, recording what each
// one owns, and writing the releases: class deinit plus field recursion, the
// synthesized drop function for a payload enum, and free() for heap
// allocations. See scope_manager.h.

#include "codegen/scopes/scope_manager.h"

#include <llvm/IR/Operator.h>

#include "codegen/codegen_visitor.h"
#include "semantic_analysis/packed_layout.h"

using sun::semantic_analysis::ClassType;
using sun::semantic_analysis::DeclarationId;
using sun::semantic_analysis::TypePtr;

using namespace llvm;

/** Provides the scope manager responsible for variable storage and cleanup. */
namespace sun::codegen::scopes {

// -------------------------------------------------------------------
// The stack itself
// -------------------------------------------------------------------

CodegenScope& ScopeManager::push(const sun::support::Position& loc) {
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

AllocaInst* ScopeManager::findVariable(DeclarationId id) {
  // Search from innermost scope to outermost
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto found = it->variables.find(id);
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

bool ScopeManager::isIndirectBinding(DeclarationId id) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    if (it->variables.count(id)) return it->indirectBindings.count(id);
    if (it->isFunctionBoundary) break;
  }
  return false;
}

Value* ScopeManager::compoundStorageAddress(DeclarationId id) {
  AllocaInst* alloca = findVariable(id);
  if (!alloca) return nullptr;
  if (isIndirectBinding(id)) {
    return ctx.builder->CreateLoad(PointerType::getUnqual(ctx.getContext()),
                                   alloca, alloca->getName() + ".borrow");
  }
  return alloca;
}

// -------------------------------------------------------------------
// Taking and giving up ownership
// -------------------------------------------------------------------

void ScopeManager::trackClassAllocation(Value* alloca, const std::string& name,
                                        TypePtr type, bool unwindOnly) {
  if (scopes_.empty()) return;
  if (type && (type->isEnum() || type->isArray()) &&
      !sun::semantic_analysis::typeNeedsDrop(type))
    return;
  for (auto& scope : scopes_) {
    for (auto& alloc : scope.classAllocations) {
      if (alloc.alloca == alloca) {
        alloc.varName = name;  // adopt the variable's name for diagnostics
        return;
      }
    }
  }
  ClassAllocation entry{alloca, name, false, std::move(type)};
  entry.unwindOnly = unwindOnly;
  // Remember this point: if a branch later moves the value on some paths only,
  // its drop flag is set here, where the value became owned.
  if (BasicBlock* here = ctx.builder->GetInsertBlock()) {
    entry.ownedAt = here->empty() ? static_cast<Value*>(here)
                                  : static_cast<Value*>(&here->back());
  }
  scopes_.back().classAllocations.push_back(std::move(entry));
}

/** Keeps the implementation helpers in this file private to this translation unit. */
namespace {

/** Identify fields by their path, since empty fields can share an address. */
struct StoragePlace {
  Value* base;
  std::vector<uint64_t> fields;
};

/** Follow constant field addresses without merging distinct empty fields. */
StoragePlace storagePlace(Value* ptr) {
  if (auto* gep = dyn_cast<GEPOperator>(ptr)) {
    auto parent = storagePlace(gep->getPointerOperand());
    auto index = gep->idx_begin();
    auto* first = dyn_cast<ConstantInt>(index->get());
    if (!first || !first->isZero()) return {ptr, {}};
    for (++index; index != gep->idx_end(); ++index) {
      auto* field = dyn_cast<ConstantInt>(index->get());
      if (!field) return {ptr, {}};
      parent.fields.push_back(field->getZExtValue());
    }
    return parent;
  }
  return {ptr, {}};
}

}  // namespace

ClassAllocation* ScopeManager::findAllocation(Value* ptr, const TypePtr& type) {
  auto place = storagePlace(ptr);
  for (auto& scope : scopes_) {
    for (auto& alloc : scope.classAllocations) {
      if (!type && alloc.alloca != ptr) continue;
      if (type && (!alloc.type || !alloc.type->equals(*type))) continue;
      auto candidate = storagePlace(alloc.alloca);
      if (place.base == candidate.base && place.fields == candidate.fields)
        return &alloc;
    }
  }
  return nullptr;
}

void ScopeManager::markInitialized(Value* ptr, const TypePtr& type) {
  if (!type || !sun::semantic_analysis::typeNeedsDrop(type)) return;
  if (auto* alloc = findAllocation(ptr, type)) {
    setOwnership(*alloc, true);
  }
  auto place = storagePlace(ptr);
  for (auto& scope : scopes_) {
    for (auto& field : scope.classAllocations) {
      if (!field.isField) continue;
      auto child = storagePlace(field.alloca);
      if (place.base == child.base &&
          child.fields.size() > place.fields.size() &&
          std::equal(place.fields.begin(), place.fields.end(),
                     child.fields.begin())) {
        setOwnership(field, true);
      }
    }
  }
}

ClassAllocation* ScopeManager::trackFieldAllocation(Value* ptr,
                                                    const TypePtr& type) {
  Value* base = storagePlace(ptr).base;
  for (auto& scope : scopes_) {
    for (const auto& owner : scope.classAllocations) {
      if (owner.isField || owner.alloca != base) continue;
      ClassAllocation field{ptr, owner.varName + ".field", false, type};
      field.isField = true;
      field.ownedAt = owner.ownedAt;
      scope.classAllocations.push_back(std::move(field));
      return &scope.classAllocations.back();
    }
  }
  return nullptr;
}

void ScopeManager::setOwnership(ClassAllocation& alloc, bool owned) {
  alloc.moved = !owned;
  if (alloc.dropFlag)
    ctx.builder->CreateStore(ConstantInt::getBool(ctx.getContext(), owned),
                             alloc.dropFlag);
}

void ScopeManager::markClassAllocationAsDeinited(Value* alloca, TypePtr type) {
  auto* alloc = findAllocation(alloca, type);
  if (!alloc && type) alloc = trackFieldAllocation(alloca, type);
  if (alloc) {
    // A later assignment may restore ownership on only some paths.
    ensureDropFlag(*alloc);
    setOwnership(*alloc, false);
  }
}

void ScopeManager::ensureDropFlag(ClassAllocation& alloc) {
  if (alloc.dropFlag) return;

  // Where the value became owned. Nothing left to anchor to means the code
  // that took ownership is gone, so leave the decision static.
  Value* anchor = alloc.ownedAt;
  auto* anchorInst = dyn_cast_or_null<Instruction>(anchor);
  BasicBlock* anchorBlock = anchorInst ? anchorInst->getParent()
                                       : dyn_cast_or_null<BasicBlock>(anchor);
  if (!anchorBlock || !anchorBlock->getParent()) return;

  llvm::Type* boolTy = llvm::Type::getInt1Ty(ctx.getContext());
  Function* func = anchorBlock->getParent();

  // The flag sits beside the value in the frame, never inside it, so class
  // layout is untouched. False on entry, so a path that never reached the
  // point of ownership never drops.
  IRBuilder<> entry(&func->getEntryBlock(), func->getEntryBlock().begin());
  alloc.dropFlag =
      entry.CreateAlloca(boolTy, nullptr, alloc.varName + ".owned");
  entry.CreateStore(ConstantInt::getFalse(ctx.getContext()), alloc.dropFlag);

  // True from the point of ownership on. This sits inside the loop body when
  // the value is created per iteration, so each iteration starts owning it.
  IRBuilder<> owned(anchorBlock, anchorInst
                                     ? std::next(anchorInst->getIterator())
                                     : anchorBlock->begin());
  owned.CreateStore(ConstantInt::getTrue(ctx.getContext()), alloc.dropFlag);
}

void ScopeManager::emitFlaggedDrop(const ClassAllocation& alloc) {
  Function* parent = ctx.builder->GetInsertBlock()->getParent();
  Value* owned =
      ctx.builder->CreateLoad(llvm::Type::getInt1Ty(ctx.getContext()),
                              alloc.dropFlag, alloc.varName + ".is_owned");

  BasicBlock* dropBlock =
      BasicBlock::Create(ctx.getContext(), alloc.varName + ".drop", parent);
  BasicBlock* afterBlock =
      BasicBlock::Create(ctx.getContext(), alloc.varName + ".dropped", parent);
  ctx.builder->CreateCondBr(owned, dropBlock, afterBlock);

  ctx.builder->SetInsertPoint(dropBlock);
  emitUnconditionalDrop(alloc.type, alloc.alloca, alloc.varName);
  // Given up here, so a later cleanup on this path finds nothing to do
  ctx.builder->CreateStore(ConstantInt::getFalse(ctx.getContext()),
                           alloc.dropFlag);
  ctx.builder->CreateBr(afterBlock);

  ctx.builder->SetInsertPoint(afterBlock);
}

std::optional<std::string> ScopeManager::releaseBlockResult(Value* result) {
  if (!result || scopes_.empty()) return std::nullopt;
  for (auto& alloc : scopes_.back().classAllocations) {
    if (alloc.alloca != result || alloc.moved) continue;
    // The block always reaches its own last statement, so ownership leaves it
    // on every path and the decision stays static in the scope that gets it.
    setOwnership(alloc, false);
    return alloc.varName;
  }
  return std::nullopt;
}

bool ScopeManager::hasLiveOwners(size_t depth) const {
  for (size_t i = depth; i < scopes_.size(); ++i) {
    // A drop flag means ownership is a run-time answer, so assume it is owned
    for (const auto& a : scopes_[i].classAllocations)
      if (!a.moved || a.dropFlag) return true;
    for (const auto& a : scopes_[i].ownedAllocations)
      if (!a.moved) return true;
  }
  return false;
}

// -------------------------------------------------------------------
// Emitting drops
// -------------------------------------------------------------------

void ScopeManager::emitFieldCleanup(Value* objectPtr,
                                    const ClassType* classType,
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
      Align ptrAlign = sun::semantic_analysis::fieldAlign(
          classType, ptrTy, state_.module->getDataLayout());
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
    } else if (auto* nestedClass =
                   sun::codegen::support::tryGetType<ClassType>(field.type)) {
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

void ScopeManager::emitDeinitCall(const ClassType* classType, Value* receiver) {
  if (!classType->deinitializer) return;
  Function* deinitFunc =
      gen_.functionRegistry().lookupFunctionById(classType->deinitializer);
  ctx.builder->CreateCall(
      deinitFunc,
      {gen_.materializeMethodClosure(deinitFunc, receiver, "deinit.closure")});
}

void ScopeManager::emitFieldDeinit(Value* objectPtr, const ClassType* classType,
                                   const std::string& baseName) {
  if (!classType) return;

  StructType* structType = classType->getStructType(ctx.getContext());

  for (const auto& field : classType->getFields()) {
    if (!sun::semantic_analysis::typeNeedsDrop(field.type)) continue;
    const auto name = baseName + "." + field.name;
    Value* fieldPtr =
        ctx.builder->CreateStructGEP(structType, objectPtr, field.index, name);
    emitDropInPlace(field.type, fieldPtr, name);
  }
}

/**
 * Drops every initialized element of a sized array's inline storage.
 * Safe code cannot move individual elements out of an array.
 */
void ScopeManager::emitArrayDrop(sun::semantic_analysis::ArrayType& arrayType,
                                 Value* storagePtr, const std::string& name) {
  if (arrayType.isUnsized() ||
      !sun::semantic_analysis::typeNeedsDrop(&arrayType))
    return;
  const TypePtr& elemType = arrayType.getElementType();
  llvm::Type* elemLLVMType = elemType->toLLVMType(ctx.getContext());
  size_t count = arrayType.getTotalElements();

  // The storage is [N x [M x T]]; its first element's address is also the
  // address of a flat run of N*M elements
  Value* first = ctx.builder->CreateBitCast(
      storagePtr, PointerType::getUnqual(ctx.getContext()), name + ".elems");

  Function* parent = ctx.builder->GetInsertBlock()->getParent();
  BasicBlock* headBlock =
      BasicBlock::Create(ctx.getContext(), name + ".drop.head", parent);
  BasicBlock* bodyBlock =
      BasicBlock::Create(ctx.getContext(), name + ".drop.body", parent);
  BasicBlock* doneBlock =
      BasicBlock::Create(ctx.getContext(), name + ".drop.done", parent);
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());

  BasicBlock* entryBlock = ctx.builder->GetInsertBlock();
  ctx.builder->CreateBr(headBlock);

  ctx.builder->SetInsertPoint(headBlock);
  PHINode* index = ctx.builder->CreatePHI(i64Ty, 2, name + ".drop.i");
  index->addIncoming(ConstantInt::get(i64Ty, 0), entryBlock);
  Value* more = ctx.builder->CreateICmpULT(
      index, ConstantInt::get(i64Ty, count), name + ".drop.more");
  ctx.builder->CreateCondBr(more, bodyBlock, doneBlock);

  ctx.builder->SetInsertPoint(bodyBlock);
  Value* elemPtr =
      ctx.builder->CreateGEP(elemLLVMType, first, index, name + ".elem");
  emitDropInPlace(elemType, elemPtr, name + "[i]");
  Value* next =
      ctx.builder->CreateAdd(index, ConstantInt::get(i64Ty, 1), name + ".next");
  index->addIncoming(next, ctx.builder->GetInsertBlock());
  ctx.builder->CreateBr(headBlock);

  ctx.builder->SetInsertPoint(doneBlock);
}

/**
 * Drops the erased concrete owner referenced by an interface value.
 */
void ScopeManager::emitInterfaceDrop(
    sun::semantic_analysis::InterfaceType& interfaceType, Value* storagePtr) {
  StructType* fatType = interfaceType.getFatPointerType(ctx.getContext());
  Value* fat = ctx.builder->CreateLoad(fatType, storagePtr, "iface.drop.fat");
  Value* data = ctx.builder->CreateExtractValue(fat, 0, "iface.drop.data");
  Value* vtable = ctx.builder->CreateExtractValue(fat, 1, "iface.drop.vtable");

  auto* ptrTy = PointerType::getUnqual(ctx.getContext());
  auto* nullPtr = ConstantPointerNull::get(ptrTy);
  Value* isEmpty = ctx.builder->CreateOr(
      ctx.builder->CreateICmpEQ(data, nullPtr),
      ctx.builder->CreateICmpEQ(vtable, nullPtr), "iface.drop.empty");

  Function* parent = ctx.builder->GetInsertBlock()->getParent();
  BasicBlock* dropBlock =
      BasicBlock::Create(ctx.getContext(), "iface.drop", parent);
  BasicBlock* doneBlock =
      BasicBlock::Create(ctx.getContext(), "iface.dropped", parent);
  ctx.builder->CreateCondBr(isEmpty, doneBlock, dropBlock);

  ctx.builder->SetInsertPoint(dropBlock);
  unsigned dropIndex = 0;
  for (const auto& method : interfaceType.getMethods()) {
    if (!method.isGeneric()) ++dropIndex;
  }
  Value* dropSlot = ctx.builder->CreateGEP(
      ptrTy, vtable,
      ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), dropIndex),
      "iface.drop.slot");
  Value* drop = ctx.builder->CreateLoad(ptrTy, dropSlot, "iface.drop.fn");
  llvm::FunctionType* dropType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(ctx.getContext()), {ptrTy}, false);
  ctx.builder->CreateCall(dropType, drop, {data});
  ctx.builder->CreateStore(Constant::getNullValue(fatType), storagePtr);
  ctx.builder->CreateBr(doneBlock);

  ctx.builder->SetInsertPoint(doneBlock);
}

void ScopeManager::emitDropInPlace(const TypePtr& type, Value* ptr,
                                   const std::string& name) {
  if (!type || !ptr) return;
  if (auto* alloc = findAllocation(ptr, type)) {
    if (alloc->dropFlag) {
      auto current = *alloc;
      current.alloca = ptr;
      emitFlaggedDrop(current);
      return;
    }
    if (alloc->moved) return;
  }
  emitUnconditionalDrop(type, ptr, name);
}

void ScopeManager::emitUnconditionalDrop(const TypePtr& type, Value* ptr,
                                         const std::string& name) {
  if (auto* classType = sun::codegen::support::tryGetType<ClassType>(type)) {
    emitDeinitCall(classType, ptr);
    emitFieldDeinit(ptr, classType, name);
  } else if (auto* interfaceType = sun::codegen::support::tryGetType<
                 sun::semantic_analysis::InterfaceType>(type)) {
    emitInterfaceDrop(*interfaceType, ptr);
  } else if (type->isEnum()) {
    gen_.enumGenerator().emitDrop(
        static_cast<sun::semantic_analysis::EnumType&>(*type), ptr);
  } else if (type->isArray()) {
    emitArrayDrop(static_cast<sun::semantic_analysis::ArrayType&>(*type), ptr,
                  name);
  }
}

void ScopeManager::emitCleanupToDepth(size_t depth, bool unwinding) {
  if (scopes_.empty()) return;
  for (size_t i = scopes_.size(); i-- > depth;) {
    emitCleanupForScope(scopes_[i], unwinding);
  }
}

void ScopeManager::emitCleanupForScope(CodegenScope& scope, bool unwinding) {
  // First, cleanup class allocations (call deinit methods)
  auto& currentClassScope = scope.classAllocations;

  // Drop all non-moved allocations in reverse order (LIFO). One that carries a
  // drop flag was moved on some paths only, so its flag decides at run time.
  if (!currentClassScope.empty()) {
    for (auto it = currentClassScope.rbegin(); it != currentClassScope.rend();
         ++it) {
      if (!it->alloca || !it->type || (it->unwindOnly && !unwinding)) continue;
      if (it->isField) continue;
      if (it->dropFlag) {
        emitFlaggedDrop(*it);
      } else if (!it->moved) {
        emitDropInPlace(it->type, it->alloca, it->varName);
      }
    }
  }

  // Then, cleanup owned pointer allocations (ptr<T>)
  auto& currentScope = scope.ownedAllocations;
  if (currentScope.empty()) return;

  // Get or declare free function: void free(ptr)
  llvm::FunctionType* freeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(ctx.getContext()),
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

      if (auto* classType =
              sun::codegen::support::tryGetType<ClassType>(it->pointeeType)) {
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

}  // namespace sun::codegen::scopes
