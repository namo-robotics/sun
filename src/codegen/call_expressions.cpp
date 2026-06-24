// call_expressions.cpp - Call expression codegen methods

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"
#include "llvm/IR/InlineAsm.h"
#include "semantic_scope.h"

using namespace llvm;

// Look up an LLVM Function for a class method by name.
// Uses getMangledMethodName (with paramSuffix) first, falls back to plain
// "TypeName_methodName" for legacy/simple cases.
Function* CodegenVisitor::findClassMethod(
    const std::shared_ptr<sun::ClassType>& classType,
    const std::string& typeName, const std::string& methodName) {
  if (classType) {
    if (auto* m = classType->getMethod(methodName)) {
      std::string mangled =
          classType->getMangledMethodName(methodName, m->paramTypes);
      if (auto* f = module->getFunction(mangled)) return f;
    }
  }
  // Fallback: try without paramSuffix
  return module->getFunction(typeName + "_" + methodName);
}

// -------------------------------------------------------------------
// Helper for unwrapping error union from call results
// -------------------------------------------------------------------

// Unwraps error union { i1 isError, T value } from a call result.
// Generates error propagation code for try-catch or error-returning functions.
// Returns the unwrapped inner value.
Value* CodegenVisitor::unwrapCallErrorUnion(Value* callResult) {
  if (!callResult || !callResult->getType()->isStructTy()) {
    return callResult;
  }

  auto* structType = cast<StructType>(callResult->getType());
  // Check if this looks like an error union: { i1, T }
  if (structType->getNumElements() != 2 ||
      !structType->getElementType(0)->isIntegerTy(1)) {
    return callResult;
  }

  Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();

  // Extract isError flag and value
  Value* isError =
      ctx.builder->CreateExtractValue(callResult, 0, "call.isError");
  Value* innerValue =
      ctx.builder->CreateExtractValue(callResult, 1, "call.value");

  // Create blocks for error handling
  BasicBlock* errorBB =
      BasicBlock::Create(ctx.getContext(), "call.error", currentFunc);
  BasicBlock* successBB =
      BasicBlock::Create(ctx.getContext(), "call.success", currentFunc);

  ctx.builder->CreateCondBr(isError, errorBB, successBB);

  // Error block: either propagate to caller or jump to catch block
  ctx.builder->SetInsertPoint(errorBB);

  if (!tryStack.empty()) {
    // Inside a try block: store error result and jump to catch
    ctx.builder->CreateStore(callResult, tryStack.back().errorResultAlloca);
    ctx.builder->CreateBr(tryStack.back().catchBlock);
  } else if (currentFunctionCanError) {
    // In error-returning function: propagate error to caller
    llvm::Type* retType = currentFunc->getReturnType();
    Value* errorStruct = UndefValue::get(retType);
    errorStruct = ctx.builder->CreateInsertValue(
        errorStruct, ConstantInt::getTrue(ctx.getContext()), 0);
    if (currentFunctionValueType) {
      Value* zeroVal = Constant::getNullValue(currentFunctionValueType);
      errorStruct = ctx.builder->CreateInsertValue(errorStruct, zeroVal, 1);
    }
    ctx.builder->CreateRet(errorStruct);
  } else {
    // Not in try block and not in error-returning function
    // Continue to success block (error silently ignored)
    ctx.builder->CreateBr(successBB);
  }

  // Success block: continue with the unwrapped value
  ctx.builder->SetInsertPoint(successBB);
  return innerValue;
}

// -------------------------------------------------------------------
// Helper: Apply move semantics for class arguments passed by value
// -------------------------------------------------------------------

Value* CodegenVisitor::applyMoveSemantics(Value* argVal,
                                          sun::TypePtr argSunType) {
  // Only apply move semantics to class types that are pointers (addressable)
  if (!argSunType || !argSunType->isClass() ||
      !argVal->getType()->isPointerTy()) {
    return argVal;
  }

  auto* classType = static_cast<sun::ClassType*>(argSunType.get());
  llvm::StructType* structType = classType->getStructType(ctx.getContext());

  // Load the struct value from the source
  Value* structVal = ctx.builder->CreateLoad(structType, argVal, "move.val");

  // Move semantics: zero out the source to prevent double-free
  llvm::FunctionCallee memsetFn = module->getOrInsertFunction(
      "memset", FunctionType::get(PointerType::getUnqual(ctx.getContext()),
                                  {PointerType::getUnqual(ctx.getContext()),
                                   Type::getInt32Ty(ctx.getContext()),
                                   Type::getInt64Ty(ctx.getContext())},
                                  false));
  const DataLayout& DL = module->getDataLayout();
  uint64_t structSize = DL.getTypeAllocSize(structType);
  ctx.builder->CreateCall(
      memsetFn,
      {argVal, ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0),
       ConstantInt::get(Type::getInt64Ty(ctx.getContext()), structSize)});

  return structVal;
}

// -------------------------------------------------------------------
// Helper: Create interface fat pointer { data_ptr, vtable_ptr }
// -------------------------------------------------------------------

