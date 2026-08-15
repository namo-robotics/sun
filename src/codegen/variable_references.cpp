// variable_references.cpp - Variable reference and assignment codegen methods

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"

using namespace llvm;

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

llvm::LoadInst* CodegenVisitor::createLoadForLocalVar(
    const std::string& varName) {
  AllocaInst* alloca = findVariable(varName);
  if (alloca) {
    return ctx.builder->CreateLoad(alloca->getAllocatedType(), alloca,
                                   varName.c_str());
  }
  return nullptr;
}

// -------------------------------------------------------------------
// Global variable loading
// -------------------------------------------------------------------

llvm::LoadInst* CodegenVisitor::createLoadForGlobalVar(
    const std::string& varName) {
  GlobalVariable* globalVar = module->getGlobalVariable(varName);
  if (globalVar) {
    return ctx.builder->CreateLoad(globalVar->getValueType(), globalVar,
                                   varName.c_str());
  }
  return nullptr;
}

// -------------------------------------------------------------------
// Reference variable loading
// -------------------------------------------------------------------

llvm::Value* CodegenVisitor::createLoadForRef(
    const std::string& varName, const sun::ReferenceType& refType) {
  llvm::Type* referencedLLVMType =
      typeResolver.resolve(refType.getReferencedType());

  AllocaInst* alloca = findVariable(varName);
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

void CodegenVisitor::createStoreForRef(const std::string& varName,
                                       const sun::ReferenceType& refType,
                                       llvm::Value* value) {
  llvm::Type* referencedLLVMType =
      typeResolver.resolve(refType.getReferencedType());

  AllocaInst* alloca = findVariable(varName);
  if (!alloca) {
    logAndThrowError("Reference variable not found: " + varName);
  }

  if (isDirectAlias(alloca, referencedLLVMType)) {
    // Direct alias - store directly to the alloca (same as target)
    ctx.builder->CreateStore(value, alloca);
  } else {
    // Indirect reference (to global) - load ptr, then store through it
    Value* ptr =
        ctx.builder->CreateLoad(llvm::PointerType::getUnqual(ctx.getContext()),
                                alloca, varName + ".ptr");
    ctx.builder->CreateStore(value, ptr);
  }
}

// -------------------------------------------------------------------
// Variable reference codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const VariableReferenceAST& expr) {
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

    // For references to arrays, we need to return a pointer to the fat struct
    // The alloca holds the pointer value - load it to get the actual pointer
    if (refType->getReferencedType()->isArray()) {
      AllocaInst* alloca = findVariable(expr.getName());
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
        isPayloadEnum(refType->getReferencedType())) {
      AllocaInst* alloca = findVariable(expr.getName());
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
    AllocaInst* alloca = findVariable(expr.getName());
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
    if (Value* addr = createCaptureSlotAddress(expr.getName())) {
      llvm::StructType* fatType =
          sun::ArrayType::getArrayStructType(ctx.getContext());
      return ctx.builder->CreateLoad(fatType, addr, expr.getName() + ".fat");
    }
    logAndThrowError("Array variable not found: " + expr.getName());
  }

  // For class and payload-enum types, return the alloca pointer (not load
  // the struct value). Methods expect 'this' as a pointer to the struct;
  // match destructuring GEPs payloads out of the storage. Indirect bindings
  // (compound match payloads) yield the borrowed slot's address instead.
  if (varType && (varType->isClass() || isPayloadEnum(varType))) {
    if (Value* addr = compoundStorageAddress(expr.getName())) {
      return addr;
    }
    // Check for global class variables
    GlobalVariable* gv = module->getGlobalVariable(expr.getName());
    if (!gv) {
      gv = module->getGlobalVariable(expr.getMangledName());
    }
    if (gv) {
      // Return the global variable pointer directly (same semantics as alloca)
      return gv;
    }
    // [ref c] capture: the slot address is the original object pointer
    if (Value* addr = createCaptureSlotAddress(expr.getName())) {
      return addr;
    }
    // Fall through to error if not found
  }

  // For interface types, return the alloca pointer (like classes)
  // Interface method dispatch expects a pointer to the fat struct { ptr, ptr }
  // so it can load and extract data/vtable pointers
  if (varType && varType->isInterface()) {
    if (Value* addr = compoundStorageAddress(expr.getName())) {
      return addr;
    }
    GlobalVariable* gv = module->getGlobalVariable(expr.getName());
    if (!gv) {
      gv = module->getGlobalVariable(expr.getMangledName());
    }
    if (gv) {
      return gv;
    }
    // [ref i] capture: the slot address is the original fat pointer address
    if (Value* addr = createCaptureSlotAddress(expr.getName())) {
      return addr;
    }
    // Fall through to error if not found
  }

  // Non-reference variable - regular load
  llvm::LoadInst* loadVarInst = createLoadForLocalVar(expr.getName());
  if (loadVarInst) return loadVarInst;

  Value* cv = createLoadVarFromClosure(expr.getName());
  if (cv) return cv;

  // Use qualified name for module-qualified globals
  llvm::LoadInst* loadInst = createLoadForGlobalVar(expr.getMangledName());
  if (loadInst) return loadInst;

  // Check for named functions using qualified name from semantic analysis
  // The qualified name handles using imports (e.g., hash_i64 -> sun_hash_i64)
  const std::string& funcName = expr.getMangledName();
  if (Function* func = module->getFunction(funcName)) {
    return func;
  }

  // Enhanced error with both names for debugging
  logAndThrowError("Global variable not found in module: " + expr.getName() +
                   " (qualifiedName='" + expr.getMangledName() + "')");
}

