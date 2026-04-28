// variable_creation.cpp - Variable creation codegen methods

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"
#include "llvm/IR/InlineAsm.h"

using namespace llvm;

// -------------------------------------------------------------------
// Global variable creation
// -------------------------------------------------------------------

GlobalVariable* CodegenVisitor::createGlobalVariable(
    const std::string& name, llvm::Type* type, llvm::Constant* initializer) {
  // Create appropriate zero initializer if none provided
  if (!initializer) {
    if (type->isDoubleTy()) {
      initializer = ConstantFP::get(ctx.getContext(), APFloat(0.0));
    } else if (type->isFloatTy()) {
      initializer = ConstantFP::get(type, 0.0f);
    } else if (type->isIntegerTy()) {
      initializer = ConstantInt::get(type, 0);
    } else if (type->isArrayTy()) {
      initializer = ConstantAggregateZero::get(type);
    } else {
      initializer = Constant::getNullValue(type);
    }
  }

  // Create new global variable
  GlobalVariable* gv = new GlobalVariable(
      *module, type, false, GlobalValue::ExternalLinkage, initializer, name);
  return gv;
}

// -------------------------------------------------------------------
// Variable creation codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const VariableCreationAST& expr) {
  // Get the type from the resolved type set by semantic analyzer
  sun::TypePtr varSunType = expr.getResolvedType();
  if (!varSunType) {
    logAndThrowError(
        "Variable declaration has no type (semantic analysis may have failed)");
  }
  if (varSunType->isTypeParameter()) {
    logAndThrowError(
        "Semantic analysis failed to substitute type parameter for variable: " +
        expr.getName());
  }

  // Use qualified name from semantic analysis
  std::string varName = expr.getQualifiedName();

  // Check if we're creating a global variable and if it already exists
  if (scopes.empty()) {
    if (module->getGlobalVariable(varName)) {
      logAndThrowError("Cannot redeclare global variable: " + varName);
    }
  }

  bool isLambdaType = varSunType->isLambda();

  // Only lambda literals can be assigned to variables (not named functions)
  if (expr.getValue()->isFunction()) {
    logAndThrowError(
        "Cannot assign a named function to a variable. Use a lambda instead: " +
        varName);
  }

  // If the value is a lambda literal, generate it and store its pointer
  if (expr.getValue()->isLambda()) {
    if (!isLambdaType) {
      logAndThrowError(
          "Type mismatch: expected lambda type for lambda literal variable: " +
          varName);
    }
    return genFunctionVariable(expr);
  }

  // Resolve the LLVM type (handles function types -> ptr, lambda types ->
  // closure struct)
  llvm::Type* varType = typeResolver.resolve(varSunType);

  if (scopes.empty()) {
    // Global arrays need special handling - they can't use stack allocations
    if (varSunType->isArray()) {
      return genGlobalArray(expr);
    }
    // Global class variables need runtime initialization
    if (varSunType->isClass()) {
      auto classType = std::dynamic_pointer_cast<sun::ClassType>(varSunType);
      if (classType) {
        return genGlobalClassVar(expr, *classType);
      }
    }
    return genGlobalVarForConstantExpr(expr, varType);
  }

  return genLocalVar(expr, varType);
}

// -------------------------------------------------------------------
// Lambda variable creation
// -------------------------------------------------------------------

Value* CodegenVisitor::genFunctionVariable(const VariableCreationAST& expr) {
  if (!expr.getValue()->isLambda()) {
    logAndThrowError("Expected lambda literal for lambda type variable: " +
                     expr.getName());
  }

  // Use qualified name from semantic analysis
  std::string varName = expr.getQualifiedName();

  // Generate the lambda
  auto& lambdaAst =
      static_cast<LambdaAST&>(const_cast<ExprAST&>(*expr.getValue()));
  llvm::Value* resultPtr = codegenLambda(lambdaAst);

  if (!resultPtr) {
    logAndThrowError("Failed to generate lambda: " + varName);
  }

  // Lambda: resultPtr is an alloca containing the closure struct (or constant
  // for global)
  llvm::Type* varType = resultPtr->getType();

  if (scopes.empty()) {
    // Top-level: use global variable for closure struct
    createGlobalVariable(varName, varType,
                         llvm::dyn_cast<llvm::Constant>(resultPtr));
  } else {
    // Inside a function: resultPtr is already an alloca from createFatClosure
    // that holds the closure struct. Just register it in the scope.
    auto& scope = scopes.back().variables;
    if (auto* fatAlloca = llvm::dyn_cast<AllocaInst>(resultPtr)) {
      // resultPtr is already an alloca containing the closure struct - use it
      // directly
      fatAlloca->setName(varName);
      scope[expr.getName()] = fatAlloca;
    } else {
      // Fallback: create a new alloca and store the value
      Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
      AllocaInst* alloca =
          createEntryBlockAlloca(currentFunc, varName, varType);
      ctx.builder->CreateStore(resultPtr, alloca);
      scope[expr.getName()] = alloca;
    }
  }
  return resultPtr;
}

// -------------------------------------------------------------------
// Local variable creation
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// Error union unwrapping helper
// -------------------------------------------------------------------

