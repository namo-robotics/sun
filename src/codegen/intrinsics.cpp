// src/codegen/intrinsics.cpp - Intrinsic function codegen
//
// This file contains codegen for:
// - Generic intrinsics: _sizeof<T>, _init<T>, _load<T>, _store<T>,
//                       _static_ptr_data<T>, _static_ptr_len<T>, _ptr_as_raw<T>
// - Non-generic intrinsics: _load_i64, _store_i64, _malloc, _free

#include "intrinsics.h"

#include "codegen_visitor.h"
#include "error.h"

using namespace llvm;

// -------------------------------------------------------------------
// Generic intrinsics codegen
// Called from codegen(GenericCallAST) for _sizeof, _own, _init, _load, _store
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenSizeofIntrinsic(sun::TypePtr targetType) {
  // _sizeof<T>() returns the byte size of type T as i64
  if (!targetType) {
    logAndThrowError("Type argument not resolved for _sizeof<T>");
    return nullptr;
  }

  llvm::Type* llvmType = targetType->toLLVMType(ctx.getContext());
  const llvm::DataLayout& DL = module->getDataLayout();
  uint64_t size = DL.getTypeAllocSize(llvmType);

  return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()), size);
}

Value* CodegenVisitor::codegenInitIntrinsic(
    sun::TypePtr targetType,
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _init<T>(ptr, args...) constructs T at ptr with forwarded arguments
  if (args.empty()) {
    logAndThrowError("_init<T>() requires a pointer argument");
    return nullptr;
  }

  llvm::Value* rawPtr = codegen(*args[0]);
  if (!rawPtr) return nullptr;

  if (!targetType) {
    logAndThrowError("Type argument not resolved for _init<T>");
    return nullptr;
  }

  // Only class types have constructors
  if (!targetType->isClass()) {
    // For non-class types, _init is a no-op (primitives are zero-initialized)
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
  }

  auto* classType = static_cast<sun::ClassType*>(targetType.get());

  // Collect constructor arguments, expanding any pack expansions
  std::vector<Value*> ctorArgs;
  ctorArgs.push_back(rawPtr);  // 'this' pointer

  // Skip args[0] (the pointer), process remaining args with pack expansion
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i]->getType() == ASTNodeType::PACK_EXPANSION) {
      // Expand the pack: first check variadicArgsStack (for call-site
      // expansion)
      const auto& packExpr = static_cast<const PackExpansionAST&>(*args[i]);
      const std::string& packName = packExpr.getPackName();

      // Find the variadic args for this pack name in the stack
      bool found = false;
      for (auto it = variadicArgsStack.rbegin(); it != variadicArgsStack.rend();
           ++it) {
        if (it->first == packName) {
          // Load each variadic argument from its allocated slot
          size_t varIdx = 0;
          for (const auto* varArgExpr : it->second) {
            std::string paramName = packName + "." + std::to_string(varIdx);
            AllocaInst* alloca = findVariable(paramName);
            if (alloca) {
              sun::TypePtr argType = varArgExpr->getResolvedType();
              llvm::Type* llvmType = typeResolver.resolve(argType);
              Value* argVal =
                  ctx.builder->CreateLoad(llvmType, alloca, paramName + ".val");
              ctorArgs.push_back(argVal);
            } else {
              logAndThrowError("Cannot find variadic argument: " + paramName);
              return nullptr;
            }
            ++varIdx;
          }
          found = true;
          break;
        }
      }

      // If not in stack, check if we're inside a variadic function body
      // Look for indexed variables in scope (e.g., "args.0", "args.1")
      if (!found) {
        size_t varIdx = 0;
        while (true) {
          std::string paramName = packName + "." + std::to_string(varIdx);
          AllocaInst* alloca = findVariable(paramName);
          if (!alloca) break;  // No more variadic params

          // Get the alloca's type to determine how to load
          llvm::Type* allocaType = alloca->getAllocatedType();
          Value* argVal =
              ctx.builder->CreateLoad(allocaType, alloca, paramName + ".val");
          ctorArgs.push_back(argVal);
          ++varIdx;
        }
        // Even if varIdx==0 (empty pack), that's valid - check if we're in a
        // variadic method by looking for any pack variables or the pack marker
        if (varIdx > 0) {
          found = true;
        } else {
          // Check if this is a valid empty pack expansion by seeing if we're
          // in a method that was declared with variadic params (even if 0 args)
          // The marker is that the pack name exists but has no indexed vars
          // This is valid - the pack simply expands to nothing
          found = true;  // Empty pack expansion is valid
        }
      }

      // Empty pack expansion is valid - nothing to add to ctorArgs
      // (the 'found' check below is now always true for pack expansions)
    } else {
      // Regular argument
      Value* argVal = codegen(*args[i]);
      if (!argVal) return nullptr;
      ctorArgs.push_back(argVal);
    }
  }

  // Look up the constructor (init method)
  std::string ctorMangledName = classType->getMangledMethodName("init");
  Function* ctorFunc = nullptr;
  size_t ctorArgCount = ctorArgs.size();  // includes 'this' pointer

  // Try to find existing constructor matching argument count
  for (int i = 0; i < 10; ++i) {
    std::string ctorName =
        (i == 0) ? ctorMangledName : ctorMangledName + "." + std::to_string(i);
    Function* candidate = module->getFunction(ctorName);
    if (!candidate) break;

    // Constructor takes 'this' + user args
    if (candidate->arg_size() == ctorArgCount) {
      ctorFunc = candidate;
      break;
    }
  }

  // If not found, try to create a declaration for it
  // This handles cases where the class is processed later in codegen order
  if (!ctorFunc) {
    const sun::ClassMethod* initMethod = classType->getMethod("init");
    if (initMethod && initMethod->paramTypes.size() + 1 == ctorArgCount) {
      // Build parameter types for the constructor
      std::vector<llvm::Type*> paramTypes;
      paramTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // this
      for (const auto& paramType : initMethod->paramTypes) {
        paramTypes.push_back(typeResolver.resolve(paramType));
      }
      FunctionType* funcType = FunctionType::get(
          Type::getVoidTy(ctx.getContext()), paramTypes, false);
      ctorFunc = Function::Create(funcType, Function::ExternalLinkage,
                                  ctorMangledName, module);
    }
  }

  if (ctorFunc) {
    ctx.builder->CreateCall(ctorFunc, ctorArgs);
  }

  return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
}

