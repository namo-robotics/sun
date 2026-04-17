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

    // For references to class/interface types, return the pointer (not the
    // struct value). Classes need their address for field access and method
    // calls, just like local class variables return their alloca.
    if (refType->getReferencedType()->isClass() ||
        refType->getReferencedType()->isInterface()) {
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
    logAndThrowError("Array variable not found: " + expr.getName());
  }

  // For class types, return the alloca pointer (not load the struct value)
  // Methods expect 'this' as a pointer to the struct
  if (varType && varType->isClass()) {
    AllocaInst* alloca = findVariable(expr.getName());
    if (alloca) {
      // Return the alloca directly - it's the pointer to the struct
      return alloca;
    }
    // Check for global class variables
    GlobalVariable* gv = module->getGlobalVariable(expr.getName());
    if (!gv) {
      gv = module->getGlobalVariable(getMangledName(expr.getName()));
    }
    if (gv) {
      // Return the global variable pointer directly (same semantics as alloca)
      return gv;
    }
    // Fall through to error if not found
  }

  // For interface types, return the alloca pointer (like classes)
  // Interface method dispatch expects a pointer to the fat struct { ptr, ptr }
  // so it can load and extract data/vtable pointers
  if (varType && varType->isInterface()) {
    AllocaInst* alloca = findVariable(expr.getName());
    if (alloca) {
      return alloca;
    }
    GlobalVariable* gv = module->getGlobalVariable(expr.getName());
    if (!gv) {
      gv = module->getGlobalVariable(getMangledName(expr.getName()));
    }
    if (gv) {
      return gv;
    }
    // Fall through to error if not found
  }

  // Non-reference variable - regular load
  llvm::LoadInst* loadVarInst = createLoadForLocalVar(expr.getName());
  if (loadVarInst) return loadVarInst;

  Value* cv = createLoadVarFromClosure(expr.getName());
  if (cv) return cv;

  llvm::LoadInst* loadInst = createLoadForGlobalVar(expr.getName());
  if (loadInst) return loadInst;

  // Check for named functions using qualified name from semantic analysis
  // The qualified name handles using imports (e.g., hash_i64 -> sun_hash_i64)
  const std::string& funcName = expr.getQualifiedName();
  if (Function* func = module->getFunction(funcName)) {
    return func;
  }

  // Fallback: Check for using imports (e.g., using Math::square brings square
  // into scope) This handles cases where semantic analysis didn't set the
  // qualified name
  auto it = usingImports.find(expr.getName());
  if (it != usingImports.end()) {
    // Try to find the mangled name as a function
    if (Function* func = module->getFunction(it->second)) {
      return func;
    }
    // Try to find the mangled name as a global variable
    if (GlobalVariable* gv = module->getGlobalVariable(it->second)) {
      return ctx.builder->CreateLoad(gv->getValueType(), gv,
                                     expr.getName().c_str());
    }
  }

  // Fallback: Check wildcard imports (e.g., using Math::* brings everything
  // into scope)
  for (const auto& nsPrefix : wildcardImports) {
    std::string mangledName = nsPrefix + expr.getName();
    if (Function* func = module->getFunction(mangledName)) {
      return func;
    }
    if (GlobalVariable* gv = module->getGlobalVariable(mangledName)) {
      return ctx.builder->CreateLoad(gv->getValueType(), gv,
                                     expr.getName().c_str());
    }
  }

  logAndThrowError("Global variable not found in module: " + expr.getName());
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
    ctx.builder->CreateStore(value, alloca);
    return value;
  }

  Value* closureVal = createLoadVarFromClosure(expr.getName());
  if (closureVal) {
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
    ctx.builder->CreateStore(value, closureVal);
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