llvm::Value* CodegenVisitor::unwrapErrorUnion(llvm::Value* value,
                                              llvm::Type* expectedType,
                                              llvm::Function* func) {
  // Check if value is an error union struct but expectedType is the inner type
  if (!value->getType()->isStructTy() || expectedType->isStructTy()) {
    return value;  // Not an error union, return unchanged
  }

  auto* structType = cast<StructType>(value->getType());
  // Check if this looks like an error union: { i1, T }
  if (structType->getNumElements() != 2 ||
      !structType->getElementType(0)->isIntegerTy(1)) {
    return value;  // Not an error union format
  }

  // Extract isError flag and inner value
  Value* isError = ctx.builder->CreateExtractValue(value, 0, "call.isError");
  Value* innerValue = ctx.builder->CreateExtractValue(value, 1, "call.value");

  // If we're in an error-returning function, propagate the error
  if (currentFunctionCanError) {
    BasicBlock* errorBB =
        BasicBlock::Create(ctx.getContext(), "call.error", func);
    BasicBlock* successBB =
        BasicBlock::Create(ctx.getContext(), "call.success", func);

    ctx.builder->CreateCondBr(isError, errorBB, successBB);

    // Error block: return error from this function
    ctx.builder->SetInsertPoint(errorBB);
    llvm::Type* retType = func->getReturnType();
    Value* errorStruct = UndefValue::get(retType);
    errorStruct = ctx.builder->CreateInsertValue(
        errorStruct, ConstantInt::getTrue(ctx.getContext()), 0);
    if (currentFunctionValueType) {
      Value* zeroVal = Constant::getNullValue(currentFunctionValueType);
      errorStruct = ctx.builder->CreateInsertValue(errorStruct, zeroVal, 1);
    }
    ctx.builder->CreateRet(errorStruct);

    // Success block: continue with the value
    ctx.builder->SetInsertPoint(successBB);
  }

  return innerValue;
}

// -------------------------------------------------------------------
// Local variable creation
// -------------------------------------------------------------------

llvm::Value* CodegenVisitor::genLocalVar(const VariableCreationAST& expr,
                                         llvm::Type* varType) {
  Value* value = codegen(*expr.getValue());
  if (!value) return nullptr;

  // Inside a function: use local alloca
  auto& scope = scopes.back().variables;
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  sun::TypePtr varSunType = expr.getResolvedType();

  // Handle error union unwrapping first
  value = unwrapErrorUnion(value, varType, func);

  // Handle interface types
  if (varSunType && varSunType->isInterface()) {
    sun::TypePtr valueSunType = expr.getValue()->getResolvedType();
    // Unwrap reference if needed
    if (valueSunType && valueSunType->isReference()) {
      valueSunType = static_cast<sun::ReferenceType*>(valueSunType.get())
                         ->getReferencedType();
    }

    // Case 1: Class to interface conversion - create fat pointer
    if (valueSunType && valueSunType->isClass()) {
      auto* classType = static_cast<sun::ClassType*>(valueSunType.get());
      auto* ifaceType = static_cast<sun::InterfaceType*>(varSunType.get());

      // value is the object pointer (alloca or struct pointer)
      Value* fatPtr = createInterfaceFatPointer(value, classType, ifaceType);
      if (!fatPtr) return nullptr;

      // Create alloca for the fat pointer and store it
      AllocaInst* alloca =
          createEntryBlockAlloca(func, expr.getName(), fatPtr->getType());
      ctx.builder->CreateStore(fatPtr, alloca);
      scope[expr.getName()] = alloca;
      return fatPtr;
    }

    // Case 2: Interface to interface (already a fat pointer)
    // Value may be a pointer (from materialized struct return) or struct value
    if (valueSunType && valueSunType->isInterface()) {
      llvm::StructType* fatPtrType =
          sun::InterfaceType::getFatPointerType(ctx.getContext());
      Value* fatPtrVal = value;

      // If value is a pointer, load the fat pointer struct
      if (value->getType()->isPointerTy()) {
        fatPtrVal = ctx.builder->CreateLoad(fatPtrType, value, "iface.load");
      }

      // Create alloca for the fat pointer and store it
      AllocaInst* alloca =
          createEntryBlockAlloca(func, expr.getName(), fatPtrType);
      ctx.builder->CreateStore(fatPtrVal, alloca);
      scope[expr.getName()] = alloca;
      return fatPtrVal;
    }
  }

  // For array and class types, use the alloca directly
  // instead of creating a new alloca and storing a pointer
  if (varType->isArrayTy() || (varSunType && varSunType->isClass())) {
    if (auto* allocaValue = dyn_cast<AllocaInst>(value)) {
      allocaValue->setName(expr.getName());
      scope[expr.getName()] = allocaValue;

      // Track class allocation for automatic deinit at scope exit
      if (varSunType && varSunType->isClass()) {
        auto classType = std::dynamic_pointer_cast<sun::ClassType>(varSunType);
        if (classType) {
          trackClassAllocation(allocaValue, expr.getName(), classType);
        }
      }
      return allocaValue;
    }

    // For class types, if value is a struct (from array indexing),
    // create alloca of the STRUCT type and store the struct value
    if (varSunType && varSunType->isClass() && value->getType()->isStructTy()) {
      llvm::Type* structType = value->getType();
      AllocaInst* alloca =
          createEntryBlockAlloca(func, expr.getName(), structType);
      ctx.builder->CreateStore(value, alloca);
      scope[expr.getName()] = alloca;

      // Track class allocation for automatic deinit at scope exit
      auto classType = std::dynamic_pointer_cast<sun::ClassType>(varSunType);
      if (classType) {
        trackClassAllocation(alloca, expr.getName(), classType);
      }
      return alloca;
    }

    // For class types, if value is a pointer to struct (from member access),
    // implement MOVE SEMANTICS: load the struct, zero out the source field,
    // and track the destination for deinit. This prevents double-free.
    if (varSunType && varSunType->isClass() &&
        value->getType()->isPointerTy()) {
      auto classType = std::dynamic_pointer_cast<sun::ClassType>(varSunType);
      if (classType) {
        llvm::StructType* structType =
            classType->getStructType(ctx.getContext());
        // Load the struct value from the source pointer
        Value* structVal =
            ctx.builder->CreateLoad(structType, value, "move.val");
        // Create a new alloca for this variable (the move destination)
        AllocaInst* alloca =
            createEntryBlockAlloca(func, expr.getName(), structType);
        ctx.builder->CreateStore(structVal, alloca);
        scope[expr.getName()] = alloca;

        // MOVE SEMANTICS: Zero out the source field to prevent double-free
        // when the original object's deinit runs. The source expression
        // was a member access, so 'value' is a pointer to the embedded struct.
        // We zero the entire struct so its deinit (if called) does nothing.
        llvm::FunctionCallee memsetFn = module->getOrInsertFunction(
            "memset",
            FunctionType::get(PointerType::getUnqual(ctx.getContext()),
                              {PointerType::getUnqual(ctx.getContext()),
                               Type::getInt32Ty(ctx.getContext()),
                               Type::getInt64Ty(ctx.getContext())},
                              false));
        const DataLayout& DL = module->getDataLayout();
        uint64_t structSize = DL.getTypeAllocSize(structType);
        ctx.builder->CreateCall(
            memsetFn,
            {value, ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0),
             ConstantInt::get(Type::getInt64Ty(ctx.getContext()), structSize)});

        // Track the destination for deinit - it now owns the data
        trackClassAllocation(alloca, expr.getName(), classType);
        return alloca;
      }
    }
  }

  // Handle implicit type conversion for integers (e.g., i8 to i32)
  llvm::Type* valueType = value->getType();
  if (valueType != varType) {
    // Integer widening
    if (valueType->isIntegerTy() && varType->isIntegerTy()) {
      unsigned valueBits = valueType->getIntegerBitWidth();
      unsigned varBits = varType->getIntegerBitWidth();
      if (valueBits < varBits) {
        value = ctx.builder->CreateSExt(value, varType, "widen");
      } else if (valueBits > varBits) {
        value = ctx.builder->CreateTrunc(value, varType, "trunc");
      }
    }
    // Float widening
    else if (valueType->isFloatTy() && varType->isDoubleTy()) {
      value = ctx.builder->CreateFPExt(value, varType, "widen");
    } else if (valueType->isDoubleTy() && varType->isFloatTy()) {
      value = ctx.builder->CreateFPTrunc(value, varType, "trunc");
    }
  }

  AllocaInst* alloca = createEntryBlockAlloca(func, expr.getName(), varType);
  ctx.builder->CreateStore(value, alloca);
  scope[expr.getName()] = alloca;

  return value;
}

