/** Builds borrowed interface views and static dispatch tables. */

#include "ast.h"
#include "codegen/classes/class_generator.h"
#include "codegen/codegen_visitor.h"

using sun::semantic_analysis::PortableDeclarationKey;
using sun::types::ClassType;
using sun::types::InterfaceType;

using namespace llvm;

/** Provides the generator for class storage and method operations. */
namespace sun::codegen::classes {

// -------------------------------------------------------------------

GlobalVariable* ClassGenerator::getOrCreateInterfaceVtable(
    ClassType* classType, InterfaceType* ifaceType) {
  if (!classType->belongsTo(state_.analysis->declarations) ||
      !ifaceType->belongsTo(state_.analysis->declarations)) {
    sun::support::logAndThrowError(
        "Interface dispatch types belong to another analysis session");
  }
  auto& table = vtableGlobals[{classType->getDeclarationId(),
                               ifaceType->getDeclarationId()}];
  if (table) return table;

  auto* ptrTy = PointerType::getUnqual(ctx.getContext());

  // Build one function pointer per non-generic interface method. Methods not
  // present here are declared as externals and resolved from the defining
  // module at link/JIT time. An optional parent table link follows the methods.
  std::vector<Constant*> vtableEntries;
  for (const auto& m : ifaceType->getMethods()) {
    if (m.isGeneric()) continue;

    vtableEntries.push_back(functions().lookupFunctionById(
        classType->getInterfaceMethod(m.declarationId)));
  }
  if (ifaceType->getParent()) {
    vtableEntries.push_back(
        getOrCreateInterfaceVtable(classType, ifaceType->getParent().get()));
  }

  std::vector<llvm::Type*> slotTypes(vtableEntries.size(), ptrTy);
  llvm::StructType* vtableType =
      llvm::StructType::get(ctx.getContext(), slotTypes);

  std::string vtableName =
      PortableDeclarationKey::inInstance(
          PortableDeclarationKey::fromDeclaration(
              ifaceType->getDeclarationId(), state_.analysis->declarations),
          PortableDeclarationKey::fromDeclaration(
              classType->getDeclarationId(), state_.analysis->declarations))
          .symbol("vtable");
  Constant* vtableInit = ConstantStruct::get(vtableType, vtableEntries);
  auto* vtableGlobal =
      new GlobalVariable(*module, vtableType, /*isConstant=*/true,
                         GlobalValue::InternalLinkage, vtableInit, vtableName);

  table = vtableGlobal;
  return table;
}

Value* ClassGenerator::upcastInterface(Value* value, InterfaceType* source,
                                       InterfaceType* target) {
  if (!source->extendsInterface(*target))
    sun::support::logAndThrowError("Invalid interface upcast");
  auto* fatType = InterfaceType::getFatPointerType(ctx.getContext());
  if (value->getType()->isPointerTy()) {
    value = ctx.builder->CreateLoad(fatType, value, "iface.borrow");
  }
  auto* ptrType = PointerType::getUnqual(ctx.getContext());
  while (!source->equals(*target)) {
    Value* table = ctx.builder->CreateExtractValue(value, 1, "iface.table");
    Value* slot = ctx.builder->CreateGEP(
        ptrType, table, ctx.builder->getInt32(source->getParentIndex()),
        "iface.parent.slot");
    Value* parent =
        ctx.builder->CreateLoad(ptrType, slot, "iface.parent.table");
    value = ctx.builder->CreateInsertValue(value, parent, 1, "iface.parent");
    source = source->getParent().get();
  }
  return value;
}

Value* ClassGenerator::createBorrowedInterfaceUpcast(
    Value* value, TypePtr sourceType, TypePtr targetType) {
  if (!value || !targetType || !targetType->isReference()) return value;
  if (!sun::types::areDifferentInterfaces(sourceType, targetType)) return value;
  auto source = sun::types::unwrapRef(sourceType);
  auto target = sun::types::unwrapRef(targetType);

  Value* view =
      upcastInterface(value, static_cast<InterfaceType*>(source.get()),
                      static_cast<InterfaceType*>(target.get()));
  auto* storage = ctx.builder->CreateAlloca(view->getType(), nullptr,
                                            "iface.parent.view");
  ctx.builder->CreateStore(view, storage);
  return storage;
}

Value* ClassGenerator::createInterfaceFatPointer(Value* objectPtr,
                                                 ClassType* classType,
                                                 InterfaceType* ifaceType) {
  // Look up (or build) the vtable for this (class, interface) pair.
  GlobalVariable* vtableGlobal =
      getOrCreateInterfaceVtable(classType, ifaceType);
  if (!vtableGlobal) {
    sun::support::logAndThrowError(
        "Vtable not found for class " + classType->getDisplayName() +
        " implementing interface " + ifaceType->getName());
    return nullptr;
  }

  // Create the fat pointer struct { ptr data, ptr vtable }
  llvm::StructType* fatPtrType =
      InterfaceType::getFatPointerType(ctx.getContext());
  Value* fatPtr = UndefValue::get(fatPtrType);

  // Insert the data pointer (element 0)
  fatPtr = ctx.builder->CreateInsertValue(fatPtr, objectPtr, 0, "fat.data");

  // Insert the vtable pointer (element 1)
  fatPtr =
      ctx.builder->CreateInsertValue(fatPtr, vtableGlobal, 1, "fat.vtable");

  return fatPtr;
}

// -------------------------------------------------------------------
// Helper: Convert class to interface fat pointer if needed
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// Helper: Prepare class argument for ref Interface parameter
// Creates fat pointer on stack and returns pointer to it
// -------------------------------------------------------------------

Value* ClassGenerator::prepareClassForRefInterface(
    Value* classPtr, sun::types::TypePtr argType,
    sun::types::TypePtr paramType) {
  // Check if conversion is needed: param is ref Interface and arg is class
  auto* refType =
      sun::codegen::support::tryGetType<sun::types::ReferenceType>(paramType);
  if (!refType) return nullptr;  // Not a ref param

  auto* ifaceType = sun::codegen::support::tryGetType<InterfaceType>(
      refType->getReferencedType());
  if (!ifaceType) return nullptr;  // Not ref Interface

  auto* classType = sun::codegen::support::tryGetType<ClassType>(argType);
  if (!classType) return nullptr;  // Arg is not a class

  // Create interface fat pointer value
  Value* fatPtr = createInterfaceFatPointer(classPtr, classType, ifaceType);

  // Allocate space on stack for the fat pointer and store it there
  // ref Interface expects a pointer to the fat pointer
  llvm::Type* fatPtrType = ifaceType->getFatPointerType(ctx.getContext());
  AllocaInst* fatPtrAlloca =
      ctx.builder->CreateAlloca(fatPtrType, nullptr, "iface.ref.tmp");
  ctx.builder->CreateStore(fatPtr, fatPtrAlloca);
  return fatPtrAlloca;
}

// -------------------------------------------------------------------
// Helper: Load closure struct for lambda-typed parameters
// Lambda literals codegen to an alloca; params take the closure by value

}  // namespace sun::codegen::classes