Value* CodegenVisitor::createInterfaceFatPointer(
    Value* objectPtr, sun::ClassType* classType,
    sun::InterfaceType* ifaceType) {
  // Look up the vtable for this (class, interface) pair
  auto vtableIt =
      vtableGlobals.find({classType->getMangledName(), ifaceType->getName()});
  if (vtableIt == vtableGlobals.end()) {
    logAndThrowError("Vtable not found for class " +
                     classType->getDisplayName() + " implementing interface " +
                     ifaceType->getName());
    return nullptr;
  }
  GlobalVariable* vtableGlobal = vtableIt->second;

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

Value* CodegenVisitor::convertToInterfaceIfNeeded(Value* argVal,
                                                  sun::TypePtr argType,
                                                  sun::TypePtr paramType) {
  // Check if conversion is needed: param is interface and arg is class
  if (!paramType || !paramType->isInterface()) {
    return argVal;  // No conversion needed
  }
  if (!argType || !argType->isClass()) {
    return argVal;  // Can't convert, return as-is
  }

  auto* classType = static_cast<sun::ClassType*>(argType.get());
  auto* ifaceType = static_cast<sun::InterfaceType*>(paramType.get());
  return createInterfaceFatPointer(argVal, classType, ifaceType);
}

// -------------------------------------------------------------------
// Helper: Prepare class argument for ref Interface parameter
// Creates fat pointer on stack and returns pointer to it
// -------------------------------------------------------------------

Value* CodegenVisitor::prepareClassForRefInterface(Value* classPtr,
                                                   sun::TypePtr argType,
                                                   sun::TypePtr paramType) {
  // Check if conversion is needed: param is ref Interface and arg is class
  if (!paramType || !paramType->isReference()) {
    return nullptr;  // Not a ref param
  }
  auto* refType = static_cast<sun::ReferenceType*>(paramType.get());
  sun::TypePtr innerType = refType->getReferencedType();
  if (!innerType || !innerType->isInterface()) {
    return nullptr;  // Not ref Interface
  }
  if (!argType || !argType->isClass()) {
    return nullptr;  // Arg is not a class
  }

  auto* classType = static_cast<sun::ClassType*>(argType.get());
  auto* ifaceType = static_cast<sun::InterfaceType*>(innerType.get());

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
// Helper: Widen numeric types if needed (i32->i64, f32->f64)
// -------------------------------------------------------------------

Value* CodegenVisitor::widenNumericIfNeeded(Value* argVal,
                                            sun::TypePtr paramType) {
  if (!paramType) {
    return argVal;
  }

  llvm::Type* expectedType = typeResolver.resolve(paramType);

  // Integer widening: smaller int -> larger int
  if (argVal->getType()->isIntegerTy() && expectedType->isIntegerTy()) {
    unsigned argBits = argVal->getType()->getIntegerBitWidth();
    unsigned paramBits = expectedType->getIntegerBitWidth();
    if (argBits < paramBits) {
      return ctx.builder->CreateSExt(argVal, expectedType, "widen");
    }
  }
  // Float widening: f32 -> f64
  else if (argVal->getType()->isFloatTy() && expectedType->isDoubleTy()) {
    return ctx.builder->CreateFPExt(argVal, expectedType, "widen");
  }

  return argVal;
}

// -------------------------------------------------------------------
// Helper: Materialize struct return value to caller's stack
// -------------------------------------------------------------------

Value* CodegenVisitor::materializeStructReturn(Value* callResult) {
  if (!callResult || !callResult->getType()->isStructTy()) {
    return callResult;
  }

  auto* structType = cast<StructType>(callResult->getType());

  // Check that it's not an error union { i1, T } - those should be unwrapped
  // first
  bool isErrorUnion = structType->getNumElements() == 2 &&
                      structType->getElementType(0)->isIntegerTy(1);
  if (isErrorUnion) {
    return callResult;
  }

  // Check that it's not a well-known internal struct type
  // (closure, static_ptr, interface_fat, array_struct)
  if (structType->hasName()) {
    StringRef name = structType->getName();
    for (const auto& info : sun::StructNames::All) {
      if (name == info.name) {
        return callResult;
      }
    }
  }

  // This is a compound type (class) returned by value
  // Store it to the caller's stack and return a pointer for addressability
  Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* resultAlloca =
      createEntryBlockAlloca(currentFunc, "ret.struct", structType);
  ctx.builder->CreateStore(callResult, resultAlloca);
  return resultAlloca;
}

// -------------------------------------------------------------------
// Helper: Prepare argument for reference parameter
// -------------------------------------------------------------------

Value* CodegenVisitor::prepareRefArgument(const ExprAST* argExpr,
                                          sun::TypePtr argSunType) {
  // Auto-deref: if argument is raw_ptr<T> and param is ref T, pass the
  // pointer directly
  if (argSunType && argSunType->isRawPointer()) {
    // raw_ptr<T> passed to ref T - pass the pointer value directly
    Value* argVal = codegen(*argExpr);
    return argVal;
  }

  // Variable reference: pass address of variable
  if (auto* varRef = dynamic_cast<const VariableReferenceAST*>(argExpr)) {
    AllocaInst* alloca = findVariable(varRef->getName());
    if (alloca) {
      // If the variable is already a reference, load the pointer and pass it
      if (argSunType && argSunType->isReference()) {
        return ctx.builder->CreateLoad(
            llvm::PointerType::getUnqual(ctx.getContext()), alloca,
            varRef->getName() + ".ptr");
      } else {
        return alloca;
      }
    }
    // Check for global variable
    GlobalVariable* gv = module->getGlobalVariable(varRef->getName());
    if (gv) {
      return gv;
    }
    logAndThrowError("Cannot find variable for reference parameter: " +
                     varRef->getName());
    return nullptr;
  }

  // Array value expression - need to create a temporary alloca
  if (argSunType && argSunType->isArray()) {
    Value* argVal = codegen(*argExpr);
    if (!argVal) return nullptr;
    llvm::StructType* fatType =
        sun::ArrayType::getArrayStructType(ctx.getContext());
    AllocaInst* tempAlloca =
        ctx.builder->CreateAlloca(fatType, nullptr, "arr.temp");
    ctx.builder->CreateStore(argVal, tempAlloca);
    return tempAlloca;
  }

  // Member access (e.g., this.alloc) - codegen gives pointer for class
  // fields, loaded value for primitives
  if (dynamic_cast<const MemberAccessAST*>(argExpr)) {
    Value* val = codegen(*argExpr);
    if (!val) return nullptr;
    if (val->getType()->isPointerTy()) {
      return val;
    }
    AllocaInst* tempAlloca =
        ctx.builder->CreateAlloca(val->getType(), nullptr, "ref.member");
    ctx.builder->CreateStore(val, tempAlloca);
    return tempAlloca;
  }

  // Class temporary passed by ref: create a temporary alloca to hold the value
  // The caller owns this temporary and will deinit it at scope exit.
  // Borrow checker ensures the callee can't escape refs to this temporary.
  if (argSunType && argSunType->isClass()) {
    auto* classType = dynamic_cast<const sun::ClassType*>(argSunType.get());
    if (classType) {
      // Generate the temporary value
      Value* tempVal = codegen(*argExpr);
      if (!tempVal) return nullptr;

      // If codegen returned a pointer, it's already an alloca - use it directly.
      // The original temporary is already tracked for deinit, no need to copy.
      // Copying would cause double-free since both would try to deinit the same
      // owned resources (e.g., Unique<T> pointers).
      if (tempVal->getType()->isPointerTy()) {
        return tempVal;
      }

      // Codegen returned a struct value - need to materialize it in an alloca
      llvm::Type* llvmType = typeResolver.resolve(argSunType);
      AllocaInst* tempAlloca =
          ctx.builder->CreateAlloca(llvmType, nullptr, "ref.temp");
      ctx.builder->CreateStore(tempVal, tempAlloca);

      // Track for cleanup - caller owns the temporary
      auto classTypePtr = std::dynamic_pointer_cast<sun::ClassType>(argSunType);
      if (classTypePtr) {
        trackClassAllocation(tempAlloca, "ref.temp", classTypePtr);
      }

      return tempAlloca;
    }
  }

  // Other temporary expressions - create alloca for the value
  if (argExpr->isTemporary()) {
    Value* tempVal = codegen(*argExpr);
    if (!tempVal) return nullptr;

    AllocaInst* tempAlloca =
        ctx.builder->CreateAlloca(tempVal->getType(), nullptr, "ref.temp");
    ctx.builder->CreateStore(tempVal, tempAlloca);
    return tempAlloca;
  }

  logAndThrowError(
      "Reference parameter must be passed a variable, not an expression");
  return nullptr;
}

// -------------------------------------------------------------------
// Builtin type method dispatch
// Handles: Thread.join(), array.shape(), raw_ptr._get(), static_ptr methods
// Returns nullptr if not a builtin type method (caller should continue).
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenBuiltinTypeMethod(const CallExprAST& expr,
                                                Value* objectPtr,
                                                sun::TypePtr objectType,
                                                const std::string& methodName) {
  if (!objectType) return nullptr;

  // Handle Thread<T>.join()
  if (objectType->isThread()) {
    if (methodName == "join") {
      if (!expr.getArgs().empty()) {
        logAndThrowError("Thread.join() takes no arguments");
        return nullptr;
      }
      auto* threadType = static_cast<sun::ThreadType*>(objectType.get());
      sun::TypePtr resultType = threadType->getResultType();
      return codegenThreadJoin(objectPtr, resultType);
    }
    logAndThrowError("Unknown method on Thread: " + methodName);
    return nullptr;
  }

  // Handle pointer type methods: raw_ptr._get(),
  // static_ptr._get()/.length/.data
  if (objectType->isRawPointer() || objectType->isStaticPointer()) {
    sun::TypePtr pointeeType = nullptr;
    if (objectType->isRawPointer()) {
      pointeeType =
          static_cast<sun::RawPointerType*>(objectType.get())->getPointeeType();
    } else {
      pointeeType = static_cast<sun::StaticPointerType*>(objectType.get())
                        ->getPointeeType();
    }

    // Handle ._get() method for pointer dereferencing
    if (methodName == "_get") {
      if (!expr.getArgs().empty()) {
        logAndThrowError("_get() takes no arguments");
        return nullptr;
      }

      if (objectType->isStaticPointer()) {
        // static_ptr is a fat pointer struct { ptr, i64 }
        // objectPtr is the struct value, extract data ptr and load
        Value* dataPtr =
            ctx.builder->CreateExtractValue(objectPtr, 0, "static_ptr.data");
        auto* staticPtrType =
            static_cast<sun::StaticPointerType*>(objectType.get());
        llvm::Type* pointeeLLVMType =
            staticPtrType->getPointeeType()->toLLVMType(ctx.getContext());
        return ctx.builder->CreateLoad(pointeeLLVMType, dataPtr,
                                       "static_ptr._get");
      } else {
        auto* ptrType = static_cast<sun::RawPointerType*>(objectType.get());
        llvm::Type* pointeeLLVMType =
            ptrType->getPointeeType()->toLLVMType(ctx.getContext());
        return ctx.builder->CreateLoad(pointeeLLVMType, objectPtr,
                                       "raw_ptr._get");
      }
    }

    // For pointer-to-class, we don't handle other methods here - return nullptr
    // to continue with class method dispatch
    if (pointeeType && pointeeType->isClass()) {
      return nullptr;  // Let caller handle as class method
    }
  }

  // Not a builtin type method
  return nullptr;
}

// -------------------------------------------------------------------
// Module-qualified function call: mymod.foo() -> mymod_foo()
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenModuleFunctionCall(
    const CallExprAST& expr, sun::ModuleType* moduleType,
    const std::string& funcName, const MemberAccessAST& memberAccess) {
  // Use resolved name from semantic analysis (includes library hash prefix)
  std::string qualifiedName;
  if (memberAccess.hasResolvedQualifiedName()) {
    qualifiedName = memberAccess.getResolvedQualifiedName();
  } else {
    qualifiedName =
        mangleModulePath(moduleType->getModulePath()) + "_" + funcName;
  }

  // Build argument list
  std::vector<Value*> argValues;
  for (const auto& arg : expr.getArgs()) {
    Value* val = codegen(*arg);
    if (!val) return nullptr;
    argValues.push_back(val);
  }

  // Get or declare the function
  Function* func = module->getFunction(qualifiedName);
  if (!func) {
    logAndThrowError("Unknown function: " + qualifiedName);
    return nullptr;
  }

  Value* result = ctx.builder->CreateCall(func, argValues, "calltmp");
  result = unwrapCallErrorUnion(result);
  return materializeStructReturn(result);
}

// -------------------------------------------------------------------
// Interface method dispatch via vtable
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenInterfaceMethodCall(
    const CallExprAST& expr, Value* objectPtr, sun::InterfaceType* ifaceType,
    const std::string& methodName) {
  // Get the method info from the interface
  const sun::InterfaceMethod* ifaceMethod = ifaceType->getMethod(methodName);
  if (!ifaceMethod) {
    logAndThrowError("Unknown method: " + methodName + " on interface " +
                     ifaceType->getName());
    return nullptr;
  }

  // Generic interface methods cannot be dispatched via vtable
  if (ifaceMethod->isGeneric()) {
    logAndThrowError(
        "Cannot dynamically dispatch generic method '" + methodName +
        "' on interface type '" + ifaceType->getName() +
        "'. Generic methods require compile-time type information.");
    return nullptr;
  }

  // Get the vtable slot index for this method
  int methodIndex = ifaceType->getMethodIndex(methodName);
  if (methodIndex < 0) {
    logAndThrowError("Method not in vtable: " + methodName + " on interface " +
                     ifaceType->getName());
    return nullptr;
  }

  // Load the fat pointer from objectPtr (which is an alloca to the fat struct)
  llvm::StructType* fatPtrType =
      sun::InterfaceType::getFatPointerType(ctx.getContext());
  Value* fatPtr = ctx.builder->CreateLoad(fatPtrType, objectPtr, "iface.fat");

  // Extract data_ptr (element 0) and vtable_ptr (element 1)
  Value* dataPtr = ctx.builder->CreateExtractValue(fatPtr, 0, "iface.data");
  Value* vtablePtr = ctx.builder->CreateExtractValue(fatPtr, 1, "iface.vtable");

  // GEP into vtable to get the function pointer at the method slot
  llvm::Type* ptrTy = PointerType::getUnqual(ctx.getContext());
  Value* funcPtrSlot = ctx.builder->CreateGEP(
      ptrTy, vtablePtr,
      ConstantInt::get(Type::getInt32Ty(ctx.getContext()), methodIndex),
      "vtable.slot");

  // Load the function pointer from the vtable
  Value* funcPtr = ctx.builder->CreateLoad(ptrTy, funcPtrSlot, "iface.func");

  // Build the function type for the indirect call
  // Parameters: this pointer (ptr), then method params
  std::vector<llvm::Type*> paramTypes;
  paramTypes.push_back(ptrTy);  // 'this' pointer
  for (const auto& pt : ifaceMethod->paramTypes) {
    paramTypes.push_back(typeResolver.resolve(pt));
  }
  llvm::Type* returnType =
      typeResolver.resolveForReturn(ifaceMethod->returnType);
  llvm::FunctionType* funcType =
      FunctionType::get(returnType, paramTypes, false);

  // Build argument list: data_ptr as 'this', then user arguments
  std::vector<Value*> argValues;
  argValues.push_back(dataPtr);  // 'this' pointer

  for (const auto& argExpr : expr.getArgs()) {
    sun::TypePtr argSunType = argExpr->getResolvedType();
    Value* argVal = codegen(*argExpr);
    if (!argVal) return nullptr;
    argVal = applyMoveSemantics(argVal, argSunType);
    argValues.push_back(argVal);
  }

  // Make the indirect call
  Value* result;
  if (returnType->isVoidTy()) {
    result = ctx.builder->CreateCall(funcType, funcPtr, argValues);
  } else {
    result =
        ctx.builder->CreateCall(funcType, funcPtr, argValues, "iface.call");
  }

  // Handle error union unwrapping and struct materialization
  result = unwrapCallErrorUnion(result);
  return materializeStructReturn(result);
}

// -------------------------------------------------------------------
// Class method dispatch (regular and generic methods)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenClassMethodCall(
    const CallExprAST& expr, Value* objectPtr, sun::ClassType* classType,
    const std::string& methodName, const MemberAccessAST* memberAccess) {
  // Semantic analysis must have resolved the method overload and stored
  // its signature in the member access's resolved type (a FunctionType).
  const sun::ClassMethod* method = nullptr;

  if (!memberAccess) {
    logAndThrowError("Internal error: method call without member access AST");
    return nullptr;
  }

  sun::TypePtr resolvedType = memberAccess->getResolvedType();
  if (!resolvedType || !resolvedType->isFunction()) {
    logAndThrowError("Method '" + methodName + "' on class " +
                     classType->getDisplayName() +
                     " was not resolved by semantic analysis");
    return nullptr;
  }

  // Use the param types from semantic analysis to find the exact method
  const auto& paramTypes =
      static_cast<sun::FunctionType*>(resolvedType.get())->getParamTypes();
  method = classType->getMethodForArgs(methodName, paramTypes);

  // Fallback: for generic methods, type parameters won't match concrete args,
  // so look up by name alone
  if (!method) {
    method = classType->getMethod(methodName);
  }

  if (!method) {
    logAndThrowError("Unknown method: " + methodName + " on class " +
                     classType->getDisplayName());
    return nullptr;
  }

  // Handle generic method calls with type arguments
  // e.g., allocator.create<Point>(3, 4)
  if (memberAccess && memberAccess->hasTypeArguments() && method->isGeneric()) {
    // Type arguments must be resolved by semantic analysis
    if (!memberAccess->hasResolvedTypeArgs()) {
      logAndThrowError(
          "Generic method type arguments not resolved by semantic analysis: " +
          methodName);
      return nullptr;
    }
    const std::vector<sun::TypePtr>& typeArgs =
        memberAccess->getResolvedTypeArgs();

    // Build the specialized mangled name
    std::string baseMangledName = classType->getMangledMethodName(methodName);
    std::string mangledName = baseMangledName;
    for (const auto& typeArg : typeArgs) {
      mangledName += "_" + typeArg->toString();
    }

    // Look up the specialized method function
    Function* specializedFunc = module->getFunction(mangledName);
    if (!specializedFunc) {
      logAndThrowError("Generic method specialization not found: " +
                       mangledName);
      return nullptr;
    }

    // Build arguments: this pointer first, then user arguments
    std::vector<Value*> argValues;
    argValues.push_back(objectPtr);

    for (const auto& argExpr : expr.getArgs()) {
      sun::TypePtr argSunType = argExpr->getResolvedType();
      Value* argVal = codegen(*argExpr);
      if (!argVal) return nullptr;
      argVal = applyMoveSemantics(argVal, argSunType);
      argValues.push_back(argVal);
    }

    if (specializedFunc->getReturnType()->isVoidTy()) {
      return ctx.builder->CreateCall(specializedFunc, argValues);
    }
    Value* result =
        ctx.builder->CreateCall(specializedFunc, argValues, "method.call");
    result = unwrapCallErrorUnion(result);
    return materializeStructReturn(result);
  }

  // Get the mangled method name for regular (non-generic) call
  // Include parameter types for overload disambiguation
  std::string mangledName =
      classType->getMangledMethodName(methodName, method->paramTypes);
  Function* methodFunc = module->getFunction(mangledName);
  if (!methodFunc) {
    logAndThrowError(
        "Method function not found: " + classType->getDisplayName() + "." +
        methodName + " (mangled: " + mangledName + ")");
    return nullptr;
  }

  // Build arguments: this pointer first, then user arguments
  std::vector<Value*> argValues;
  argValues.push_back(objectPtr);

  const auto& methodParamTypes = method->paramTypes;
  size_t methodArgIdx = 0;
  for (const auto& argExpr : expr.getArgs()) {
    sun::TypePtr argSunType = argExpr->getResolvedType();
    sun::TypePtr paramType = methodArgIdx < methodParamTypes.size()
                                 ? methodParamTypes[methodArgIdx]
                                 : nullptr;

    bool isRefParam = paramType && paramType->isReference();

    if (isRefParam) {
      // Check for class -> ref Interface conversion first
      if (argSunType && argSunType->isClass()) {
        Value* classPtr = prepareRefArgument(argExpr.get(), argSunType);
        Value* ifaceRef =
            prepareClassForRefInterface(classPtr, argSunType, paramType);
        if (ifaceRef) {
          argValues.push_back(ifaceRef);
          ++methodArgIdx;
          continue;
        }
      }
      Value* refArg = prepareRefArgument(argExpr.get(), argSunType);
      if (!refArg) return nullptr;
      argValues.push_back(refArg);
    } else {
      Value* argVal = codegen(*argExpr);
      if (!argVal) return nullptr;

      argVal = convertToInterfaceIfNeeded(argVal, argSunType, paramType);
      if (!argVal) return nullptr;

      if (!paramType || !paramType->isInterface()) {
        argVal = applyMoveSemantics(argVal, argSunType);
        argVal = widenNumericIfNeeded(argVal, paramType);
      }

      // Interface args: load fat pointer struct if value is still a pointer
      if (paramType && paramType->isInterface() &&
          argVal->getType()->isPointerTy()) {
        llvm::StructType* fatPtrType =
            sun::InterfaceType::getFatPointerType(ctx.getContext());
        argVal = ctx.builder->CreateLoad(fatPtrType, argVal, "iface.arg.load");
      }

      argValues.push_back(argVal);
    }
    ++methodArgIdx;
  }

  // If this is an explicit deinit() call, mark as already deinited
  if (methodName == "deinit") {
    markClassAllocationAsDeinited(objectPtr);
  }

  if (methodFunc->getReturnType()->isVoidTy()) {
    return ctx.builder->CreateCall(methodFunc, argValues);
  }
  Value* result = ctx.builder->CreateCall(methodFunc, argValues, "method.call");
  result = unwrapCallErrorUnion(result);
  return materializeStructReturn(result);
}

// -------------------------------------------------------------------
// Top-level method call handler
// Orchestrates: module calls → builtin type methods → interface → class
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenMethodCall(const CallExprAST& expr,
                                         const MemberAccessAST& memberAccess) {
  sun::TypePtr objectType = memberAccess.getObject()->getResolvedType();
  const std::string& methodName = memberAccess.getMemberName();

  // Check if the full member access resolves to a class type (constructor call)
  // This handles module-qualified generic class instantiation like
  // Test.Inner<T>(v)
  sun::TypePtr memberAccessType = memberAccess.getResolvedType();
  if (memberAccessType && memberAccessType->isClass()) {
    return codegenStackClassInstance(
        expr, methodName, static_cast<sun::ClassType&>(*memberAccessType));
  }

  // Handle module-qualified function call: mymod.foo()
  if (objectType && objectType->isModule()) {
    return codegenModuleFunctionCall(
        expr, static_cast<sun::ModuleType*>(objectType.get()), methodName,
        memberAccess);
  }

  // Handle array.shape() builtin
  if (objectType && (objectType->isArray() ||
                     (objectType->isReference() &&
                      static_cast<sun::ReferenceType*>(objectType.get())
                          ->getReferencedType()
                          ->isArray()))) {
    if (methodName == "shape") {
      if (!expr.getArgs().empty()) {
        logAndThrowError("shape() takes no arguments");
        return nullptr;
      }
      return codegenArrayShape(memberAccess);
    }
  }

  // Generate object pointer
  Value* objectPtr = codegen(*memberAccess.getObject());
  if (!objectPtr) {
    logAndThrowError("Failed to generate object for method call");
    return nullptr;
  }

  // For generic method bodies, 'this' may have a type parameter type.
  // In that case, use the currentClass which is the specialized type.
  if (dynamic_cast<const ThisExprAST*>(memberAccess.getObject()) &&
      currentClass) {
    objectType = currentClass;
  }

  // Try builtin type methods first (Thread.join(), ptr._get(), etc.)
  // Note: For pointer-to-class, this returns nullptr to continue with class
  // dispatch
  if (Value* builtinResult =
          codegenBuiltinTypeMethod(expr, objectPtr, objectType, methodName)) {
    return builtinResult;
  }

  // Handle pointer-to-class: unwrap to get the underlying class type
  if (objectType &&
      (objectType->isRawPointer() || objectType->isStaticPointer())) {
    sun::TypePtr pointeeType = nullptr;
    if (objectType->isRawPointer()) {
      pointeeType =
          static_cast<sun::RawPointerType*>(objectType.get())->getPointeeType();
    } else {
      pointeeType = static_cast<sun::StaticPointerType*>(objectType.get())
                        ->getPointeeType();
    }

    if (pointeeType && pointeeType->isClass()) {
      auto* cls = static_cast<sun::ClassType*>(pointeeType.get());
      auto registeredClass = typeRegistry->getClass(cls->getMangledName());
      if (!registeredClass) {
        logAndThrowError("Class not found in type registry: " +
                         cls->getMangledName());
        return nullptr;
      }
      objectType = registeredClass;
    }
  }

  // Handle reference types - unwrap to get the underlying type
  if (objectType && objectType->isReference()) {
    objectType =
        static_cast<sun::ReferenceType*>(objectType.get())->getReferencedType();
  }

  // Handle interface dispatch
  if (objectType && objectType->isInterface()) {
    return codegenInterfaceMethodCall(
        expr, objectPtr, static_cast<sun::InterfaceType*>(objectType.get()),
        methodName);
  }

  // Handle Thread type (if not caught by builtin method handler)
  if (objectType && objectType->isThread()) {
    logAndThrowError("Unknown method on Thread: " + methodName);
    return nullptr;
  }

  // Must be a class method call
  if (!objectType || !objectType->isClass()) {
    logAndThrowError("Method call on non-class type: " +
                     (objectType ? objectType->toString() : "null"));
    return nullptr;
  }

  return codegenClassMethodCall(expr, objectPtr,
                                static_cast<sun::ClassType*>(objectType.get()),
                                methodName, &memberAccess);
}

// -------------------------------------------------------------------
// Call expression dispatch
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const CallExprAST& expr) {
  std::string calleeName = "<call-expression>";

  // Check if this is a method call (MemberAccessAST as callee)
  if (auto* memberAccess =
          dynamic_cast<const MemberAccessAST*>(expr.getCallee())) {
    return codegenMethodCall(expr, *memberAccess);
  }

  if (auto* varRef =
          dynamic_cast<const VariableReferenceAST*>(expr.getCallee())) {
    calleeName = varRef->getName();

    // Check for built-in functions (bypass type system)
    if (isBuiltinFunction(calleeName)) {
      return codegenBuiltin(calleeName, expr);
    }

    // Check if this is a stack-allocated class constructor call:
    // ClassName(args...)
    sun::TypePtr calleeType = expr.getCallee()->getResolvedType();
    if (calleeType && calleeType->isClass()) {
      return codegenStackClassInstance(
          expr, calleeName, static_cast<sun::ClassType&>(*calleeType));
    }
  } else if (auto* qualName =
                 dynamic_cast<const QualifiedNameAST*>(expr.getCallee())) {
    // Qualified name like Math::square - mangle :: to _ for LLVM name
    std::string fullName = qualName->getFullName();
    calleeName = fullName;
    size_t pos;
    while ((pos = calleeName.find("::")) != std::string::npos) {
      calleeName.replace(pos, 2, "_");
    }
  }

  // Get the resolved function type (from semantic analysis)
  sun::TypePtr calleeSunType = expr.getCallee()->getResolvedType();
  if (!calleeSunType || !calleeSunType->isCallable()) {
    logAndThrowError("Callee is not callable: " + calleeName);
    return nullptr;
  }

  // Handle Lambda type: fat pointer call with closure
  Value* result;
  if (calleeSunType->isLambda()) {
    result = codegenLambdaCall(
        expr, calleeName, static_cast<const sun::LambdaType&>(*calleeSunType));
  } else {
    // Handle Function type: direct call
    result = codegenFunctionCall(
        expr, calleeName,
        static_cast<const sun::FunctionType&>(*calleeSunType));
  }

  // Handle array returns: copy data/dims to caller's stack
  // Arrays returned by value have pointers to callee's stack which become
  // dangling after return. Copy to caller's stack to fix this.
  sun::TypePtr returnType = expr.getResolvedType();
  if (returnType && returnType->isArray() && result) {
    auto* arrayType = static_cast<sun::ArrayType*>(returnType.get());
    if (!arrayType->getDimensions().empty()) {
      result = copyArrayToCallerStack(result, arrayType);
    }
  }

  return result;
}