// -------------------------------------------------------------------
// Global array creation
// -------------------------------------------------------------------

// Helper to generate a constant element value for global arrays
static llvm::Constant* genConstantElement(const ExprAST* elemExpr,
                                          llvm::LLVMContext& llvmCtx) {
  switch (elemExpr->getType()) {
    case ASTNodeType::NUMBER: {
      const auto& numExpr = static_cast<const NumberExprAST&>(*elemExpr);
      auto elemType = elemExpr->getResolvedType();
      if (!elemType) return nullptr;

      if (elemType->isFloat32() || elemType->isFloat64()) {
        llvm::Type* ty = elemType->isFloat32()
                             ? llvm::Type::getFloatTy(llvmCtx)
                             : llvm::Type::getDoubleTy(llvmCtx);
        return llvm::ConstantFP::get(ty, numExpr.getVal());
      } else {
        // Integer type
        llvm::Type* ty = elemType->toLLVMType(llvmCtx);
        return llvm::ConstantInt::get(ty,
                                      static_cast<int64_t>(numExpr.getVal()),
                                      /*isSigned=*/true);
      }
    }
    default:
      return nullptr;
  }
}

// Recursively build a constant array for multi-dimensional global arrays
static llvm::Constant* buildConstantArray(
    const std::vector<std::unique_ptr<ExprAST>>& elements,
    const std::vector<size_t>& dims, size_t dimIndex, llvm::Type* elementType,
    llvm::LLVMContext& llvmCtx) {
  if (dimIndex >= dims.size()) {
    // This shouldn't happen - means we've gone too deep
    return nullptr;
  }

  std::vector<llvm::Constant*> constElements;
  constElements.reserve(elements.size());

  for (const auto& elem : elements) {
    if (elem->getType() == ASTNodeType::ARRAY_LITERAL) {
      // Nested array - recurse
      const auto& nestedArray = static_cast<const ArrayLiteralAST&>(*elem);
      llvm::Constant* nestedConst = buildConstantArray(
          nestedArray.getElements(), dims, dimIndex + 1, elementType, llvmCtx);
      if (!nestedConst) return nullptr;
      constElements.push_back(nestedConst);
    } else {
      // Scalar element
      llvm::Constant* elemConst = genConstantElement(elem.get(), llvmCtx);
      if (!elemConst) return nullptr;
      constElements.push_back(elemConst);
    }
  }

  // Determine the type for this level
  llvm::Type* arrayType = elementType;
  for (size_t i = dims.size() - 1; i > dimIndex; --i) {
    arrayType = llvm::ArrayType::get(arrayType, dims[i]);
  }
  llvm::ArrayType* thisLevelType =
      llvm::ArrayType::get(arrayType, dims[dimIndex]);

  return llvm::ConstantArray::get(thisLevelType, constElements);
}

