// variable_references.cpp - Variable reference and assignment codegen methods

#include "ast.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "codegen/support/struct_access.h"
#include "codegen/variables/variable_generator.h"

using namespace llvm;

namespace layout = sun::codegen::layout;

// -------------------------------------------------------------------
// Reference helper
// -------------------------------------------------------------------

// Check if a reference is a direct alias (same alloca as target) or
// indirect (holds a pointer to a global). Returns true if direct alias.
static bool isDirectAlias(AllocaInst* alloca, llvm::Type* referencedType) {
  return alloca->getAllocatedType() == referencedType;
}

// -------------------------------------------------------------------
// Local variable loading
// -------------------------------------------------------------------

llvm::LoadInst* VariableGenerator::createLoadForLocalVar(
    const std::string& varName) {
  AllocaInst* alloca = scopes().findVariable(varName);
  if (alloca) {
    return ctx.builder->CreateLoad(alloca->getAllocatedType(), alloca,
                                   varName.c_str());
  }
  return nullptr;
}

// -------------------------------------------------------------------
// Global variable loading
// -------------------------------------------------------------------

llvm::GlobalVariable* VariableGenerator::globalForSunName(
    const std::string& name) const {
  const std::string& symbol = gen_.externCEmitter().symbolFor(name);
  return module->getGlobalVariable(symbol);
}

llvm::LoadInst* VariableGenerator::createLoadForGlobalVar(
    const std::string& varName) {
  GlobalVariable* globalVar = globalForSunName(varName);
  if (globalVar) {
    return ctx.builder->CreateLoad(globalVar->getValueType(), globalVar,
                                   varName.c_str());
  }
  return nullptr;
}

// -------------------------------------------------------------------
// Reference variable loading
// -------------------------------------------------------------------

llvm::Value* VariableGenerator::createLoadForRef(
    const std::string& varName, const sun::ReferenceType& refType) {
  llvm::Type* referencedLLVMType =
      typeResolver.resolve(refType.getReferencedType());

  AllocaInst* alloca = scopes().findVariable(varName);
  if (alloca) {
    if (isDirectAlias(alloca, referencedLLVMType)) {
      // Direct alias - just load from the alloca (same as target variable)
      return ctx.builder->CreateLoad(referencedLLVMType, alloca,
                                     varName.c_str());
    } else {
      // Indirect reference (to global) - load ptr, then load value
      Value* ptr = ctx.builder->CreateLoad(
          llvm::PointerType::getUnqual(ctx.getContext()), alloca,
          varName + ".ptr");
      return ctx.builder->CreateLoad(referencedLLVMType, ptr,
                                     varName + ".deref");
    }
  }
  return nullptr;
}

void VariableGenerator::createStoreForRef(const std::string& varName,
                                          const sun::ReferenceType& refType,
                                          llvm::Value* value) {
  llvm::Type* referencedLLVMType =
      typeResolver.resolve(refType.getReferencedType());

  AllocaInst* alloca = scopes().findVariable(varName);
  if (!alloca) {
    logAndThrowError("Reference variable not found: " + varName);
  }

  // Resolve the referent's storage, then assign exactly as a direct write to
  // the referent would: assignToVariableSlot drops the old value and MOVES a
  // compound source in. A raw store here would write the source's address
  // over the referent's leading bytes (issue: assigning a String through a
  // ref String corrupted the referent and double-freed).
  Value* target = alloca;
  if (!isDirectAlias(alloca, referencedLLVMType)) {
    // Indirect reference - the alloca holds the referent's address
    target =
        ctx.builder->CreateLoad(llvm::PointerType::getUnqual(ctx.getContext()),
                                alloca, varName + ".ptr");
  }
  assignToVariableSlot(target, value, refType.getReferencedType(), varName);
}

// -------------------------------------------------------------------
// Variable reference codegen
// -------------------------------------------------------------------

