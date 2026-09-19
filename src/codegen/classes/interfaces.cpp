// interfaces.cpp — Interface dispatch: vtables and fat pointers
//
// A class used as an interface value becomes a fat pointer { data, vtable }.
// The vtable holds the interface's methods in declaration order followed by
// concrete drop glue. Owning conversions move the object into a stable heap
// box; borrowed conversions keep pointing at their existing storage.

#include "ast.h"
#include "codegen/classes/class_generator.h"
#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/libc.h"

using sun::semantic_analysis::PortableDeclarationKey;
using sun::types::ClassType;
using sun::types::InterfaceType;

using namespace llvm;

/** Provides the generator for class storage and method operations. */
namespace sun::codegen::classes {

// -------------------------------------------------------------------

/**
 * Emits the concrete destructor stored in an interface vtable's final slot.
 */
Function* ClassGenerator::getOrCreateInterfaceDropFunction(
    ClassType* classType) {
  std::string name =
      state_.declarationSymbol(classType->getDeclarationId(), "interface-drop");
  if (Function* existing = module->getFunction(name)) return existing;

  auto* ptrTy = PointerType::getUnqual(ctx.getContext());
  llvm::FunctionType* fnTy = llvm::FunctionType::get(
      llvm::Type::getVoidTy(ctx.getContext()), {ptrTy}, false);
  Function* fn =
      Function::Create(fnTy, Function::LinkOnceODRLinkage, name, module);

  sun::codegen::CodegenState::InsertPointGuard here(state_);
  BasicBlock* entry = BasicBlock::Create(ctx.getContext(), "entry", fn);
  ctx.builder->SetInsertPoint(entry);
  Value* object = fn->getArg(0);
  scopes().emitDeinitCall(classType, object);
  scopes().emitFieldDeinit(object, classType, "interface.owner");
  ctx.builder->CreateCall(sun::codegen::intrinsics::free(module), {object});
  ctx.builder->CreateRetVoid();
  return fn;
}

GlobalVariable* ClassGenerator::getOrCreateInterfaceVtable(
    ClassType* classType, InterfaceType* ifaceType) {
  if (!classType->belongsTo(typeRegistry->declarations) ||
      !ifaceType->belongsTo(typeRegistry->declarations)) {
    sun::support::logAndThrowError(
        "Interface dispatch types belong to another analysis session");
  }
  auto& tables = vtableGlobals[{classType->getDeclarationId(),
                                ifaceType->getDeclarationId()}];
  if (tables.owning) return tables.owning;

  auto* ptrTy = PointerType::getUnqual(ctx.getContext());

  // Build one function pointer per non-generic interface method. Methods not
  // present here are declared as externals and resolved from the defining
  // module at link/JIT time. The last slot always owns concrete drop glue.
  std::vector<Constant*> vtableEntries;
  for (const auto& m : ifaceType->getMethods()) {
    if (m.isGeneric()) continue;

    vtableEntries.push_back(functions().lookupFunctionById(
        classType->getInterfaceMethod(m.declarationId)));
  }
  vtableEntries.push_back(getOrCreateInterfaceDropFunction(classType));

  std::vector<llvm::Type*> slotTypes(vtableEntries.size(), ptrTy);
  llvm::StructType* vtableType =
      llvm::StructType::get(ctx.getContext(), slotTypes);

  std::string vtableName =
      PortableDeclarationKey::inInstance(
          PortableDeclarationKey::fromDeclaration(ifaceType->getDeclarationId(),
                                                  typeRegistry->declarations),
          PortableDeclarationKey::fromDeclaration(classType->getDeclarationId(),
                                                  typeRegistry->declarations))
          .symbol("owning-vtable");
  Constant* vtableInit = ConstantStruct::get(vtableType, vtableEntries);
  auto* vtableGlobal =
      new GlobalVariable(*module, vtableType, /*isConstant=*/true,
                         GlobalValue::InternalLinkage, vtableInit, vtableName);

  tables.owning = vtableGlobal;
  return vtableGlobal;
}

/**
 * Builds a dispatch-equivalent vtable whose final drop slot is a no-op.
 */
GlobalVariable* ClassGenerator::getOrCreateBorrowedInterfaceVtable(
    ClassType* classType, InterfaceType* ifaceType) {
  GlobalVariable* owning = getOrCreateInterfaceVtable(classType, ifaceType);
  auto& tables = vtableGlobals.at(
      {classType->getDeclarationId(), ifaceType->getDeclarationId()});
  if (tables.borrowed) return tables.borrowed;
  auto* owningInit = cast<ConstantStruct>(owning->getInitializer());
  std::vector<Constant*> entries;
  for (unsigned i = 0; i + 1 < owningInit->getNumOperands(); ++i) {
    entries.push_back(cast<Constant>(owningInit->getOperand(i)));
  }

  std::string dropName = "__sun_interface_borrow_drop";
  Function* noOpDrop = module->getFunction(dropName);
  if (!noOpDrop) {
    auto* ptrTy = PointerType::getUnqual(ctx.getContext());
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx.getContext()), {ptrTy}, false);
    noOpDrop =
        Function::Create(fnTy, Function::LinkOnceODRLinkage, dropName, module);
    BasicBlock* entry = BasicBlock::Create(ctx.getContext(), "entry", noOpDrop);
    IRBuilder<> builder(entry);
    builder.CreateRetVoid();
  }
  entries.push_back(noOpDrop);

  auto* ptrTy = PointerType::getUnqual(ctx.getContext());
  std::vector<llvm::Type*> slots(entries.size(), ptrTy);
  StructType* type = StructType::get(ctx.getContext(), slots);
  std::string name =
      PortableDeclarationKey::inInstance(
          PortableDeclarationKey::fromDeclaration(ifaceType->getDeclarationId(),
                                                  typeRegistry->declarations),
          PortableDeclarationKey::fromDeclaration(classType->getDeclarationId(),
                                                  typeRegistry->declarations))
          .symbol("borrowed-vtable");
  auto* result =
      new GlobalVariable(*module, type, true, GlobalValue::InternalLinkage,
                         ConstantStruct::get(type, entries), name);
  tables.borrowed = result;
  return result;
}