llvm::Constant* CodegenVisitor::genGlobalArray(
    const VariableCreationAST& expr) {
  assert(scopes.empty() && "genGlobalArray should only be called at top-level");

  sun::TypePtr varType = expr.getResolvedType();
  if (!varType || !varType->isArray()) {
    logAndThrowError("genGlobalArray called on non-array type: " +
                     expr.getName());
  }

  auto* arrayType = static_cast<sun::ArrayType*>(varType.get());
  const auto& dims = arrayType->getDimensions();

  if (dims.empty()) {
    logAndThrowError("Cannot create global unsized array: " + expr.getName());
  }

  // Get the array literal
  if (expr.getValue()->getType() != ASTNodeType::ARRAY_LITERAL) {
    logAndThrowError("Global array must be initialized with array literal: " +
                     expr.getName());
  }
  const auto& arrayLit = static_cast<const ArrayLiteralAST&>(*expr.getValue());

  // Build the constant data array
  llvm::Type* elementLLVMType =
      arrayType->getElementType()->toLLVMType(ctx.getContext());
  llvm::Constant* dataConst = buildConstantArray(
      arrayLit.getElements(), dims, 0, elementLLVMType, ctx.getContext());

  if (!dataConst) {
    logAndThrowError("Failed to generate constant data for global array: " +
                     expr.getName());
  }

  // Create global variable for data storage
  std::string varName = expr.getQualifiedName();
  llvm::GlobalVariable* dataGV = new llvm::GlobalVariable(
      *module, dataConst->getType(), /*isConstant=*/false,
      llvm::GlobalValue::InternalLinkage, dataConst, varName + ".data");

  // Build constant dims array
  std::vector<llvm::Constant*> dimValues;
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  for (int64_t d : dims) {
    dimValues.push_back(llvm::ConstantInt::get(i64Ty, d));
  }
  llvm::ArrayType* dimsArrayType = llvm::ArrayType::get(i64Ty, dims.size());
  llvm::Constant* dimsConst =
      llvm::ConstantArray::get(dimsArrayType, dimValues);

  // Create global variable for dims array
  llvm::GlobalVariable* dimsGV = new llvm::GlobalVariable(
      *module, dimsArrayType, /*isConstant=*/true,
      llvm::GlobalValue::InternalLinkage, dimsConst, varName + ".dims");

  // Build fat struct constant: { ptr data, i32 ndims, ptr dims }
  llvm::StructType* fatType =
      sun::ArrayType::getArrayStructType(ctx.getContext());

  // Get pointers to first element of data and dims
  llvm::Constant* zero =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()), 0);
  llvm::Constant* dataPtr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      dataConst->getType(), dataGV,
      llvm::ArrayRef<llvm::Constant*>{zero, zero});
  llvm::Constant* dimsPtr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      dimsArrayType, dimsGV, llvm::ArrayRef<llvm::Constant*>{zero, zero});

  llvm::Constant* fatStruct = llvm::ConstantStruct::get(
      fatType, {dataPtr,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()),
                                       dims.size()),
                dimsPtr});

  // Create the global variable for the fat struct
  createGlobalVariable(varName, fatType, fatStruct);
  return fatStruct;
}

// -------------------------------------------------------------------
// Global variable creation for constant expressions
// -------------------------------------------------------------------

llvm::Constant* CodegenVisitor::genGlobalVarForConstantExpr(
    const VariableCreationAST& expr, llvm::Type* varType) {
  assert(scopes.empty() &&
         "genGlobalVarForConstantExpr should only be called at top-level");
  assert(!varType->isFunctionTy() &&
         "Function types should be handled separately");
  // At top-level, we need a constant initializer for the global variable
  // Generate the value - it must be a constant at global scope
  Value* value = codegen(*expr.getValue());
  if (!value) return nullptr;

  // Convert to constant - global initializers must be constants
  Constant* constValue = dyn_cast<Constant>(value);
  if (!constValue) {
    logAndThrowError(
        "Global variable initializer must be a constant expression: " +
        expr.getName());
  }

  // Convert value to the correct type if needed
  if (constValue->getType() != varType) {
    if (varType->isIntegerTy() && constValue->getType()->isFloatingPointTy()) {
      // Constant fold: FP to Int
      if (auto* fpConst = dyn_cast<ConstantFP>(constValue)) {
        APFloat fpVal = fpConst->getValueAPF();
        bool isExact;
        APSInt intVal(varType->getIntegerBitWidth(), /*isUnsigned=*/false);
        fpVal.convertToInteger(intVal, APFloat::rmTowardZero, &isExact);
        constValue = ConstantInt::get(varType, intVal);
      }
    } else if (varType->isFloatingPointTy() &&
               constValue->getType()->isIntegerTy()) {
      // Constant fold: Int to FP
      if (auto* intConst = dyn_cast<ConstantInt>(constValue)) {
        double fpVal = intConst->getSExtValue();
        constValue = ConstantFP::get(varType, fpVal);
      }
    } else if (varType->isIntegerTy() && constValue->getType()->isIntegerTy()) {
      // Integer cast - extract value and create new constant
      if (auto* intConst = dyn_cast<ConstantInt>(constValue)) {
        int64_t val = intConst->getSExtValue();
        constValue = ConstantInt::get(varType, val, /*isSigned=*/true);
      }
    } else if (varType->isFloatingPointTy() &&
               constValue->getType()->isFloatingPointTy()) {
      // FP cast - extract value and create new constant
      if (auto* fpConst = dyn_cast<ConstantFP>(constValue)) {
        double val = fpConst->getValueAPF().convertToDouble();
        constValue = ConstantFP::get(varType, val);
      }
    }
  }

  // Create global variable with the constant initializer
  std::string varName = expr.getQualifiedName();
  createGlobalVariable(varName, varType, constValue);
  return constValue;
}