// -------------------------------------------------------------------
// Variable assignment codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const VariableAssignmentAST& expr) {
  // Named functions cannot be assigned to variables - only lambdas can
  if (expr.getValue()->isFunction()) {
    logAndThrowError(
        "Cannot assign a named function to a variable. Use a lambda instead: " +
        expr.getName());
  }

  // Check if the value is a lambda literal - need special handling
  bool isLambdaLiteral = expr.getValue()->isLambda();
  BasicBlock* savedBlock = nullptr;

  if (isLambdaLiteral) {
    savedBlock = ctx.builder->GetInsertBlock();
  }

  AllocaInst* alloca = findVariable(expr.getName());
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

    // Assigning to a payload-enum variable: the right-hand side is a storage
    // pointer. The overwritten value is dropped first, then the source is
    // MOVED in (tag-poisoned, tracking released) — never implicitly copied.
    if (varType && isPayloadEnum(varType) &&
        value->getType()->isPointerTy()) {
      if (value == alloca) return value;
      auto& enumType = static_cast<sun::EnumType&>(*varType);
      emitEnumDrop(enumType, alloca);
      markClassAllocationAsDeinited(value);
      value = applyMoveSemantics(value, varType);
    }

    // Assigning to a class variable, where codegen of the right-hand side
    // yields an address rather than the struct itself. Storing that address
    // would write a pointer over the object's leading bytes — silently, since
    // a two-word class is exactly pointer-sized.
    if (varType && varType->isClass() && value->getType()->isPointerTy()) {
      // Self-assignment would deinit the object and then copy from the
      // corpse; it has no effect, so emit nothing.
      if (value == alloca) return value;

      auto* classType = static_cast<sun::ClassType*>(varType.get());

      // The value being overwritten reaches the end of its life here, so it
      // is deinitialized exactly as it would be at scope exit.
      emitDeinitCall(classType, alloca);
      emitFieldDeinit(alloca, classType, expr.getName());

      // Assignment moves, which the borrow checker already enforces (it
      // rejects use of the source afterwards). applyMoveSemantics loads the
      // struct and zeroes the source so its own deinit becomes a no-op,
      // matching how by-value arguments are handled.
      value = applyMoveSemantics(value, varType);
    }

    storeIntoSlot(alloca, value, varType);
    return value;
  }

  // Captured variables: store through the capture slot address (for [ref x]
  // captures that is the original variable's storage; by-value captures are
  // rejected in semantic analysis before reaching here)
  if (Value* slotAddr = createCaptureSlotAddress(expr.getName())) {
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
    ctx.builder->CreateStore(value, slotAddr);
    return value;
  }

  // Check for global variable
  GlobalVariable* gv = module->getGlobalVariable(expr.getName());
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
    ctx.builder->CreateStore(value, gv);
    return value;
  }

  logAndThrowError("Unknown variable name in assignment: " + expr.getName());
}