Value* ClassGenerator::createInterfaceFatPointer(Value* objectPtr,
                                                 ClassType* classType,
                                                 InterfaceType* ifaceType) {
  // Look up (or build) the vtable for this (class, interface) pair.
  GlobalVariable* vtableGlobal =
      getOrCreateBorrowedInterfaceVtable(classType, ifaceType);
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

/**
 * Moves a concrete class into stable storage owned by an interface value.
 */
Value* ClassGenerator::createOwnedInterfaceFatPointer(
    Value* objectPtr, ClassType* classType, InterfaceType* ifaceType) {
  if (!objectPtr || !objectPtr->getType()->isPointerTy()) return nullptr;

  Value* fatPtr = createInterfaceFatPointer(objectPtr, classType, ifaceType);
  if (!fatPtr) return nullptr;

  GlobalVariable* owningVtable =
      getOrCreateInterfaceVtable(classType, ifaceType);
  fatPtr = ctx.builder->CreateInsertValue(fatPtr, owningVtable, 1,
                                          "iface.owner.vtable");
  StructType* objectType = classType->getStructType(ctx.getContext());
  const DataLayout& layout = module->getDataLayout();
  uint64_t objectSize = layout.getTypeAllocSize(objectType);
  uint64_t allocationSize = objectSize == 0 ? 1 : objectSize;
  Align objectAlign = layout.getABITypeAlign(objectType);

  // Interface ownership uses the same system heap that backs HeapAllocator.
  // The source is moved into the box and zeroed so its pending drop is inert.
  Value* box = ctx.builder->CreateCall(
      sun::codegen::intrinsics::malloc(module),
      {ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()),
                        allocationSize)},
      "iface.box");
  Value* moved = ctx.builder->CreateAlignedLoad(objectType, objectPtr,
                                                objectAlign, "iface.move");
  ctx.builder->CreateAlignedStore(moved, box, objectAlign);
  ctx.builder->CreateMemSet(objectPtr, ctx.builder->getInt8(0), objectSize,
                            MaybeAlign(objectAlign));
  scopes().markClassAllocationAsDeinited(
      objectPtr, std::make_shared<ClassType>(*classType));

  return ctx.builder->CreateInsertValue(fatPtr, box, 0, "iface.owner");
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