// -------------------------------------------------------------------
// Scope cleanup for owned allocations
// -------------------------------------------------------------------

// Helper: emit cleanup code for ptr<T> and raw_ptr<i8> fields in a class
// instance. Recursively frees pointer fields before the containing object is
// freed.
void CodegenVisitor::emitFieldCleanup(llvm::Value* objectPtr,
                                      const sun::ClassType* classType,
                                      const std::string& baseName,
                                      llvm::FunctionCallee freeFunc) {
  if (!classType) return;

  llvm::StructType* structType = classType->getStructType(ctx.getContext());

  auto* nullPtr = llvm::ConstantPointerNull::get(
      llvm::PointerType::getUnqual(ctx.getContext()));
  llvm::Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();

  for (const auto& field : classType->getFields()) {
    if (field.type->isRawPointer()) {
      // raw_ptr<T> fields are also freed - used for dynamic data allocations
      // in classes that manage their own memory
      llvm::Value* fieldPtr =
          ctx.builder->CreateStructGEP(structType, objectPtr, field.index,
                                       baseName + "." + field.name + ".ptr");

      llvm::Value* fieldValue = ctx.builder->CreateLoad(
          llvm::PointerType::getUnqual(ctx.getContext()), fieldPtr,
          baseName + "." + field.name + ".value");

      // Null-check raw_ptr fields too
      llvm::BasicBlock* freeRawBB = llvm::BasicBlock::Create(
          ctx.getContext(), baseName + "." + field.name + ".free_raw",
          currentFunc);
      llvm::BasicBlock* skipRawBB = llvm::BasicBlock::Create(
          ctx.getContext(), baseName + "." + field.name + ".skip_raw",
          currentFunc);

      llvm::Value* isRawNull = ctx.builder->CreateICmpEQ(
          fieldValue, nullPtr, baseName + "." + field.name + ".raw_is_null");
      ctx.builder->CreateCondBr(isRawNull, skipRawBB, freeRawBB);

      ctx.builder->SetInsertPoint(freeRawBB);
      ctx.builder->CreateCall(freeFunc, {fieldValue});
      ctx.builder->CreateStore(nullPtr, fieldPtr);
      ctx.builder->CreateBr(skipRawBB);

      ctx.builder->SetInsertPoint(skipRawBB);
    } else if (field.type->isClass()) {
      // Embedded class field - recursively call deinit on it if it has one
      auto* nestedClass = static_cast<sun::ClassType*>(field.type.get());

      // Generate GEP to access the embedded struct field
      llvm::Value* fieldPtr = ctx.builder->CreateStructGEP(
          structType, objectPtr, field.index, baseName + "." + field.name);

      // Recursively emit field cleanup and deinit for the nested class
      emitFieldCleanup(fieldPtr, nestedClass, baseName + "." + field.name,
                       freeFunc);
    }
  }
}

// Helper: emit deinit calls for class fields that have deinit methods
// Recursively calls deinit on nested class fields
void CodegenVisitor::emitFieldDeinit(llvm::Value* objectPtr,
                                     const sun::ClassType* classType,
                                     const std::string& baseName) {
  if (!classType) return;

  llvm::StructType* structType = classType->getStructType(ctx.getContext());

  for (const auto& field : classType->getFields()) {
    if (field.type->isClass()) {
      auto* nestedClass = static_cast<sun::ClassType*>(field.type.get());

      // Generate GEP to access the embedded struct field
      llvm::Value* fieldPtr = ctx.builder->CreateStructGEP(
          structType, objectPtr, field.index, baseName + "." + field.name);

      // Check if the nested class has a deinit method
      const sun::ClassMethod* deinitMethod = nestedClass->getMethod("deinit");
      if (deinitMethod) {
        // Get or declare the deinit function
        std::string mangledName = nestedClass->getMangledMethodName("deinit");
        llvm::Function* deinitFunc = module->getFunction(mangledName);

        if (!deinitFunc) {
          // Create declaration for deinit: void deinit(this*)
          std::vector<llvm::Type*> paramTypes;
          paramTypes.push_back(llvm::PointerType::getUnqual(ctx.getContext()));

          llvm::FunctionType* funcType = llvm::FunctionType::get(
              llvm::Type::getVoidTy(ctx.getContext()), paramTypes, false);
          deinitFunc = llvm::Function::Create(
              funcType, llvm::Function::ExternalLinkage, mangledName, module);
        }

        // Call deinit on the field
        ctx.builder->CreateCall(deinitFunc, {fieldPtr});
      }

      // Recursively deinit nested class fields
      emitFieldDeinit(fieldPtr, nestedClass, baseName + "." + field.name);
    }
  }
}