// -------------------------------------------------------------------
// Function call codegen (direct call)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenFunctionCall(const CallExprAST& expr,
                                           const std::string& calleeName,
                                           const sun::FunctionType& funcType) {
  // For Function types, we need to:
  // 1. Look up the llvm::Function directly
  // 2. Call it directly without any closure indirection

  // Try to find the function in the module
  llvm::Function* func = nullptr;

  // Check if callee is a variable reference
  if (auto* varRef =
          dynamic_cast<const VariableReferenceAST*>(expr.getCallee())) {
    // Use qualified name from semantic analysis (handles using imports)
    std::string resolvedName = varRef->getMangledName();
    func = module->getFunction(resolvedName);
  } else if (auto* qualName =
                 dynamic_cast<const QualifiedNameAST*>(expr.getCallee())) {
    // Qualified name - use the mangled name (calleeName already has :: replaced
    // with _)
    func = module->getFunction(calleeName);
  }

  if (!func) {
    // Function not found in module - this happens when calling a function
    // passed as a parameter (e.g., `apply(f: _(i32) i32, x: i32) { return f(x);
    // }`) Load the function pointer from the variable
    Value* funcPtrVal = codegen(*expr.getCallee());
    if (!funcPtrVal) {
      logAndThrowError("Failed to get function pointer for: " + calleeName);
      return nullptr;
    }

    // Get the LLVM function type for the indirect call
    llvm::FunctionType* llvmFuncType =
        typeResolver.resolveDirectFunctionSignature(funcType);

    // Build arguments
    std::vector<Value*> argValues;
    for (const auto& argExpr : expr.getArgs()) {
      Value* argVal = codegen(*argExpr);
      if (!argVal) return nullptr;
      argValues.push_back(argVal);
    }

    // Indirect call through function pointer
    if (llvmFuncType->getReturnType()->isVoidTy()) {
      return ctx.builder->CreateCall(llvmFuncType, funcPtrVal, argValues);
    }
    return ctx.builder->CreateCall(llvmFuncType, funcPtrVal, argValues,
                                   "calltmp");
  }

  // Direct call to known function
  std::vector<Value*> argValues;

  // Check if this function has captures (needs closure as first arg)
  auto infoIt = functionInfo.find(calleeName);
  if (infoIt != functionInfo.end() && !infoIt->second.captures.empty()) {
    // Find the closure for this function in local scope
    if (auto* varRef =
            dynamic_cast<const VariableReferenceAST*>(expr.getCallee())) {
      if (AllocaInst* closureAlloca = findVariable(varRef->getName())) {
        argValues.push_back(closureAlloca);
      } else {
        logAndThrowError("Cannot find closure for function with captures: " +
                         calleeName);
        return nullptr;
      }
    }
  }

  // Get parameter types from the function type
  const auto& paramTypes = funcType.getParamTypes();

  unsigned argIdx = 0;
  for (const auto& argExpr : expr.getArgs()) {
    // Check if this parameter is a reference type
    bool isRefParam = argIdx < paramTypes.size() && paramTypes[argIdx] &&
                      paramTypes[argIdx]->isReference();

    // Get the argument's Sun type for auto-deref logic
    sun::TypePtr argSunType = argExpr->getResolvedType();

    if (isRefParam) {
      // Check for class -> ref Interface conversion first
      sun::TypePtr paramType =
          argIdx < paramTypes.size() ? paramTypes[argIdx] : nullptr;
      if (argSunType && argSunType->isClass()) {
        Value* classPtr = prepareRefArgument(argExpr.get(), argSunType);
        Value* ifaceRef =
            prepareClassForRefInterface(classPtr, argSunType, paramType);
        if (ifaceRef) {
          argValues.push_back(ifaceRef);
          ++argIdx;
          continue;
        }
      }
      Value* refArg = prepareRefArgument(argExpr.get(), argSunType);
      if (!refArg) return nullptr;
      argValues.push_back(refArg);
    } else {
      // Normal parameter: codegen the value
      Value* argVal = codegen(*argExpr);
      if (!argVal) return nullptr;

      sun::TypePtr paramType =
          argIdx < paramTypes.size() ? paramTypes[argIdx] : nullptr;

      // Handle class-to-interface conversion (must come before move semantics)
      argVal = convertToInterfaceIfNeeded(argVal, argSunType, paramType);
      if (!argVal) return nullptr;

      // Skip move semantics for interface args (already handled)
      if (!paramType || !paramType->isInterface()) {
        argVal = applyMoveSemantics(argVal, argSunType);

        // Auto-deref: if argument is raw_ptr<T> and param is primitive T, load
        // the value
        if (argSunType && argSunType->isRawPointer() && paramType) {
          sun::TypePtr pointeeType =
              static_cast<sun::RawPointerType*>(argSunType.get())
                  ->getPointeeType();

          // Check if param expects the pointee type (primitive auto-deref)
          if (pointeeType->equals(*paramType) && paramType->isPrimitive()) {
            llvm::Type* pointeeLLVMType =
                pointeeType->toLLVMType(ctx.getContext());
            argVal = ctx.builder->CreateLoad(pointeeLLVMType, argVal,
                                             "auto_deref_arg");
          }
        }

        argVal = widenNumericIfNeeded(argVal, paramType);
      }

      // Interface args: load fat pointer struct if value is still a pointer
      if (paramType && paramType->isInterface() &&
          argVal->getType()->isPointerTy()) {
        llvm::StructType* fatPtrType =
            sun::InterfaceType::getFatPointerType(ctx.getContext());
        argVal = ctx.builder->CreateLoad(fatPtrType, argVal, "iface.arg.load");
      }

      argValues.push_back(argVal);
    }
    ++argIdx;
  }

  // Don't name the call result if the function returns void
  Value* callResult;
  if (func->getReturnType()->isVoidTy()) {
    callResult = ctx.builder->CreateCall(func, argValues);
  } else {
    callResult = ctx.builder->CreateCall(func, argValues, "calltmp");
  }

  // Handle error union unwrapping: if the called function returns an error
  // union, unwrap the result and propagate errors (either to caller or to catch
  // block)
  if (callResult->getType()->isStructTy()) {
    auto* structType = cast<StructType>(callResult->getType());
    // Check if this looks like an error union: { i1, T }
    if (structType->getNumElements() == 2 &&
        structType->getElementType(0)->isIntegerTy(1)) {
      Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();

      // Extract isError flag and value
      Value* isError =
          ctx.builder->CreateExtractValue(callResult, 0, "call.isError");
      Value* innerValue =
          ctx.builder->CreateExtractValue(callResult, 1, "call.value");

      // Create blocks for error handling
      BasicBlock* errorBB =
          BasicBlock::Create(ctx.getContext(), "call.error", currentFunc);
      BasicBlock* successBB =
          BasicBlock::Create(ctx.getContext(), "call.success", currentFunc);

      ctx.builder->CreateCondBr(isError, errorBB, successBB);

      // Error block: either propagate to caller or jump to catch block
      ctx.builder->SetInsertPoint(errorBB);

      if (!tryStack.empty()) {
        // Inside a try block: store error result and jump to catch
        ctx.builder->CreateStore(callResult, tryStack.back().errorResultAlloca);
        ctx.builder->CreateBr(tryStack.back().catchBlock);
      } else if (currentFunctionCanError) {
        // In error-returning function: propagate error to caller
        llvm::Type* retType = currentFunc->getReturnType();
        Value* errorStruct = UndefValue::get(retType);
        errorStruct = ctx.builder->CreateInsertValue(
            errorStruct, ConstantInt::getTrue(ctx.getContext()), 0);
        if (currentFunctionValueType) {
          Value* zeroVal = Constant::getNullValue(currentFunctionValueType);
          errorStruct = ctx.builder->CreateInsertValue(errorStruct, zeroVal, 1);
        }
        ctx.builder->CreateRet(errorStruct);
      } else {
        // Not in try block and not in error-returning function
        // Log warning and continue - error will be silently ignored at runtime
        llvm::errs() << "Warning: calling error-returning function outside of "
                        "try block or error-returning function\n";
        ctx.builder->CreateBr(successBB);
      }

      // Success block: continue with the unwrapped value
      ctx.builder->SetInsertPoint(successBB);
      callResult = innerValue;
    }
  }

  // Handle struct return values (classes returned by value)
  return materializeStructReturn(callResult);
}