Value* VariableGenerator::codegen(const VariableReferenceAST& expr) {
  // Check if this variable is a reference type
  sun::TypePtr varType = expr.getResolvedType();

  // Module types are resolved at compile time - return null as sentinel
  // The actual variable access happens in MemberAccessAST codegen
  if (varType && varType->isModule()) {
    // Return a null pointer as a sentinel - MemberAccessAST will handle it
    return llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(ctx.getContext()));
  }

  if (varType && varType->isReference()) {
    const auto* refType = static_cast<const sun::ReferenceType*>(varType.get());

    // A global reference stores the native pointer in global storage.
    if (GlobalVariable* global = globalForSunName(expr.getMangledName())) {
      Value* pointer = ctx.builder->CreateLoad(
          global->getValueType(), global, expr.getName() + ".ref.ptr");
      sun::TypePtr referenced = refType->getReferencedType();
      if (referenced->isArray() || referenced->isClass() ||
          referenced->isInterface() ||
          CodegenVisitor::isPayloadEnum(referenced)) {
        return pointer;
      }
      llvm::Type* referencedType = typeResolver.resolve(referenced);
      return ctx.builder->CreateLoad(referencedType, pointer,
                                     expr.getName() + ".deref");
    }

    // For references to arrays, we need to return a pointer to the fat struct
    // The alloca holds the pointer value - load it to get the actual pointer
    if (refType->getReferencedType()->isArray()) {
      AllocaInst* alloca = scopes().findVariable(expr.getName());
      if (alloca) {
        // Load the pointer from the alloca - this gives us ptr to fat struct
        // IndexExprAST will then load the fat struct through this pointer
        llvm::Type* ptrType = llvm::PointerType::getUnqual(ctx.getContext());
        return ctx.builder->CreateLoad(ptrType, alloca,
                                       expr.getName() + ".ref.ptr");
      }
      logAndThrowError("Array ref variable not found: " + expr.getName());
    }

    // For references to class/interface/payload-enum types, return the
    // pointer (not the struct value). Classes need their address for field
    // access and method calls, just like local class variables return their
    // alloca.
    if (refType->getReferencedType()->isClass() ||
        refType->getReferencedType()->isInterface() ||
        CodegenVisitor::isPayloadEnum(refType->getReferencedType())) {
      AllocaInst* alloca = scopes().findVariable(expr.getName());
      if (alloca) {
        llvm::Type* ptrType = llvm::PointerType::getUnqual(ctx.getContext());
        return ctx.builder->CreateLoad(ptrType, alloca,
                                       expr.getName() + ".ref.ptr");
      }
      logAndThrowError("Class ref variable not found: " + expr.getName());
    }

    if (Value* val = createLoadForRef(expr.getName(), *refType)) {
      return val;
    }
    logAndThrowError("Reference variable not found: " + expr.getName());
  }

  // For array types, load the fat struct value
  // Arrays are now represented as { ptr data, i32 ndims, ptr dims }
  if (varType && varType->isArray()) {
    AllocaInst* alloca = scopes().findVariable(expr.getName());
    if (alloca) {
      llvm::StructType* fatType =
          sun::ArrayType::getArrayStructType(ctx.getContext());
      return ctx.builder->CreateLoad(fatType, alloca, expr.getName() + ".fat");
    }
    // Check global arrays
    GlobalVariable* gv = module->getGlobalVariable(expr.getName());
    if (gv) {
      llvm::StructType* fatType =
          sun::ArrayType::getArrayStructType(ctx.getContext());
      return ctx.builder->CreateLoad(fatType, gv, expr.getName() + ".fat");
    }
    // [ref arr] capture: the slot address points at the original fat struct
    if (Value* addr = functionGen().createCaptureSlotAddress(expr.getName())) {
      llvm::StructType* fatType =
          sun::ArrayType::getArrayStructType(ctx.getContext());
      return ctx.builder->CreateLoad(fatType, addr, expr.getName() + ".fat");
    }
    logAndThrowError("Array variable not found: " + expr.getName());
  }

  // For class, payload-enum and thread-handle types, return the alloca
  // pointer (not load the struct value). Methods expect 'this' as a pointer
  // to the struct; match destructuring GEPs payloads out of the storage;
  // join() marks the handle slot joined through its address. Indirect
  // bindings (compound match payloads) yield the borrowed slot's address
  // instead.
  if (varType &&
      (varType->isClass() || CodegenVisitor::isPayloadEnum(varType))) {
    if (Value* addr = scopes().compoundStorageAddress(expr.getName())) {
      return addr;
    }
    // Check for global class variables
    GlobalVariable* gv = globalForSunName(expr.getMangledName());
    if (!gv) gv = globalForSunName(expr.getName());
    if (gv) {
      // Return the global variable pointer directly (same semantics as alloca)
      return gv;
    }
    // [ref c] capture: the slot address is the original object pointer
    if (Value* addr = functionGen().createCaptureSlotAddress(expr.getName())) {
      return addr;
    }
    // Fall through to error if not found
  }

  // For interface types, return the alloca pointer (like classes)
  // Interface method dispatch expects a pointer to the fat struct { ptr, ptr }
  // so it can load and extract data/vtable pointers
  if (varType && varType->isInterface()) {
    if (Value* addr = scopes().compoundStorageAddress(expr.getName())) {
      return addr;
    }
    GlobalVariable* gv = globalForSunName(expr.getMangledName());
    if (!gv) gv = globalForSunName(expr.getName());
    if (gv) {
      return gv;
    }
    // [ref i] capture: the slot address is the original fat pointer address
    if (Value* addr = functionGen().createCaptureSlotAddress(expr.getName())) {
      return addr;
    }
    // Fall through to error if not found
  }

  // Non-reference variable - regular load
  llvm::LoadInst* loadVarInst = createLoadForLocalVar(expr.getName());
  if (loadVarInst) return loadVarInst;

  Value* cv = functionGen().createLoadVarFromClosure(expr.getName());
  if (cv) return cv;

  // Use qualified name for module-qualified globals
  llvm::LoadInst* loadInst = createLoadForGlobalVar(expr.getMangledName());
  if (loadInst) return loadInst;

  // Check for named functions using qualified name from semantic analysis
  // The qualified name handles using imports (e.g., hash_i64 -> sun_hash_i64)
  const std::string& funcName = expr.getMangledName();
  if (Function* func = functions().lookupCallTarget(funcName)) {
    return func;
  }

  // Enhanced error with both names for debugging
  logAndThrowError("Global variable not found in module: " + expr.getName() +
                       " (qualifiedName='" + expr.getMangledName() + "')",
                   expr.getLocation());
}