Value* CodegenVisitor::codegenLoadIntrinsic(
    sun::TypePtr targetType,
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _load<T>(ptr, index) loads element T at ptr[index]
  if (args.size() != 2) {
    logAndThrowError("_load<T>(ptr, index) requires 2 arguments");
    return nullptr;
  }

  if (!targetType) {
    logAndThrowError("Type argument not resolved for _load<T>");
    return nullptr;
  }

  llvm::Value* rawPtr = codegen(*args[0]);
  llvm::Value* index = codegen(*args[1]);
  if (!rawPtr || !index) return nullptr;

  llvm::Type* elemType = targetType->toLLVMType(ctx.getContext());

  // Calculate address: ptr + index * sizeof(T)
  llvm::Value* elemPtr =
      ctx.builder->CreateGEP(elemType, rawPtr, index, "elem.ptr");

  // For class types, return the pointer to the element (classes are
  // addressable)
  if (targetType->isClass()) {
    return elemPtr;
  }

  return ctx.builder->CreateLoad(elemType, elemPtr, "elem.val");
}

Value* CodegenVisitor::codegenStoreIntrinsic(
    sun::TypePtr targetType,
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _store<T>(ptr, index, value) stores value at ptr[index]
  if (args.size() != 3) {
    logAndThrowError("_store<T>(ptr, index, value) requires 3 arguments");
    return nullptr;
  }

  if (!targetType) {
    logAndThrowError("Type argument not resolved for _store<T>");
    return nullptr;
  }

  llvm::Value* rawPtr = codegen(*args[0]);
  llvm::Value* index = codegen(*args[1]);
  llvm::Value* value = codegen(*args[2]);
  if (!rawPtr || !index || !value) return nullptr;

  llvm::Type* elemType = targetType->toLLVMType(ctx.getContext());

  // Calculate address: ptr + index * sizeof(T)
  llvm::Value* elemPtr =
      ctx.builder->CreateGEP(elemType, rawPtr, index, "elem.ptr");

  // For class types, value may be a pointer (alloca) or already a struct value
  // (e.g. from a ref parameter that was auto-dereferenced)
  if (targetType->isClass()) {
    llvm::Value* structVal = value;
    if (value->getType()->isPointerTy()) {
      structVal = ctx.builder->CreateLoad(elemType, value, "struct.val");
    }
    ctx.builder->CreateStore(structVal, elemPtr);
    return structVal;
  }

  // For interface types (fat pointer struct), same handling as class types
  if (targetType->isInterface()) {
    llvm::Value* structVal = value;
    if (value->getType()->isPointerTy()) {
      structVal = ctx.builder->CreateLoad(elemType, value, "iface.val");
    }
    ctx.builder->CreateStore(structVal, elemPtr);
    return structVal;
  }

  ctx.builder->CreateStore(value, elemPtr);
  return value;
}