// -------------------------------------------------------------------
// Lambda call codegen (fat pointer/closure call)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenLambdaCall(const CallExprAST& expr,
                                         const std::string& calleeName,
                                         const sun::LambdaType& lambdaType) {
  // For Lambda types, we need to:
  // 1. Get the closure pointer (fat pointer)
  // 2. Extract the function pointer from the closure
  // 3. Call with the fat pointer as the first argument

  // Try to get the closure pointer directly without loading (avoids redundant
  // alloca)
  Value* closurePtr = nullptr;
  if (auto* varRef =
          dynamic_cast<const VariableReferenceAST*>(expr.getCallee())) {
    // Check local variable (alloca)
    if (AllocaInst* alloca = findVariable(varRef->getName())) {
      closurePtr = alloca;
    }
    // Check global variable
    else if (GlobalVariable* gv =
                 module->getGlobalVariable(varRef->getName())) {
      closurePtr = gv;
    }
  }

  // If we couldn't get a direct pointer, fall back to loading and storing
  if (!closurePtr) {
    Value* fatPtrVal = codegen(*expr.getCallee());
    if (!fatPtrVal) return nullptr;

    // Create a temporary alloca to hold the closure struct
    Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
    llvm::IRBuilder<> tmpBuilder(&currentFunc->getEntryBlock(),
                                 currentFunc->getEntryBlock().begin());
    AllocaInst* closureAlloca =
        tmpBuilder.CreateAlloca(fatPtrVal->getType(), nullptr, "closure.tmp");
    ctx.builder->CreateStore(fatPtrVal, closureAlloca);
    closurePtr = closureAlloca;
  }

  // Build the LLVM function type using the type resolver
  llvm::FunctionType* llvmFuncType =
      typeResolver.resolveLambdaSignature(lambdaType);

  // Load the closure to extract function pointer
  llvm::Type* closureStructTy = typeResolver.getClosureType();
  Value* loadedClosure =
      ctx.builder->CreateLoad(closureStructTy, closurePtr, "closure.val");
  Value* funcPtr =
      ctx.builder->CreateExtractValue(loadedClosure, {0}, "func.ptr");

  std::vector<Value*> argValues = {closurePtr};

  const auto& paramTypes = lambdaType.getParamTypes();
  unsigned i = 1;  // start at 1, skipping the hidden closure pointer parameter
  for (const auto& argExpr : expr.getArgs()) {
    sun::TypePtr argSunType = argExpr->getResolvedType();
    Value* argVal = codegen(*argExpr);
    if (!argVal) return nullptr;

    // Apply move semantics for class arguments passed by value
    argVal = applyMoveSemantics(argVal, argSunType);

    llvm::Type* expectedTy = llvmFuncType->getParamType(i);

    if (argVal->getType() != expectedTy) {
      logAndThrowError("Type mismatch in argument " + std::to_string(i - 1) +
                       " of call to " + calleeName);
      return nullptr;
    }

    argValues.push_back(argVal);
    ++i;
  }

  // Indirect call through the extracted function pointer
  // Don't name the result if the function returns void
  if (llvmFuncType->getReturnType()->isVoidTy()) {
    return ctx.builder->CreateCall(llvmFuncType, funcPtr, argValues);
  }
  Value* result =
      ctx.builder->CreateCall(llvmFuncType, funcPtr, argValues, "calltmp");

  // Materialize struct return values for addressability
  return materializeStructReturn(result);
}

// -------------------------------------------------------------------
// Built-in functions
// -------------------------------------------------------------------