// -------------------------------------------------------------------
// Variable assignment codegen
// -------------------------------------------------------------------

Value* VariableGenerator::codegen(const VariableAssignmentAST& expr) {
  // Check if the value is a lambda literal - need special handling
  bool isLambdaLiteral = expr.getValue()->isLambda();
  BasicBlock* savedBlock = nullptr;

  if (isLambdaLiteral) {
    savedBlock = ctx.builder->GetInsertBlock();
  }

  AllocaInst* alloca = scopes().findVariable(expr.getName());
  if (alloca) {
    // Check if this is a reference type
    sun::TypePtr varType = expr.getResolvedType();

    if (varType && varType->isReference()) {
      const auto* refType =
          static_cast<const sun::ReferenceType*>(varType.get());
      Value* value = codegen(*expr.getValue());
      createStoreForRef(expr.getName(), *refType, value);
      return value;
    }

    Value* value = codegen(*expr.getValue());
    if (isLambdaLiteral && savedBlock) {
      ctx.builder->SetInsertPoint(savedBlock);
      // For lambda literals, codegenLambda returns an alloca containing the
      // closure struct. We need to load the struct before storing it.
      if (auto* valueAlloca = llvm::dyn_cast<AllocaInst>(value)) {
        value = ctx.builder->CreateLoad(valueAlloca->getAllocatedType(),
                                        valueAlloca, "closure.load");
      }
    }

    assignToVariableSlot(alloca, value, varType, expr.getName());
    return value;
  }

  // Captured variables: store through the capture slot address. For [ref x]
  // that is the original variable's storage, and for an owned capture it is
  // the closure's own; an implicit by-value capture is rejected in semantic
  // analysis before reaching here.
  if (Value* slotAddr =
          functionGen().createCaptureSlotAddress(expr.getName())) {
    Value* value = codegen(*expr.getValue());
    if (isLambdaLiteral && savedBlock) {
      ctx.builder->SetInsertPoint(savedBlock);
      // For lambda literals, codegenLambda returns an alloca containing the
      // closure struct. We need to load the struct before storing it.
      if (auto* valueAlloca = llvm::dyn_cast<AllocaInst>(value)) {
        value = ctx.builder->CreateLoad(valueAlloca->getAllocatedType(),
                                        valueAlloca, "closure.load");
      }
    }
    // A compound value arrives as an address, so go through the same store
    // path a local uses: it drops what the slot held and moves the new value
    // in rather than storing the pointer itself.
    assignToVariableSlot(slotAddr, value, expr.getResolvedType(),
                         expr.getName());
    return value;
  }

  // Check for global variable: mangled name first (module-qualified), then
  // the name as written (root-level globals)
  GlobalVariable* gv = globalForSunName(expr.getMangledName());
  if (!gv) gv = globalForSunName(expr.getName());
  if (gv) {
    Value* value = codegen(*expr.getValue());
    if (isLambdaLiteral && savedBlock) {
      ctx.builder->SetInsertPoint(savedBlock);
      // For lambda literals, codegenLambda returns an alloca containing the
      // closure struct. We need to load the struct before storing it.
      if (auto* valueAlloca = llvm::dyn_cast<AllocaInst>(value)) {
        value = ctx.builder->CreateLoad(valueAlloca->getAllocatedType(),
                                        valueAlloca, "closure.load");
      }
    }
    assignToVariableSlot(gv, value, expr.getResolvedType(), expr.getName());
    return value;
  }

  logAndThrowError("Unknown variable name in assignment: " + expr.getName());
}

// Compound values reach here as an address (codegen of a class or payload-enum
// expression yields the object's storage, not the struct). Storing that address
// would write a pointer over the object's leading bytes — silently, since a
// two-word class is exactly pointer-sized. Instead the overwritten value is
// dropped (it reaches the end of its life here, exactly as at scope exit) and
// the source is MOVED in: applyMoveSemantics loads the struct and invalidates
// the source so its own drop is a no-op. The borrow checker already rejects
// later uses of the source.
void VariableGenerator::assignToVariableSlot(Value* slot, Value* value,
                                             const sun::TypePtr& varType,
                                             const std::string& name) {
  bool compound =
      varType &&
      (varType->isClass() || CodegenVisitor::isPayloadEnum(varType)) &&
      value->getType()->isPointerTy();
  if (compound) {
    // Self-assignment would drop the object and then copy from the corpse;
    // it has no effect, so emit nothing.
    if (value == slot) return;
    scopes().emitDropInPlace(varType, slot, name);
    value = gen_.applyMoveSemantics(value, varType);
  }
  layout::storeIntoSlot(*ctx.builder, module->getDataLayout(), slot, value,
                        varType);
}
