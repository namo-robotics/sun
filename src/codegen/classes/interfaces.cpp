// interfaces.cpp — Interface dispatch: vtables and fat pointers
//
// A class used as an interface value becomes a fat pointer { data, vtable }.
// The vtable holds the interface's methods in declaration order, built once
// per (class, interface) pair. See class_generator.h.

#include "ast.h"
#include "codegen/classes/class_generator.h"
#include "codegen/codegen_visitor.h"

using namespace llvm;

// -------------------------------------------------------------------

GlobalVariable* ClassGenerator::getOrCreateInterfaceVtable(
    sun::ClassType* classType, sun::InterfaceType* ifaceType) {
  std::string className = classType->getMangledName();
  std::string interfaceName = ifaceType->getName();

  auto it = vtableGlobals.find({className, interfaceName});
  if (it != vtableGlobals.end()) return it->second;

  auto* ptrTy = PointerType::getUnqual(ctx.getContext());

  // Build one function pointer per non-generic interface method (declaration
  // order). Methods not present in this module are declared as externals and
  // resolved from the class's defining module at link/JIT time.
  std::vector<Constant*> vtableEntries;
  bool hasVtableMethods = false;
  for (const auto& m : ifaceType->getMethods()) {
    if (m.isGeneric()) continue;
    hasVtableMethods = true;

    std::string mangled = classType->getMangledMethodName(m.name, m.paramTypes);
    vtableEntries.push_back(functions().getOrDeclareMethodFunction(
        mangled, m.paramTypes, m.returnType, /*canThrow=*/false));
  }

  if (!hasVtableMethods) return nullptr;

  std::string vtableTypeName = className + "_" + interfaceName + "_vtable_t";
  std::vector<llvm::Type*> slotTypes(vtableEntries.size(), ptrTy);
  llvm::StructType* vtableType =
      llvm::StructType::create(ctx.getContext(), slotTypes, vtableTypeName);

  std::string vtableName = className + "_" + interfaceName + "_vtable";
  Constant* vtableInit = ConstantStruct::get(vtableType, vtableEntries);
  auto* vtableGlobal =
      new GlobalVariable(*module, vtableType, /*isConstant=*/true,
                         GlobalValue::InternalLinkage, vtableInit, vtableName);

  vtableGlobals[{className, interfaceName}] = vtableGlobal;
  return vtableGlobal;
}

Value* ClassGenerator::createInterfaceFatPointer(
    Value* objectPtr, sun::ClassType* classType,
    sun::InterfaceType* ifaceType) {
  // Look up (or build) the vtable for this (class, interface) pair.
  GlobalVariable* vtableGlobal =
      getOrCreateInterfaceVtable(classType, ifaceType);
  if (!vtableGlobal) {
    logAndThrowError("Vtable not found for class " +
                     classType->getDisplayName() + " implementing interface " +
                     ifaceType->getName());
    return nullptr;
  }

  // Create the fat pointer struct { ptr data, ptr vtable }
  llvm::StructType* fatPtrType =
      sun::InterfaceType::getFatPointerType(ctx.getContext());
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

Value* ClassGenerator::prepareClassForRefInterface(Value* classPtr,
                                                   sun::TypePtr argType,
                                                   sun::TypePtr paramType) {
  // Check if conversion is needed: param is ref Interface and arg is class
  auto* refType = sun::tryGetType<sun::ReferenceType>(paramType);
  if (!refType) return nullptr;  // Not a ref param

  auto* ifaceType =
      sun::tryGetType<sun::InterfaceType>(refType->getReferencedType());
  if (!ifaceType) return nullptr;  // Not ref Interface

  auto* classType = sun::tryGetType<sun::ClassType>(argType);
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