void CodegenVisitor::emitScopeCleanup() {
  // First, cleanup class allocations (call deinit methods)
  if (!classAllocations.empty()) {
    auto& currentClassScope = classAllocations.back();

    // Deinit all non-moved class allocations in reverse order (LIFO)
    for (auto it = currentClassScope.rbegin(); it != currentClassScope.rend();
         ++it) {
      if (!it->moved && it->alloca && it->type) {
        // Check if the class has a deinit method
        const sun::ClassMethod* deinitMethod = it->type->getMethod("deinit");
        if (deinitMethod) {
          // Get or declare the deinit function
          std::string mangledName = it->type->getMangledMethodName("deinit");
          llvm::Function* deinitFunc = module->getFunction(mangledName);

          if (!deinitFunc) {
            // Create declaration for deinit: void deinit(this*)
            std::vector<llvm::Type*> paramTypes;
            paramTypes.push_back(
                llvm::PointerType::getUnqual(ctx.getContext()));

            llvm::FunctionType* funcType = llvm::FunctionType::get(
                llvm::Type::getVoidTy(ctx.getContext()), paramTypes, false);
            deinitFunc = llvm::Function::Create(
                funcType, llvm::Function::ExternalLinkage, mangledName, module);
          }

          // Call deinit on the class instance
          ctx.builder->CreateCall(deinitFunc, {it->alloca});
        }

        // Recursively deinit class fields that have deinit methods
        emitFieldDeinit(it->alloca, it->type.get(), it->varName);
      }
    }
  }

  // Then, cleanup owned pointer allocations (ptr<T>)
  if (ownedAllocations.empty()) return;

  auto& currentScope = ownedAllocations.back();
  if (currentScope.empty()) return;

  // Get or declare free function: void free(ptr)
  llvm::FunctionType* freeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(ctx.getContext()),
      {llvm::PointerType::getUnqual(ctx.getContext())}, false);
  llvm::FunctionCallee freeFunc = module->getOrInsertFunction("free", freeType);

  auto* ptrTy = llvm::PointerType::getUnqual(ctx.getContext());

  auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
  llvm::Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();

  // Free all non-moved allocations in reverse order (LIFO)
  for (auto it = currentScope.rbegin(); it != currentScope.rend(); ++it) {
    if (!it->moved && it->ptrAlloca) {
      // Load the pointer from the alloca
      llvm::Value* ptrToFree = ctx.builder->CreateLoad(
          llvm::PointerType::getUnqual(ctx.getContext()), it->ptrAlloca,
          it->varName + ".ptr_to_free");

      // Null-check: skip freeing if the pointer is null
      llvm::BasicBlock* freeBB = llvm::BasicBlock::Create(
          ctx.getContext(), it->varName + ".cleanup", currentFunc);
      llvm::BasicBlock* skipBB = llvm::BasicBlock::Create(
          ctx.getContext(), it->varName + ".skip_cleanup", currentFunc);

      llvm::Value* isNull = ctx.builder->CreateICmpEQ(ptrToFree, nullPtr,
                                                      it->varName + ".is_null");
      ctx.builder->CreateCondBr(isNull, skipBB, freeBB);

      ctx.builder->SetInsertPoint(freeBB);

      if (it->isMmap && it->sizeAlloca) {
        // For mmap allocations, use munmap syscall (syscall 11 on x86_64)
        llvm::Value* size =
            ctx.builder->CreateLoad(llvm::Type::getInt64Ty(ctx.getContext()),
                                    it->sizeAlloca, it->varName + ".mmap_size");

        // munmap(addr, length) - syscall 11
        auto* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
        std::vector<llvm::Type*> paramTypes = {i64Ty, i64Ty, i64Ty, i64Ty};
        llvm::FunctionType* asmType =
            llvm::FunctionType::get(i64Ty, paramTypes, false);
        llvm::InlineAsm* syscallAsm = llvm::InlineAsm::get(
            asmType, "syscall",
            "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}",
            /*hasSideEffects=*/true, /*isAlignStack=*/false,
            llvm::InlineAsm::AD_ATT);

        llvm::Value* sysno = llvm::ConstantInt::get(i64Ty, 11);  // sys_munmap
        llvm::Value* addr = ctx.builder->CreatePtrToInt(ptrToFree, i64Ty);
        llvm::Value* zero = llvm::ConstantInt::get(i64Ty, 0);
        ctx.builder->CreateCall(syscallAsm, {sysno, addr, size, zero});
      } else {
        // For malloc allocations:
        // First, recursively free any ptr<T> fields if pointee is a class
        if (it->pointeeType && it->pointeeType->isClass()) {
          auto* classType = static_cast<sun::ClassType*>(it->pointeeType.get());
          emitFieldCleanup(ptrToFree, classType, it->varName, freeFunc);
        }
        // Then free the object itself
        ctx.builder->CreateCall(freeFunc, {ptrToFree});
      }

      // Null out the pointer to prevent double-free
      ctx.builder->CreateStore(nullPtr, it->ptrAlloca);

      ctx.builder->CreateBr(skipBB);
      ctx.builder->SetInsertPoint(skipBB);
    }
  }
}