bool CodegenVisitor::isBuiltinFunction(const std::string& name) {
  return name == "_print_i32" || name == "_print_i64" || name == "_print_f64" ||
         name == "_print_newline" || name == "_println_str" ||
         name == "_print_bytes" || name == "__file_open" ||
         name == "__file_close" || name == "__file_write" ||
         name == "__file_read" || name == "__lseek" || name == "__fstat" ||
         name == "__fsync" || name == "__ftruncate" || name == "__unlink" ||
         name == "__rename" || name == "__mkdir" || name == "__rmdir" ||
         name == "__write" || name == "__read" ||
         // Network socket intrinsics
         name == "__socket" || name == "__bind" || name == "__listen" ||
         name == "__accept" || name == "__connect" || name == "__send" ||
         name == "__recv" || name == "__shutdown" || name == "__setsockopt" ||
         name == "__getsockopt" ||
         // High-level IPv4 socket intrinsics
         name == "__bind_ipv4" || name == "__connect_ipv4" ||
         name == "__accept_fd" ||
         // Pointer intrinsics
         name == "_load_i64" || name == "_store_i64" ||
         // Memory allocation intrinsics
         name == "_malloc" || name == "_free" || name == "_memcpy" ||
         name == "_ptr_offset" ||
         // Atomic intrinsics
         name == "_atomic_cmpxchg_i32" || name == "_atomic_store_i32" ||
         name == "_atomic_load_i32" ||
         // Futex intrinsics
         name == "_futex_wait" || name == "_futex_wake";
}

Value* CodegenVisitor::codegenBuiltin(const std::string& name,
                                      const CallExprAST& expr) {
  if (name == "_print_i32") {
    return codegenPrintI32(expr);
  }
  if (name == "_print_i64") {
    return codegenPrintI64(expr);
  }
  if (name == "_print_f64") {
    return codegenPrintF64(expr);
  }
  if (name == "_print_newline") {
    return codegenPrintNewline();
  }
  if (name == "_println_str") {
    Value* result = codegenPrintString(expr);
    codegenPrintNewline();
    return result;
  }
  if (name == "_print_bytes") {
    return codegenPrintBytes(expr);
  }
  if (name == "__file_open") {
    return codegenFileOpen(expr);
  }
  if (name == "__file_close") {
    return codegenFileClose(expr);
  }
  if (name == "__file_write") {
    return codegenFileWrite(expr);
  }
  if (name == "__file_read") {
    return codegenFileRead(expr);
  }
  if (name == "__lseek") {
    return codegenLseek(expr);
  }
  if (name == "__fstat") {
    return codegenFstat(expr);
  }
  if (name == "__fsync") {
    return codegenFsync(expr);
  }
  if (name == "__ftruncate") {
    return codegenFtruncate(expr);
  }
  if (name == "__unlink") {
    return codegenUnlink(expr);
  }
  if (name == "__rename") {
    return codegenRename(expr);
  }
  if (name == "__mkdir") {
    return codegenMkdir(expr);
  }
  if (name == "__rmdir") {
    return codegenRmdir(expr);
  }
  if (name == "__write") {
    return codegenWrite(expr);
  }
  if (name == "__read") {
    return codegenRead(expr);
  }
  // Pointer intrinsics (implemented in pointers.cpp)
  if (name == "_load_i64") {
    return codegenLoadI64Intrinsic(expr);
  }
  if (name == "_store_i64") {
    return codegenStoreI64Intrinsic(expr);
  }
  // Memory allocation intrinsics
  if (name == "_malloc") {
    return codegenMallocIntrinsic(expr);
  }
  if (name == "_free") {
    return codegenFreeIntrinsic(expr);
  }
  if (name == "_memcpy") {
    return codegenMemcpyIntrinsic(expr);
  }
  if (name == "_ptr_offset") {
    return codegenPtrOffsetIntrinsic(expr);
  }
  // Atomic intrinsics
  if (name == "_atomic_cmpxchg_i32") {
    return codegenAtomicCmpxchgI32Intrinsic(expr);
  }
  if (name == "_atomic_store_i32") {
    return codegenAtomicStoreI32Intrinsic(expr);
  }
  if (name == "_atomic_load_i32") {
    return codegenAtomicLoadI32Intrinsic(expr);
  }
  // Futex intrinsics
  if (name == "_futex_wait") {
    return codegenFutexWaitIntrinsic(expr);
  }
  if (name == "_futex_wake") {
    return codegenFutexWakeIntrinsic(expr);
  }
  // Network socket intrinsics
  if (name == "__socket") {
    return codegenSocket(expr);
  }
  if (name == "__bind") {
    return codegenBind(expr);
  }
  if (name == "__listen") {
    return codegenListen(expr);
  }
  if (name == "__accept") {
    return codegenAccept(expr);
  }
  if (name == "__connect") {
    return codegenConnect(expr);
  }
  if (name == "__send") {
    return codegenSend(expr);
  }
  if (name == "__recv") {
    return codegenRecv(expr);
  }
  if (name == "__shutdown") {
    return codegenShutdown(expr);
  }
  if (name == "__setsockopt") {
    return codegenSetSockOpt(expr);
  }
  if (name == "__getsockopt") {
    return codegenGetSockOpt(expr);
  }
  // High-level IPv4 socket intrinsics
  if (name == "__bind_ipv4") {
    return codegenBindIPv4(expr);
  }
  if (name == "__connect_ipv4") {
    return codegenConnectIPv4(expr);
  }
  if (name == "__accept_fd") {
    return codegenAcceptFd(expr);
  }
  return nullptr;
}

// -------------------------------------------------------------------
// Raw syscall helpers
// -------------------------------------------------------------------

// Raw x86_64 Linux syscall: write(fd, buf, len) -> bytes_written
// Uses inline assembly to execute syscall instruction
static Value* emitRawSyscallWriteInline(IRBuilder<>& builder,
                                        LLVMContext& llvmCtx, Value* fd,
                                        Value* buf, Value* len) {
  std::vector<Type*> asmParamTypes = {
      Type::getInt64Ty(llvmCtx),        // syscall number
      Type::getInt32Ty(llvmCtx),        // fd
      PointerType::getUnqual(llvmCtx),  // buf (opaque ptr)
      Type::getInt64Ty(llvmCtx)         // len
  };
  FunctionType* asmType =
      FunctionType::get(Type::getInt64Ty(llvmCtx), asmParamTypes, false);

  InlineAsm* syscallAsm =
      InlineAsm::get(asmType, "syscall",
                     "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
                     /*hasSideEffects=*/true,
                     /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* syscallNum = ConstantInt::get(Type::getInt64Ty(llvmCtx), 1);
  Value* fdExt = builder.CreateZExt(fd, Type::getInt32Ty(llvmCtx));

  return builder.CreateCall(syscallAsm, {syscallNum, fdExt, buf, len},
                            "syscall_write");
}

// No longer needed - moved inline
Value* CodegenVisitor::emitRawSyscallWrite(Value* fd, Value* buf, Value* len) {
  return emitRawSyscallWriteInline(*ctx.builder, ctx.getContext(), fd, buf,
                                   len);
}

// -------------------------------------------------------------------
// Print helper functions
// -------------------------------------------------------------------

// Get or create the __sun_print_i32 helper function
static Function* getOrCreatePrintI32Helper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_print_i32");
  if (func) return func;

  // Create function: void __sun_print_i32(i32 %val)
  FunctionType* funcType = FunctionType::get(
      Type::getVoidTy(llvmCtx), {Type::getInt32Ty(llvmCtx)}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_print_i32", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  BasicBlock* loopBB = BasicBlock::Create(llvmCtx, "loop", func);
  BasicBlock* afterLoopBB = BasicBlock::Create(llvmCtx, "after_loop", func);
  BasicBlock* addMinusBB = BasicBlock::Create(llvmCtx, "add_minus", func);
  BasicBlock* writeBB = BasicBlock::Create(llvmCtx, "write", func);

  IRBuilder<> builder(entryBB);
  Value* val = func->arg_begin();

  // Buffer on stack
  llvm::Type* bufArrayType = ArrayType::get(Type::getInt8Ty(llvmCtx), 12);
  AllocaInst* buffer = builder.CreateAlloca(bufArrayType, nullptr, "buf");

  AllocaInst* idxAlloca = builder.CreateAlloca(Type::getInt32Ty(llvmCtx));
  builder.CreateStore(ConstantInt::get(Type::getInt32Ty(llvmCtx), 11),
                      idxAlloca);

  Value* isNegative = builder.CreateICmpSLT(
      val, ConstantInt::get(Type::getInt32Ty(llvmCtx), 0), "is_neg");
  Value* absVal = builder.CreateSelect(
      isNegative, builder.CreateNeg(val, "neg"), val, "abs");

  AllocaInst* numAlloca = builder.CreateAlloca(Type::getInt32Ty(llvmCtx));
  builder.CreateStore(absVal, numAlloca);
  builder.CreateBr(loopBB);

  // Loop: extract digits right to left
  builder.SetInsertPoint(loopBB);
  Value* num = builder.CreateLoad(Type::getInt32Ty(llvmCtx), numAlloca);
  Value* idx = builder.CreateLoad(Type::getInt32Ty(llvmCtx), idxAlloca);

  Value* digit =
      builder.CreateURem(num, ConstantInt::get(Type::getInt32Ty(llvmCtx), 10));
  Value* digitChar = builder.CreateAdd(
      digit, ConstantInt::get(Type::getInt32Ty(llvmCtx), '0'));
  Value* digitChar8 = builder.CreateTrunc(digitChar, Type::getInt8Ty(llvmCtx));

  Value* charPtr = builder.CreateGEP(Type::getInt8Ty(llvmCtx), buffer, idx);
  builder.CreateStore(digitChar8, charPtr);

  Value* newIdx =
      builder.CreateSub(idx, ConstantInt::get(Type::getInt32Ty(llvmCtx), 1));
  builder.CreateStore(newIdx, idxAlloca);
  Value* newNum =
      builder.CreateUDiv(num, ConstantInt::get(Type::getInt32Ty(llvmCtx), 10));
  builder.CreateStore(newNum, numAlloca);

  Value* cont = builder.CreateICmpUGT(
      newNum, ConstantInt::get(Type::getInt32Ty(llvmCtx), 0));
  builder.CreateCondBr(cont, loopBB, afterLoopBB);

  // After loop: check if negative
  builder.SetInsertPoint(afterLoopBB);
  builder.CreateCondBr(isNegative, addMinusBB, writeBB);

  // Add minus sign
  builder.SetInsertPoint(addMinusBB);
  Value* minusIdx = builder.CreateLoad(Type::getInt32Ty(llvmCtx), idxAlloca);
  Value* minusPtr =
      builder.CreateGEP(Type::getInt8Ty(llvmCtx), buffer, minusIdx);
  builder.CreateStore(ConstantInt::get(Type::getInt8Ty(llvmCtx), '-'),
                      minusPtr);
  builder.CreateStore(
      builder.CreateSub(minusIdx,
                        ConstantInt::get(Type::getInt32Ty(llvmCtx), 1)),
      idxAlloca);
  builder.CreateBr(writeBB);

  // Write to stdout
  builder.SetInsertPoint(writeBB);
  Value* finalIdx = builder.CreateLoad(Type::getInt32Ty(llvmCtx), idxAlloca);
  Value* startIdx = builder.CreateAdd(
      finalIdx, ConstantInt::get(Type::getInt32Ty(llvmCtx), 1));
  Value* startPtr =
      builder.CreateGEP(Type::getInt8Ty(llvmCtx), buffer, startIdx);
  Value* length = builder.CreateSub(
      ConstantInt::get(Type::getInt32Ty(llvmCtx), 12), startIdx);
  Value* length64 = builder.CreateZExt(length, Type::getInt64Ty(llvmCtx));
  Value* fd = ConstantInt::get(Type::getInt32Ty(llvmCtx), 1);

  emitRawSyscallWriteInline(builder, llvmCtx, fd, startPtr, length64);
  builder.CreateRetVoid();

  return func;
}

// Get or create the __sun_print_newline helper function
static Function* getOrCreatePrintNewlineHelper(llvm::Module* module,
                                               LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_print_newline");
  if (func) return func;

  FunctionType* funcType =
      FunctionType::get(Type::getVoidTy(llvmCtx), {}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_print_newline", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  AllocaInst* buffer = builder.CreateAlloca(Type::getInt8Ty(llvmCtx));
  builder.CreateStore(ConstantInt::get(Type::getInt8Ty(llvmCtx), '\n'), buffer);

  Value* fd = ConstantInt::get(Type::getInt32Ty(llvmCtx), 1);
  Value* len = ConstantInt::get(Type::getInt64Ty(llvmCtx), 1);
  emitRawSyscallWriteInline(builder, llvmCtx, fd, buffer, len);
  builder.CreateRetVoid();

  return func;
}

// Get or create the __sun_print_string helper function
// Takes an i8* (null-terminated string) and prints it
static Function* getOrCreatePrintStringHelper(llvm::Module* module,
                                              LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_print_string");
  if (func) return func;

  // Create function: void __sun_print_string(i8* %str)
  FunctionType* funcType = FunctionType::get(
      Type::getVoidTy(llvmCtx), {PointerType::getUnqual(llvmCtx)}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_print_string", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  BasicBlock* loopBB = BasicBlock::Create(llvmCtx, "strlen_loop", func);
  BasicBlock* writeBB = BasicBlock::Create(llvmCtx, "write", func);

  IRBuilder<> builder(entryBB);
  Value* strPtr = func->arg_begin();

  // Calculate string length using a loop (manual strlen)
  AllocaInst* lenAlloca = builder.CreateAlloca(Type::getInt64Ty(llvmCtx));
  builder.CreateStore(ConstantInt::get(Type::getInt64Ty(llvmCtx), 0),
                      lenAlloca);
  builder.CreateBr(loopBB);

  // strlen loop
  builder.SetInsertPoint(loopBB);
  Value* len = builder.CreateLoad(Type::getInt64Ty(llvmCtx), lenAlloca);
  Value* charPtr = builder.CreateGEP(Type::getInt8Ty(llvmCtx), strPtr, len);
  Value* ch = builder.CreateLoad(Type::getInt8Ty(llvmCtx), charPtr);
  Value* isNull =
      builder.CreateICmpEQ(ch, ConstantInt::get(Type::getInt8Ty(llvmCtx), 0));
  Value* newLen =
      builder.CreateAdd(len, ConstantInt::get(Type::getInt64Ty(llvmCtx), 1));
  builder.CreateStore(newLen, lenAlloca);
  builder.CreateCondBr(isNull, writeBB, loopBB);

  // Write to stdout
  builder.SetInsertPoint(writeBB);
  Value* finalLen = builder.CreateLoad(Type::getInt64Ty(llvmCtx), lenAlloca);
  // Subtract 1 because we incremented past the null terminator
  finalLen = builder.CreateSub(finalLen,
                               ConstantInt::get(Type::getInt64Ty(llvmCtx), 1));
  Value* fd = ConstantInt::get(Type::getInt32Ty(llvmCtx), 1);
  emitRawSyscallWriteInline(builder, llvmCtx, fd, strPtr, finalLen);
  builder.CreateRetVoid();

  return func;
}

// -------------------------------------------------------------------
// Print codegen methods
// -------------------------------------------------------------------

// Emit call to __sun_print_i32 helper
Value* CodegenVisitor::codegenPrintI32(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("print_i32 expects exactly 1 argument");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* val = codegen(*expr.getArgs()[0]);
  if (!val) return nullptr;

  if (!val->getType()->isIntegerTy(32)) {
    val = ctx.builder->CreateSExtOrTrunc(val, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreatePrintI32Helper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {val});
}

// Emit call to __sun_print_i64 helper (reuses i32 approach)
Value* CodegenVisitor::codegenPrintI64(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("print_i64 expects exactly 1 argument");
    return nullptr;
  }

  // For now, truncate to i32 and use print_i32 helper
  // TODO: implement proper i64 helper
  LLVMContext& llvmCtx = ctx.getContext();
  Value* val = codegen(*expr.getArgs()[0]);
  if (!val) return nullptr;

  if (!val->getType()->isIntegerTy(32)) {
    val = ctx.builder->CreateSExtOrTrunc(val, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreatePrintI32Helper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {val});
}

// Emit call to print f64 (simplified: prints integer part only for now)
Value* CodegenVisitor::codegenPrintF64(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("print_f64 expects exactly 1 argument");
    return nullptr;
  }

  // Simplified: convert to i32 and print
  // TODO: implement proper float printing
  LLVMContext& llvmCtx = ctx.getContext();
  Value* val = codegen(*expr.getArgs()[0]);
  if (!val) return nullptr;

  val = ctx.builder->CreateFPToSI(val, Type::getInt32Ty(llvmCtx));
  Function* helper = getOrCreatePrintI32Helper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {val});
}

// Emit call to __sun_print_newline helper
Value* CodegenVisitor::codegenPrintNewline() {
  LLVMContext& llvmCtx = ctx.getContext();
  Function* helper = getOrCreatePrintNewlineHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {});
}

// Emit call to __sun_print_string helper
// Supports two overloads:
//   println(str: static_ptr<u8>) - string literals (fat pointer struct)
Value* CodegenVisitor::codegenPrintString(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("println expects exactly 1 argument");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* val = codegen(*expr.getArgs()[0]);
  if (!val) return nullptr;

  // Overload 1: static_ptr<u8> (string literals)
  // Fat pointer struct { ptr, i64 } - extract the raw data pointer (element 0)
  if (val->getType()->isStructTy()) {
    val = ctx.builder->CreateExtractValue(val, 0, "str.data");
  }
  Function* helper = getOrCreatePrintStringHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {val});
}