Value* CodegenVisitor::codegenStaticPtrDataIntrinsic(
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _static_ptr_data<T>(static_ptr<T>) extracts data pointer from fat struct
  // static_ptr<T> is struct { ptr data, i64 length }
  // Returns element 0 (the raw pointer)
  if (args.size() != 1) {
    logAndThrowError("_static_ptr_data<T>() requires exactly one argument");
    return nullptr;
  }

  llvm::Value* fatPtr = codegen(*args[0]);
  if (!fatPtr) return nullptr;

  // Extract element 0 (the data pointer) from the fat struct
  return ctx.builder->CreateExtractValue(fatPtr, 0, "static_ptr.data");
}

Value* CodegenVisitor::codegenStaticPtrLenIntrinsic(
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _static_ptr_len<T>(static_ptr<T>) extracts length from fat struct
  // static_ptr<T> is struct { ptr data, i64 length }
  // Returns element 1 (the length as i64)
  if (args.size() != 1) {
    logAndThrowError("_static_ptr_len<T>() requires exactly one argument");
    return nullptr;
  }

  llvm::Value* fatPtr = codegen(*args[0]);
  if (!fatPtr) return nullptr;

  // Extract element 1 (the length) from the fat struct
  return ctx.builder->CreateExtractValue(fatPtr, 1, "static_ptr.len");
}

Value* CodegenVisitor::codegenPtrAsRawIntrinsic(
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _ptr_as_raw<T>(ptr<T>) returns raw_ptr<T> without transferring ownership
  // Like unique_ptr::get() - returns the underlying pointer
  // ptr<T> in LLVM is just a pointer, so just return the value directly
  if (args.size() != 1) {
    logAndThrowError("_ptr_as_raw<T>() requires exactly one argument");
    return nullptr;
  }

  llvm::Value* ownedPtr = codegen(*args[0]);
  if (!ownedPtr) return nullptr;

  // ptr<T> is represented as a simple pointer in LLVM
  // Just return it - no transformation needed
  return ownedPtr;
}

Value* CodegenVisitor::codegenIsIntrinsic(
    const std::string& targetName,
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _is<T>(value) - compile-time type check
  // Returns true/false as a compile-time constant
  //
  // Built-in type traits (pseudo-interfaces):
  //   _Integer  - i8, i16, i32, i64, u8, u16, u32, u64
  //   _Signed   - i8, i16, i32, i64
  //   _Unsigned - u8, u16, u32, u64
  //   _Float    - f32, f64
  //   _Numeric  - _Integer + _Float
  //   _Primitive- _Numeric + bool
  //
  // Concrete types: exact match (e.g., _is<i64>(x), _is<Point>(obj))
  // Interfaces: check implementsInterface (e.g., _is<IHashable>(key))

  if (args.size() != 1) {
    logAndThrowError("_is<T>(value) requires exactly one argument");
    return nullptr;
  }

  // Get the type of the value being checked
  sun::TypePtr valueType = args[0]->getResolvedType();
  if (!valueType) {
    logAndThrowError("Cannot determine type of argument to _is<T>");
    return nullptr;
  }

  // Unwrap reference types
  valueType = sun::unwrapRef(valueType);

  bool result = false;

  // Check built-in type traits first
  sun::TypeTrait trait = sun::getTypeTrait(targetName);
  switch (trait) {
    case sun::TypeTrait::Integer:
      result = valueType->isSigned() || valueType->isUnsigned();
      break;
    case sun::TypeTrait::Signed:
      result = valueType->isSigned();
      break;
    case sun::TypeTrait::Unsigned:
      result = valueType->isUnsigned();
      break;
    case sun::TypeTrait::Float:
      result = valueType->isFloat32() || valueType->isFloat64();
      break;
    case sun::TypeTrait::Numeric:
      result = valueType->isNumeric();
      break;
    case sun::TypeTrait::Primitive:
      result = valueType->isPrimitive();
      break;
    case sun::TypeTrait::None:
      // Not a built-in trait - check for concrete type or interface
      if (valueType->isClass()) {
        auto* classType = static_cast<sun::ClassType*>(valueType.get());
        // Check if targetName is an interface this class implements
        if (classType->implementsInterface(targetName)) {
          result = true;
        } else {
          // Check for exact class name match
          result = classType->getName() == targetName;
        }
      } else {
        // For non-class types, check exact type name match
        result = valueType->toString() == targetName;
      }
      break;
  }

  // Return compile-time constant
  return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx.getContext()),
                                result ? 1 : 0);
}

