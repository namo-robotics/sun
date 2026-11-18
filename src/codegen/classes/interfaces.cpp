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
  if (table) {
    return table;
  }

  auto* ptrTy = PointerType::getUnqual(ctx.getContext());

  // Build one function pointer per non-generic interface method. Methods not
  // present here are declared as externals and resolved from the defining
  // module at link/JIT time. An optional parent table link follows the methods.
  std::vector<Constant*> vtableEntries;
  for (const auto& m : ifaceType->getMethods()) {
    if (m.isGeneric()) {
      continue;
    }

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

Value* ClassGenerator::interfaceViewAddress(Value* address,
                                            TypePtr sourceType) {
  if (!sourceType->isInterface()) {
    return address;
  }
  return ctx.builder->CreateStructGEP(
      InterfaceType::getOwnerType(ctx.getContext()), address,
      InterfaceType::Object, "iface.view");
}

Value* ClassGenerator::createInterfaceView(Value* address, TypePtr sourceType,
                                           InterfaceType* target) {
  auto source = sun::types::unwrapRef(sourceType);
  if (auto* concrete = sun::codegen::support::tryGetType<ClassType>(source)) {
    Value* view =
        UndefValue::get(InterfaceType::getFatPointerType(ctx.getContext()));
    view = ctx.builder->CreateInsertValue(view, address, 0, "fat.data");
    return ctx.builder->CreateInsertValue(
        view, getOrCreateInterfaceVtable(concrete, target), 1, "fat.vtable");
  }
  auto& interface = sun::codegen::support::requireType<InterfaceType>(
      source, "borrowed interface view");
  return upcastInterface(interfaceViewAddress(address, sourceType), &interface,
                         target);
}

Value* ClassGenerator::upcastInterface(Value* value, InterfaceType* source,
                                       InterfaceType* target) {
  if (!source->extendsInterface(*target)) {
    sun::support::logAndThrowError("Invalid interface upcast");
  }
  auto* fatType = InterfaceType::getFatPointerType(ctx.getContext());
  if (value->getType()->isPointerTy()) {
    value = ctx.builder->CreateLoad(fatType, value, "iface.borrow");
  }
  auto* ptrType = PointerType::getUnqual(ctx.getContext());
  const unsigned tableIndex =
      value->getType() == fatType ? 1 : InterfaceType::Vtable;
  while (!source->equals(*target)) {
    Value* table =
        ctx.builder->CreateExtractValue(value, tableIndex, "iface.table");
    Value* slot = ctx.builder->CreateGEP(
        ptrType, table, ctx.builder->getInt32(source->getParentIndex()),
        "iface.parent.slot");
    Value* parent =
        ctx.builder->CreateLoad(ptrType, slot, "iface.parent.table");
    value = ctx.builder->CreateInsertValue(value, parent, tableIndex,
                                           "iface.parent");
    source = source->getParent().get();
  }
  return value;
}

Value* ClassGenerator::prepareInterfaceReference(Value* address,
                                                 TypePtr sourceType,
                                                 TypePtr targetType) {
  if (!targetType || !targetType->isReference()) {
    return address;
  }
  auto* target = sun::codegen::support::tryGetType<InterfaceType>(
      sun::types::unwrapRef(targetType));
  if (!target) {
    return address;
  }
  auto source = sun::types::unwrapRef(sourceType);
  if (source->equals(*target)) {
    return interfaceViewAddress(address, sourceType);
  }
  Value* view = createInterfaceView(address, sourceType, target);
  auto* storage =
      ctx.builder->CreateAlloca(view->getType(), nullptr, "iface.ref.tmp");
  ctx.builder->CreateStore(view, storage);
  return storage;
}

}  // namespace sun::codegen::classes