// Emit code to write raw bytes to stdout
// _print_bytes(ptr: raw_ptr<i8>, len: i64) -> void
Value* CodegenVisitor::codegenPrintBytes(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError(
        "_print_bytes expects 2 arguments: (ptr: raw_ptr<i8>, len: i64)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* ptr = codegen(*expr.getArgs()[0]);
  Value* len = codegen(*expr.getArgs()[1]);
  if (!ptr || !len) return nullptr;

  // Ensure len is i64
  if (!len->getType()->isIntegerTy(64)) {
    len = ctx.builder->CreateSExtOrTrunc(len, Type::getInt64Ty(llvmCtx));
  }

  // Write to stdout (fd = 1$)
  Value* fd = ConstantInt::get(Type::getInt32Ty(llvmCtx), 1);
  emitRawSyscallWriteInline(*ctx.builder, llvmCtx, fd, ptr, len);
  return ConstantInt::get(Type::getInt32Ty(llvmCtx), 0);
}

// ===================================================================
// File I/O built-in helpers (raw Linux x86_64 syscalls)
// ===================================================================

// Generic raw syscall emitter for 3-argument syscalls:
//   syscall(number, arg1, arg2, arg3) -> result
// Uses inline assembly on x86_64 Linux.
static Value* emitRawSyscall3(IRBuilder<>& builder, LLVMContext& llvmCtx,
                              Value* sysno, Value* arg1, Value* arg2,
                              Value* arg3) {
  // All args must be i64 for the inline asm constraint
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  sysno = builder.CreateZExtOrTrunc(sysno, i64Ty);
  arg1 = builder.CreateZExtOrTrunc(arg1, i64Ty);
  // arg2 can be a pointer, so we use ptrtoint if needed
  if (arg2->getType()->isPointerTy()) {
    arg2 = builder.CreatePtrToInt(arg2, i64Ty);
  } else {
    arg2 = builder.CreateZExtOrTrunc(arg2, i64Ty);
  }
  arg3 = builder.CreateZExtOrTrunc(arg3, i64Ty);

  std::vector<Type*> paramTypes(4, i64Ty);
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);

  InlineAsm* syscallAsm =
      InlineAsm::get(asmType, "syscall",
                     "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
                     /*hasSideEffects=*/true,
                     /*isAlignStack=*/false, InlineAsm::AD_ATT);

  return builder.CreateCall(syscallAsm, {sysno, arg1, arg2, arg3},
                            "syscall_result");
}