// -------------------------------------------------------------------
// Non-generic intrinsics: _load_i64, _store_i64, _malloc, _free
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenLoadI64Intrinsic(const CallExprAST& expr) {
  // _load_i64(ptr, index) - load i64 from ptr at element offset index
  const auto& args = expr.getArgs();
  if (args.size() != 2) {
    logAndThrowError("_load_i64 expects 2 arguments: (ptr, index)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  llvm::Value* index = codegen(*args[1]);
  if (!ptr || !index) return nullptr;

  auto* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  llvm::Value* elemPtr = ctx.builder->CreateGEP(i64Ty, ptr, index, "i64.ptr");
  return ctx.builder->CreateLoad(i64Ty, elemPtr, "i64.val");
}

Value* CodegenVisitor::codegenStoreI64Intrinsic(const CallExprAST& expr) {
  // _store_i64(ptr, index, value) - store i64 to ptr at element offset index
  const auto& args = expr.getArgs();
  if (args.size() != 3) {
    logAndThrowError("_store_i64 expects 3 arguments: (ptr, index, value)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  llvm::Value* index = codegen(*args[1]);
  llvm::Value* value = codegen(*args[2]);
  if (!ptr || !index || !value) return nullptr;

  auto* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  llvm::Value* elemPtr = ctx.builder->CreateGEP(i64Ty, ptr, index, "i64.ptr");
  ctx.builder->CreateStore(value, elemPtr);
  return value;
}

Value* CodegenVisitor::codegenMallocIntrinsic(const CallExprAST& expr) {
  // _malloc(size) - allocate size bytes, returns raw_ptr<i8>
  const auto& args = expr.getArgs();
  if (args.size() != 1) {
    logAndThrowError("_malloc expects 1 argument: (size)");
    return nullptr;
  }

  llvm::Value* size = codegen(*args[0]);
  if (!size) return nullptr;

  // Get or declare libc malloc: void* malloc(size_t)
  auto* i8PtrTy = llvm::PointerType::getUnqual(ctx.getContext());
  auto* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  llvm::FunctionType* mallocType =
      llvm::FunctionType::get(i8PtrTy, {i64Ty}, false);
  llvm::FunctionCallee mallocFunc =
      module->getOrInsertFunction("malloc", mallocType);

  return ctx.builder->CreateCall(mallocFunc, {size}, "malloc.result");
}

Value* CodegenVisitor::codegenFreeIntrinsic(const CallExprAST& expr) {
  // _free(ptr) - free previously allocated memory
  const auto& args = expr.getArgs();
  if (args.size() != 1) {
    logAndThrowError("_free expects 1 argument: (ptr)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  if (!ptr) return nullptr;

  // Get or declare libc free: void free(void*)
  auto* i8PtrTy = llvm::PointerType::getUnqual(ctx.getContext());
  auto* voidTy = llvm::Type::getVoidTy(ctx.getContext());
  llvm::FunctionType* freeType =
      llvm::FunctionType::get(voidTy, {i8PtrTy}, false);
  llvm::FunctionCallee freeFunc = module->getOrInsertFunction("free", freeType);

  ctx.builder->CreateCall(freeFunc, {ptr});
  return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
}