// -------------------------------------------------------------------
// Reference creation codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const ReferenceCreationAST& expr) {
  sun::TypePtr refSunType = expr.getResolvedType();
  if (!refSunType || !refSunType->isReference()) {
    logAndThrowError("Reference creation has no type or invalid type");
  }

  // A reference stores a pointer to the target variable.
  // This makes refs consistent with function parameters (both use ptr
  // indirection).
  const ExprAST* target = expr.getTarget();

  if (target->getType() != ASTNodeType::VARIABLE_REFERENCE) {
    logAndThrowError("Reference target must be a variable");
  }

  const auto& varRef = static_cast<const VariableReferenceAST&>(*target);

  // Find the target - either a local alloca or a global variable
  llvm::Value* targetPtr = findVariable(varRef.getName());
  if (!targetPtr) {
    targetPtr = module->getGlobalVariable(varRef.getName());
  }
  if (!targetPtr) {
    logAndThrowError("Cannot create reference to unknown variable: " +
                     varRef.getName());
  }

  // Create an alloca that holds a pointer to the target
  std::string refName = expr.getQualifiedName();
  llvm::Type* ptrType = llvm::PointerType::getUnqual(ctx.getContext());
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* refAlloca = createEntryBlockAlloca(func, refName, ptrType);
  ctx.builder->CreateStore(targetPtr, refAlloca);

  if (!scopes.empty()) {
    scopes.back().variables[refName] = refAlloca;
  }
  return refAlloca;
}

// -------------------------------------------------------------------
// Global class variable creation
// -------------------------------------------------------------------

GlobalVariable* CodegenVisitor::genGlobalClassVar(
    const VariableCreationAST& expr, sun::ClassType& classType) {
  assert(scopes.empty() &&
         "genGlobalClassVar should only be called at top-level");

  // Get the LLVM struct type for the class
  llvm::StructType* structType = classType.getStructType(ctx.getContext());

  // Create zero-initialized global variable for the class instance
  std::string varName = expr.getQualifiedName();
  llvm::Constant* zeroInit = llvm::ConstantAggregateZero::get(structType);
  GlobalVariable* gv = new GlobalVariable(
      *module, structType,
      /*isConstant=*/false, GlobalValue::ExternalLinkage, zeroInit, varName);

  // Queue for runtime initialization
  StaticInitInfo info;
  info.globalVar = gv;
  info.varName = expr.getName();
  info.varType = expr.getResolvedType();
  info.classType =
      std::dynamic_pointer_cast<sun::ClassType>(expr.getResolvedType());
  info.initExpr = expr.getValue();
  staticInits.push_back(std::move(info));

  return gv;
}

// -------------------------------------------------------------------
// Global variable with runtime initialization (non-class)
// -------------------------------------------------------------------

GlobalVariable* CodegenVisitor::genGlobalVarWithRuntimeInit(
    const VariableCreationAST& expr, llvm::Type* varType) {
  assert(scopes.empty() &&
         "genGlobalVarWithRuntimeInit should only be called at top-level");

  // Create zero-initialized global variable
  std::string varName = expr.getQualifiedName();
  llvm::Constant* zeroInit = Constant::getNullValue(varType);
  GlobalVariable* gv = new GlobalVariable(
      *module, varType,
      /*isConstant=*/false, GlobalValue::ExternalLinkage, zeroInit, varName);

  // Queue for runtime initialization
  StaticInitInfo info;
  info.globalVar = gv;
  info.varName = expr.getName();
  info.varType = expr.getResolvedType();
  info.classType = nullptr;
  info.initExpr = expr.getValue();
  staticInits.push_back(std::move(info));

  return gv;
}

// -------------------------------------------------------------------
// Emit static initialization function
// -------------------------------------------------------------------