// Emit raw syscall with a pointer as arg2 (for read/write buf)
static Value* emitRawSyscall3Ptr(IRBuilder<>& builder, LLVMContext& llvmCtx,
                                 Value* sysno, Value* fd, Value* buf,
                                 Value* len) {
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // syscall write/read: rax=sysno, rdi=fd, rsi=buf(ptr), rdx=len
  std::vector<Type*> paramTypes = {i64Ty, i32Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);

  InlineAsm* syscallAsm =
      InlineAsm::get(asmType, "syscall",
                     "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
                     /*hasSideEffects=*/true,
                     /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysnoVal = builder.CreateZExtOrTrunc(sysno, i64Ty);
  Value* fdVal = builder.CreateZExtOrTrunc(fd, i32Ty);
  Value* lenVal = builder.CreateZExtOrTrunc(len, i64Ty);

  return builder.CreateCall(syscallAsm, {sysnoVal, fdVal, buf, lenVal},
                            "syscall_result");
}

// -------------------------------------------------------------------
// __sun_file_open: open(path, flags, mode) -> fd
// -------------------------------------------------------------------
static Function* getOrCreateFileOpenHelper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_file_open");
  if (func) return func;

  // i32 __sun_file_open(i8* path, i32 flags)
  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {ptrTy, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_file_open", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* path = &*argIt++;
  Value* userFlags = &*argIt++;

  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // Map user flags to Linux open flags:
  //   0 -> O_RDONLY (0)
  //   1 -> O_WRONLY|O_CREAT|O_TRUNC (0x241)
  //   2 -> O_WRONLY|O_CREAT|O_APPEND (0x441)

  // Default: O_RDONLY
  BasicBlock* readBB = BasicBlock::Create(llvmCtx, "mode_read", func);
  BasicBlock* writeBB = BasicBlock::Create(llvmCtx, "mode_write", func);
  BasicBlock* appendBB = BasicBlock::Create(llvmCtx, "mode_append", func);
  BasicBlock* syscallBB = BasicBlock::Create(llvmCtx, "do_open", func);

  AllocaInst* flagsAlloca = builder.CreateAlloca(i32Ty, nullptr, "flags");
  builder.CreateStore(ConstantInt::get(i32Ty, 0), flagsAlloca);  // O_RDONLY

  Value* isWrite =
      builder.CreateICmpEQ(userFlags, ConstantInt::get(i32Ty, 1), "is_write");
  builder.CreateCondBr(isWrite, writeBB, readBB);

  // Read check -> could be append
  builder.SetInsertPoint(readBB);
  Value* isAppend =
      builder.CreateICmpEQ(userFlags, ConstantInt::get(i32Ty, 2), "is_append");
  builder.CreateCondBr(isAppend, appendBB, syscallBB);

  // Write mode: O_WRONLY|O_CREAT|O_TRUNC = 0x241 = 577
  builder.SetInsertPoint(writeBB);
  builder.CreateStore(ConstantInt::get(i32Ty, 577), flagsAlloca);
  builder.CreateBr(syscallBB);

  // Append mode: O_WRONLY|O_CREAT|O_APPEND = 0x441 = 1089
  builder.SetInsertPoint(appendBB);
  builder.CreateStore(ConstantInt::get(i32Ty, 1089), flagsAlloca);
  builder.CreateBr(syscallBB);

  // Do the syscall: sys_open = 2
  builder.SetInsertPoint(syscallBB);
  Value* openFlags = builder.CreateLoad(i32Ty, flagsAlloca);

  // mode = 0644 = 420 (for create)
  Value* mode = ConstantInt::get(i64Ty, 420);  // 0644 octal
  Value* sysno = ConstantInt::get(i64Ty, 2);   // sys_open

  // For open: rdi=path(ptr), so we need a version that takes ptr as arg1
  // open(const char *filename, int flags, umode_t mode)
  std::vector<Type*> paramTypes = {i64Ty, ptrTy, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);

  InlineAsm* syscallAsm =
      InlineAsm::get(asmType, "syscall",
                     "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
                     /*hasSideEffects=*/true,
                     /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* flagsExt = builder.CreateZExt(openFlags, i64Ty);
  Value* result =
      builder.CreateCall(syscallAsm, {sysno, path, flagsExt, mode}, "fd");

  // Truncate result to i32 for the return
  Value* fd = builder.CreateTrunc(result, i32Ty, "fd32");
  builder.CreateRet(fd);

  return func;
}

// -------------------------------------------------------------------
// __sun_file_close: close(fd) -> result
// -------------------------------------------------------------------
static Function* getOrCreateFileCloseHelper(llvm::Module* module,
                                            LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_file_close");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_file_close", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  Value* fd = &*func->arg_begin();
  auto* i64Ty = Type::getInt64Ty(llvmCtx);

  // sys_close = 3, takes 1 arg: fd
  // We use a 3-arg syscall with dummy values for unused args
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 3);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, fdExt, zero, zero},
                                     "close_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// -------------------------------------------------------------------
// __sun_file_write: write(fd, str) -> bytes_written
// Writes a null-terminated string to the given fd.
// -------------------------------------------------------------------
static Function* getOrCreateFileWriteHelper(llvm::Module* module,
                                            LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_file_write");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* i8Ty = Type::getInt8Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // i32 __sun_file_write(i32 fd, i8* str)
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty, ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_file_write", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  BasicBlock* loopBB = BasicBlock::Create(llvmCtx, "strlen_loop", func);
  BasicBlock* writeBB = BasicBlock::Create(llvmCtx, "write", func);

  IRBuilder<> builder(entryBB);
  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* strPtr = &*argIt++;

  // Calculate string length (manual strlen)
  AllocaInst* lenAlloca = builder.CreateAlloca(i64Ty, nullptr, "len");
  builder.CreateStore(ConstantInt::get(i64Ty, 0), lenAlloca);
  builder.CreateBr(loopBB);

  builder.SetInsertPoint(loopBB);
  Value* len = builder.CreateLoad(i64Ty, lenAlloca);
  Value* charPtr = builder.CreateGEP(i8Ty, strPtr, len);
  Value* ch = builder.CreateLoad(i8Ty, charPtr);
  Value* isNull = builder.CreateICmpEQ(ch, ConstantInt::get(i8Ty, 0));
  Value* newLen = builder.CreateAdd(len, ConstantInt::get(i64Ty, 1));
  builder.CreateStore(newLen, lenAlloca);
  builder.CreateCondBr(isNull, writeBB, loopBB);

  // Write to fd using sys_write (syscall 1)
  builder.SetInsertPoint(writeBB);
  Value* finalLen = builder.CreateLoad(i64Ty, lenAlloca);
  finalLen =
      builder.CreateSub(finalLen, ConstantInt::get(i64Ty, 1));  // exclude null

  Value* sysno = ConstantInt::get(i64Ty, 1);  // sys_write

  std::vector<Type*> paramTypes = {i64Ty, i32Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* result =
      builder.CreateCall(syscallAsm, {sysno, fd, strPtr, finalLen}, "written");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);

  return func;
}

// -------------------------------------------------------------------
// __sun_file_read: read(fd, count) -> string
// Reads up to 'count' bytes from fd, returns heap-allocated string.
// Uses mmap for memory allocation (syscall 9).
// -------------------------------------------------------------------
static Function* getOrCreateFileReadHelper(llvm::Module* module,
                                           LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_file_read");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* i8Ty = Type::getInt8Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);

  // i8* __sun_file_read(i32 fd, i32 count)
  FunctionType* funcType = FunctionType::get(ptrTy, {i32Ty, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_file_read", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  BasicBlock* readBB = BasicBlock::Create(llvmCtx, "do_read", func);
  BasicBlock* doneBB = BasicBlock::Create(llvmCtx, "done", func);

  IRBuilder<> builder(entryBB);
  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* count = &*argIt++;

  // Allocate buffer using mmap (syscall 9)
  // mmap(addr=0, length=count+1, prot=PROT_READ|PROT_WRITE=3,
  //      flags=MAP_PRIVATE|MAP_ANONYMOUS=0x22, fd=-1, offset=0)
  // We need a 6-arg syscall for mmap

  Value* countExt = builder.CreateZExt(count, i64Ty);
  Value* bufSize =
      builder.CreateAdd(countExt, ConstantInt::get(i64Ty, 1));  // +1 for null

  // 6-arg syscall for mmap
  // mmap uses: rax=sysno, rdi=addr, rsi=len, rdx=prot, r10=flags, r8=fd,
  // r9=offset We directly assign r10, r8, r9 as inputs instead of moving from
  // general regs
  std::vector<Type*> mmap6Types(7, i64Ty);  // sysno + 6 args
  FunctionType* mmap6FuncType = FunctionType::get(i64Ty, mmap6Types, false);
  InlineAsm* mmapAsm = InlineAsm::get(
      mmap6FuncType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* mmapSysno = ConstantInt::get(i64Ty, 9);  // sys_mmap
  Value* mmapAddr = ConstantInt::get(i64Ty, 0);   // NULL
  Value* mmapProt = ConstantInt::get(i64Ty, 3);   // PROT_READ|PROT_WRITE
  Value* mmapFlags =
      ConstantInt::get(i64Ty, 0x22);  // MAP_PRIVATE|MAP_ANONYMOUS
  Value* mmapFd = ConstantInt::getSigned(i64Ty, -1);
  Value* mmapOffset = ConstantInt::get(i64Ty, 0);

  Value* mmapResult = builder.CreateCall(
      mmapAsm,
      {mmapSysno, mmapAddr, bufSize, mmapProt, mmapFlags, mmapFd, mmapOffset},
      "mmap_result");

  Value* bufPtr = builder.CreateIntToPtr(mmapResult, ptrTy, "buf");
  builder.CreateBr(readBB);

  // sys_read(fd, buf, count) - syscall 0
  builder.SetInsertPoint(readBB);
  std::vector<Type*> readParamTypes = {i64Ty, i32Ty, ptrTy, i64Ty};
  FunctionType* readAsmType = FunctionType::get(i64Ty, readParamTypes, false);
  InlineAsm* readAsm = InlineAsm::get(
      readAsmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* readSysno = ConstantInt::get(i64Ty, 0);  // sys_read
  Value* readResult = builder.CreateCall(
      readAsm, {readSysno, fd, bufPtr, countExt}, "bytes_read");
  builder.CreateBr(doneBB);

  // Null-terminate the buffer at the position of bytes_read
  builder.SetInsertPoint(doneBB);

  // If read returned negative (error), clamp to 0
  Value* isNeg = builder.CreateICmpSLT(readResult, ConstantInt::get(i64Ty, 0));
  Value* safeLen =
      builder.CreateSelect(isNeg, ConstantInt::get(i64Ty, 0), readResult);

  Value* nullPtr = builder.CreateGEP(i8Ty, bufPtr, safeLen);
  builder.CreateStore(ConstantInt::get(i8Ty, 0), nullPtr);

  builder.CreateRet(bufPtr);
  return func;
}

// -------------------------------------------------------------------
// File I/O codegen methods
// -------------------------------------------------------------------

// file_open(path: string, flags: i32) -> i32
Value* CodegenVisitor::codegenFileOpen(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError(
        "file_open expects 2 arguments: (path: string, flags: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* path = codegen(*expr.getArgs()[0]);
  if (!path) return nullptr;
  Value* flags = codegen(*expr.getArgs()[1]);
  if (!flags) return nullptr;

  // String literals are static_ptr<u8> which is a fat pointer struct { ptr, i64
  // } Extract the raw data pointer (element 0) for the syscall
  if (path->getType()->isStructTy()) {
    path = ctx.builder->CreateExtractValue(path, 0, "path.data");
  }

  // Ensure flags is i32
  if (!flags->getType()->isIntegerTy(32)) {
    flags = ctx.builder->CreateSExtOrTrunc(flags, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateFileOpenHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {path, flags}, "fd");
}

// file_close(fd: i32) -> i32
Value* CodegenVisitor::codegenFileClose(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("file_close expects 1 argument: (fd: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateFileCloseHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd}, "close_result");
}

// file_write(fd: i32, data: string) -> i32
Value* CodegenVisitor::codegenFileWrite(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError("file_write expects 2 arguments: (fd: i32, data: string)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* data = codegen(*expr.getArgs()[1]);
  if (!data) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }

  // String literals are static_ptr<u8> which is a fat pointer struct { ptr, i64
  // } Extract the raw data pointer (element 0) for the write syscall
  if (data->getType()->isStructTy()) {
    data = ctx.builder->CreateExtractValue(data, 0, "str.data");
  }

  Function* helper = getOrCreateFileWriteHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, data}, "written");
}

// file_read(fd: i32, count: i32) -> string
Value* CodegenVisitor::codegenFileRead(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError("file_read expects 2 arguments: (fd: i32, count: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* count = codegen(*expr.getArgs()[1]);
  if (!count) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!count->getType()->isIntegerTy(32)) {
    count = ctx.builder->CreateSExtOrTrunc(count, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateFileReadHelper(module, llvmCtx);
  Value* result = ctx.builder->CreateCall(helper, {fd, count}, "read_str");

  // Track allocation metadata for automatic cleanup with munmap
  // Size is count+1 (for null terminator), same as what the helper allocates
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  Value* countExt = ctx.builder->CreateZExt(count, i64Ty);
  Value* mmapSize =
      ctx.builder->CreateAdd(countExt, ConstantInt::get(i64Ty, 1), "mmap.size");
  allocationMetadata[result] = {/*isMmap=*/true, /*size=*/mmapSize};

  return result;
}

// -------------------------------------------------------------------
// Extended file I/O helper functions
// -------------------------------------------------------------------

// __sun_lseek: lseek(fd, offset, whence) -> new_offset
static Function* getOrCreateLseekHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_lseek");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  FunctionType* funcType =
      FunctionType::get(i64Ty, {i32Ty, i64Ty, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_lseek",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* offset = &*argIt++;
  Value* whence = &*argIt++;

  // sys_lseek = 8
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 8);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* whenceExt = builder.CreateZExt(whence, i64Ty);

  Value* result = builder.CreateCall(
      syscallAsm, {sysno, fdExt, offset, whenceExt}, "lseek_result");
  builder.CreateRet(result);
  return func;
}

// __sun_fstat: fstat(fd, stat_buf) -> result
static Function* getOrCreateFstatHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_fstat");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty, ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_fstat",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* statBuf = &*argIt++;

  // sys_fstat = 5
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 5);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, fdExt, statBuf, zero},
                                     "fstat_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_fsync: fsync(fd) -> result
static Function* getOrCreateFsyncHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_fsync");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_fsync",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  Value* fd = &*func->arg_begin();

  // sys_fsync = 74
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 74);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, fdExt, zero, zero},
                                     "fsync_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_ftruncate: ftruncate(fd, length) -> result
static Function* getOrCreateFtruncateHelper(llvm::Module* module,
                                            LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_ftruncate");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {i32Ty, i64Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage,
                          "__sun_ftruncate", module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* length = &*argIt++;

  // sys_ftruncate = 77
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 77);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, fdExt, length, zero},
                                     "ftruncate_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_unlink: unlink(path) -> result
static Function* getOrCreateUnlinkHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_unlink");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_unlink",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  Value* path = &*func->arg_begin();

  // sys_unlink = 87
  std::vector<Type*> paramTypes = {i64Ty, ptrTy, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 87);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, path, zero, zero},
                                     "unlink_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_rename: rename(old_path, new_path) -> result
static Function* getOrCreateRenameHelper(llvm::Module* module,
                                         LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_rename");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {ptrTy, ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_rename",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* oldPath = &*argIt++;
  Value* newPath = &*argIt++;

  // sys_rename = 82
  std::vector<Type*> paramTypes = {i64Ty, ptrTy, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 82);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(
      syscallAsm, {sysno, oldPath, newPath, zero}, "rename_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_mkdir: mkdir(path, mode) -> result
static Function* getOrCreateMkdirHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_mkdir");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {ptrTy, i32Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_mkdir",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* path = &*argIt++;
  Value* mode = &*argIt++;

  // sys_mkdir = 83
  std::vector<Type*> paramTypes = {i64Ty, ptrTy, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 83);
  Value* modeExt = builder.CreateZExt(mode, i64Ty);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result = builder.CreateCall(syscallAsm, {sysno, path, modeExt, zero},
                                     "mkdir_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_rmdir: rmdir(path) -> result
static Function* getOrCreateRmdirHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_rmdir");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType = FunctionType::get(i32Ty, {ptrTy}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_rmdir",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  Value* path = &*func->arg_begin();

  // sys_rmdir = 84
  std::vector<Type*> paramTypes = {i64Ty, ptrTy, i64Ty, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 84);
  Value* zero = ConstantInt::get(i64Ty, 0);

  Value* result =
      builder.CreateCall(syscallAsm, {sysno, path, zero, zero}, "rmdir_result");
  Value* result32 = builder.CreateTrunc(result, i32Ty);
  builder.CreateRet(result32);
  return func;
}

// __sun_write: write(fd, buf, len) -> bytes_written
static Function* getOrCreateWriteHelper(llvm::Module* module,
                                        LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_write");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType =
      FunctionType::get(i64Ty, {i32Ty, ptrTy, i64Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_write",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* buf = &*argIt++;
  Value* len = &*argIt++;

  // sys_write = 1
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 1);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);

  Value* result =
      builder.CreateCall(syscallAsm, {sysno, fdExt, buf, len}, "write_result");
  builder.CreateRet(result);
  return func;
}

// __sun_read: read(fd, buf, len) -> bytes_read
static Function* getOrCreateReadHelper(llvm::Module* module,
                                       LLVMContext& llvmCtx) {
  Function* func = module->getFunction("__sun_read");
  if (func) return func;

  auto* i32Ty = Type::getInt32Ty(llvmCtx);
  auto* i64Ty = Type::getInt64Ty(llvmCtx);
  auto* ptrTy = PointerType::getUnqual(llvmCtx);
  FunctionType* funcType =
      FunctionType::get(i64Ty, {i32Ty, ptrTy, i64Ty}, false);
  func = Function::Create(funcType, Function::InternalLinkage, "__sun_read",
                          module);

  BasicBlock* entryBB = BasicBlock::Create(llvmCtx, "entry", func);
  IRBuilder<> builder(entryBB);

  auto argIt = func->arg_begin();
  Value* fd = &*argIt++;
  Value* buf = &*argIt++;
  Value* len = &*argIt++;

  // sys_read = 0
  std::vector<Type*> paramTypes = {i64Ty, i64Ty, ptrTy, i64Ty};
  FunctionType* asmType = FunctionType::get(i64Ty, paramTypes, false);
  InlineAsm* syscallAsm = InlineAsm::get(
      asmType, "syscall",
      "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
      /*hasSideEffects=*/true, /*isAlignStack=*/false, InlineAsm::AD_ATT);

  Value* sysno = ConstantInt::get(i64Ty, 0);
  Value* fdExt = builder.CreateZExt(fd, i64Ty);

  Value* result =
      builder.CreateCall(syscallAsm, {sysno, fdExt, buf, len}, "read_result");
  builder.CreateRet(result);
  return func;
}

// -------------------------------------------------------------------
// Extended file I/O codegen methods
// -------------------------------------------------------------------

// __lseek(fd: i32, offset: i64, whence: i32) -> i64
Value* CodegenVisitor::codegenLseek(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__lseek expects 3 arguments: (fd: i32, offset: i64, whence: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* offset = codegen(*expr.getArgs()[1]);
  if (!offset) return nullptr;
  Value* whence = codegen(*expr.getArgs()[2]);
  if (!whence) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!offset->getType()->isIntegerTy(64)) {
    offset = ctx.builder->CreateSExtOrTrunc(offset, Type::getInt64Ty(llvmCtx));
  }
  if (!whence->getType()->isIntegerTy(32)) {
    whence = ctx.builder->CreateSExtOrTrunc(whence, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateLseekHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, offset, whence}, "lseek_result");
}

// __fstat(fd: i32, stat_buf: raw_ptr<i8>) -> i32
Value* CodegenVisitor::codegenFstat(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError(
        "__fstat expects 2 arguments: (fd: i32, stat_buf: raw_ptr<i8>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* statBuf = codegen(*expr.getArgs()[1]);
  if (!statBuf) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateFstatHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, statBuf}, "fstat_result");
}

// __fsync(fd: i32) -> i32
Value* CodegenVisitor::codegenFsync(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("__fsync expects 1 argument: (fd: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateFsyncHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd}, "fsync_result");
}

// __ftruncate(fd: i32, length: i64) -> i32
Value* CodegenVisitor::codegenFtruncate(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError("__ftruncate expects 2 arguments: (fd: i32, length: i64)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* length = codegen(*expr.getArgs()[1]);
  if (!length) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!length->getType()->isIntegerTy(64)) {
    length = ctx.builder->CreateSExtOrTrunc(length, Type::getInt64Ty(llvmCtx));
  }

  Function* helper = getOrCreateFtruncateHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, length}, "ftruncate_result");
}

// __unlink(path: static_ptr<u8>) -> i32
Value* CodegenVisitor::codegenUnlink(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("__unlink expects 1 argument: (path: static_ptr<u8>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* path = codegen(*expr.getArgs()[0]);
  if (!path) return nullptr;

  // Extract raw pointer from static_ptr struct
  if (path->getType()->isStructTy()) {
    path = ctx.builder->CreateExtractValue(path, 0, "path.data");
  }

  Function* helper = getOrCreateUnlinkHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {path}, "unlink_result");
}

// __rename(old_path: static_ptr<u8>, new_path: static_ptr<u8>) -> i32
Value* CodegenVisitor::codegenRename(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError(
        "__rename expects 2 arguments: (old_path, new_path: static_ptr<u8>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* oldPath = codegen(*expr.getArgs()[0]);
  if (!oldPath) return nullptr;
  Value* newPath = codegen(*expr.getArgs()[1]);
  if (!newPath) return nullptr;

  // Extract raw pointers from static_ptr structs
  if (oldPath->getType()->isStructTy()) {
    oldPath = ctx.builder->CreateExtractValue(oldPath, 0, "old_path.data");
  }
  if (newPath->getType()->isStructTy()) {
    newPath = ctx.builder->CreateExtractValue(newPath, 0, "new_path.data");
  }

  Function* helper = getOrCreateRenameHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {oldPath, newPath}, "rename_result");
}

// __mkdir(path: static_ptr<u8>, mode: i32) -> i32
Value* CodegenVisitor::codegenMkdir(const CallExprAST& expr) {
  if (expr.getArgs().size() != 2) {
    logAndThrowError(
        "__mkdir expects 2 arguments: (path: static_ptr<u8>, mode: i32)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* path = codegen(*expr.getArgs()[0]);
  if (!path) return nullptr;
  Value* mode = codegen(*expr.getArgs()[1]);
  if (!mode) return nullptr;

  // Extract raw pointer from static_ptr struct
  if (path->getType()->isStructTy()) {
    path = ctx.builder->CreateExtractValue(path, 0, "path.data");
  }
  if (!mode->getType()->isIntegerTy(32)) {
    mode = ctx.builder->CreateSExtOrTrunc(mode, Type::getInt32Ty(llvmCtx));
  }

  Function* helper = getOrCreateMkdirHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {path, mode}, "mkdir_result");
}

// __rmdir(path: static_ptr<u8>) -> i32
Value* CodegenVisitor::codegenRmdir(const CallExprAST& expr) {
  if (expr.getArgs().size() != 1) {
    logAndThrowError("__rmdir expects 1 argument: (path: static_ptr<u8>)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* path = codegen(*expr.getArgs()[0]);
  if (!path) return nullptr;

  // Extract raw pointer from static_ptr struct
  if (path->getType()->isStructTy()) {
    path = ctx.builder->CreateExtractValue(path, 0, "path.data");
  }

  Function* helper = getOrCreateRmdirHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {path}, "rmdir_result");
}

// __write(fd: i32, buf: raw_ptr<u8>, len: i64) -> i64
Value* CodegenVisitor::codegenWrite(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__write expects 3 arguments: (fd: i32, buf: raw_ptr<u8>, len: i64)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* buf = codegen(*expr.getArgs()[1]);
  if (!buf) return nullptr;
  Value* len = codegen(*expr.getArgs()[2]);
  if (!len) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!len->getType()->isIntegerTy(64)) {
    len = ctx.builder->CreateSExtOrTrunc(len, Type::getInt64Ty(llvmCtx));
  }

  Function* helper = getOrCreateWriteHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, buf, len}, "write_result");
}

// __read(fd: i32, buf: raw_ptr<u8>, len: i64) -> i64
Value* CodegenVisitor::codegenRead(const CallExprAST& expr) {
  if (expr.getArgs().size() != 3) {
    logAndThrowError(
        "__read expects 3 arguments: (fd: i32, buf: raw_ptr<u8>, len: i64)");
    return nullptr;
  }

  LLVMContext& llvmCtx = ctx.getContext();
  Value* fd = codegen(*expr.getArgs()[0]);
  if (!fd) return nullptr;
  Value* buf = codegen(*expr.getArgs()[1]);
  if (!buf) return nullptr;
  Value* len = codegen(*expr.getArgs()[2]);
  if (!len) return nullptr;

  if (!fd->getType()->isIntegerTy(32)) {
    fd = ctx.builder->CreateSExtOrTrunc(fd, Type::getInt32Ty(llvmCtx));
  }
  if (!len->getType()->isIntegerTy(64)) {
    len = ctx.builder->CreateSExtOrTrunc(len, Type::getInt64Ty(llvmCtx));
  }

  Function* helper = getOrCreateReadHelper(module, llvmCtx);
  return ctx.builder->CreateCall(helper, {fd, buf, len}, "read_result");
}