void CodegenVisitor::emitStaticInitFunction() {
  if (staticInits.empty()) return;

  // Create the initialization function: void __sun_static_init()
  FunctionType* initFuncType =
      FunctionType::get(Type::getVoidTy(ctx.getContext()), false);
  Function* initFunc = Function::Create(initFuncType, Function::ExternalLinkage,
                                        "__sun_static_init", module);

  BasicBlock* entryBB = BasicBlock::Create(ctx.getContext(), "entry", initFunc);
  ctx.builder->SetInsertPoint(entryBB);

  // Push a scope for any temporaries needed during init
  pushScope();

  // Generate initialization code for each global variable
  for (const auto& init : staticInits) {
    GlobalVariable* gv = init.globalVar;

    if (init.classType && init.initExpr) {
      // Class type: call constructor
      sun::ClassType* classType = init.classType.get();
      llvm::StructType* structType = classType->getStructType(ctx.getContext());

      // Zero-initialize the memory using memset
      const DataLayout& DL = module->getDataLayout();
      uint64_t structSize = DL.getTypeAllocSize(structType);

      llvm::FunctionCallee memsetFn = module->getOrInsertFunction(
          "memset", FunctionType::get(PointerType::getUnqual(ctx.getContext()),
                                      {PointerType::getUnqual(ctx.getContext()),
                                       Type::getInt32Ty(ctx.getContext()),
                                       Type::getInt64Ty(ctx.getContext())},
                                      false));
      ctx.builder->CreateCall(
          memsetFn,
          {gv, ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0),
           ConstantInt::get(Type::getInt64Ty(ctx.getContext()), structSize)});

      // Get constructor arguments from the CallExprAST
      const CallExprAST* callExpr = nullptr;
      if (init.initExpr->getType() == ASTNodeType::CALL) {
        callExpr = static_cast<const CallExprAST*>(init.initExpr);
      }

      // Get the init method info from the class type
      const sun::ClassMethod* initMethod = classType->getMethod("init");
      std::string baseCtorName = classType->getMangledMethodName("init");

      // Try to find the constructor function
      Function* ctorFunc = module->getFunction(baseCtorName);

      // If not found but init method exists in class type, create declaration
      if (!ctorFunc && initMethod) {
        std::vector<llvm::Type*> paramLLVMTypes;
        paramLLVMTypes.push_back(
            PointerType::getUnqual(ctx.getContext()));  // this
        for (const auto& paramType : initMethod->paramTypes) {
          paramLLVMTypes.push_back(typeResolver.resolve(paramType));
        }
        FunctionType* funcType = FunctionType::get(
            Type::getVoidTy(ctx.getContext()), paramLLVMTypes, false);
        ctorFunc = Function::Create(funcType, Function::ExternalLinkage,
                                    baseCtorName, module);
      }

      // Call the constructor if found and argument count matches
      size_t argCount = callExpr ? callExpr->getArgs().size() : 0;
      if (ctorFunc && ctorFunc->arg_size() == argCount + 1) {
        const auto& paramTypes =
            initMethod ? initMethod->paramTypes : std::vector<sun::TypePtr>{};

        std::vector<Value*> ctorArgValues;
        ctorArgValues.push_back(gv);  // 'this' pointer is the global variable

        // Generate argument values
        if (callExpr) {
          size_t argIdx = 0;
          for (const auto& arg : callExpr->getArgs()) {
            bool isRefParam = argIdx < paramTypes.size() &&
                              paramTypes[argIdx] &&
                              paramTypes[argIdx]->isReference();

            Value* argVal = codegen(*arg);
            if (!argVal) {
              logAndThrowError(
                  "Failed to generate argument for global class constructor: " +
                  init.varName);
            }

            // Handle reference parameters
            if (isRefParam && !argVal->getType()->isPointerTy()) {
              AllocaInst* tempAlloca = createEntryBlockAlloca(
                  initFunc, "ref.temp", argVal->getType());
              ctx.builder->CreateStore(argVal, tempAlloca);
              argVal = tempAlloca;
            }

            ctorArgValues.push_back(argVal);
            ++argIdx;
          }
        }

        ctx.builder->CreateCall(ctorFunc, ctorArgValues);
      } else if (!initMethod && callExpr && argCount > 0) {
        // No explicit init method: default field-wise constructor
        // Directly assign constructor arguments to class fields in order
        const auto& fields = classType->getFields();
        size_t argIdx = 0;
        for (const auto& arg : callExpr->getArgs()) {
          if (argIdx >= fields.size()) break;

          Value* argVal = codegen(*arg);
          if (!argVal) {
            logAndThrowError(
                "Failed to generate argument for global class field init: " +
                init.varName);
          }

          // Get GEP to the field
          Value* fieldPtr =
              ctx.builder->CreateStructGEP(structType, gv, argIdx, "field.ptr");
          ctx.builder->CreateStore(argVal, fieldPtr);
          ++argIdx;
        }
      }
    } else if (init.initExpr) {
      // Non-class type: evaluate expression and store
      Value* initVal = codegen(*init.initExpr);
      if (!initVal) {
        logAndThrowError(
            "Failed to generate initializer for global variable: " +
            init.varName);
      }
      ctx.builder->CreateStore(initVal, gv);
    }
  }

  popScope();
  ctx.builder->CreateRetVoid();

  // Register the init function in llvm.global_ctors
  // This is an array of { i32 priority, ptr function, ptr data }
  llvm::StructType* ctorStructType = llvm::StructType::get(
      ctx.getContext(), {Type::getInt32Ty(ctx.getContext()),
                         PointerType::getUnqual(ctx.getContext()),
                         PointerType::getUnqual(ctx.getContext())});

  llvm::Constant* ctorEntry = llvm::ConstantStruct::get(
      ctorStructType,
      {ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 65535),  // priority
       initFunc,
       ConstantPointerNull::get(PointerType::getUnqual(ctx.getContext()))});

  llvm::ArrayType* ctorArrayType = llvm::ArrayType::get(ctorStructType, 1);
  llvm::Constant* ctorArray =
      llvm::ConstantArray::get(ctorArrayType, {ctorEntry});

  // Create or append to llvm.global_ctors
  GlobalVariable* existingCtors =
      module->getGlobalVariable("llvm.global_ctors");
  if (existingCtors) {
    // Append to existing array
    llvm::Constant* existingInit = existingCtors->getInitializer();
    if (auto* existingArray = dyn_cast<llvm::ConstantArray>(existingInit)) {
      std::vector<llvm::Constant*> entries;
      for (unsigned i = 0; i < existingArray->getNumOperands(); ++i) {
        entries.push_back(existingArray->getOperand(i));
      }
      entries.push_back(ctorEntry);
      llvm::ArrayType* newArrayType =
          llvm::ArrayType::get(ctorStructType, entries.size());
      llvm::Constant* newArray =
          llvm::ConstantArray::get(newArrayType, entries);
      existingCtors->eraseFromParent();
      new GlobalVariable(*module, newArrayType, false,
                         GlobalValue::AppendingLinkage, newArray,
                         "llvm.global_ctors");
    }
  } else {
    new GlobalVariable(*module, ctorArrayType, false,
                       GlobalValue::AppendingLinkage, ctorArray,
                       "llvm.global_ctors");
  }

  // Clear the queue
  staticInits.clear();
}
