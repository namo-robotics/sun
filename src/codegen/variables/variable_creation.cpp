// variable_creation.cpp - Variable creation codegen methods

#include "ast.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics.h"
#include "codegen/support/scalar_ops.h"
#include "codegen/support/struct_access.h"
#include "codegen/variables/variable_generator.h"

using namespace llvm;

namespace layout = sun::codegen::layout;
namespace ops = sun::codegen::ops;

// -------------------------------------------------------------------
// Global variable creation
// -------------------------------------------------------------------

GlobalVariable* VariableGenerator::createGlobalVariable(
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

void VariableGenerator::declareBlockExternGlobals(const BlockExprAST& block) {
  for (const auto& node : block.getBody()) {
    if (node->getType() != ASTNodeType::VARIABLE_CREATION) continue;
    const auto& variable = static_cast<const VariableCreationAST&>(*node);
    if (!variable.isCExtern()) continue;
    llvm::Type* type = typeResolver.resolve(variable.getResolvedType());
    gen_.externCEmitter().declareGlobal(variable, type);
  }
}

Value* VariableGenerator::codegen(const VariableCreationAST& expr) {
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
  std::string varName = expr.getMangledName();

  if (expr.isCExtern()) {
    return gen_.externCEmitter().declareGlobal(
        expr, typeResolver.resolve(varSunType));
  }

  // A global imported from a .moon is defined in the bundle's bitcode, which
  // is linked in. Declare it so references resolve; defining it here would
  // give the program a second, uninitialized copy.
  if (expr.isPrecompiled()) {
    if (GlobalVariable* existing = module->getGlobalVariable(varName)) {
      return existing;
    }
    return new GlobalVariable(*module, typeResolver.resolve(varSunType),
                              /*isConstant=*/false,
                              GlobalValue::ExternalLinkage,
                              /*Initializer=*/nullptr, varName);
  }

  // Check if we're creating a global variable and if it already exists
  if (scopes().empty()) {
    if (module->getGlobalVariable(varName)) {
      logAndThrowError("Cannot redeclare global variable: " + varName);
    }
  }

  bool isLambdaType = varSunType->isLambda();

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

  if (scopes().empty()) {
    // Global arrays need special handling - they can't use stack allocations
    if (varSunType->isArray()) {
      return genGlobalArray(expr);
    }
    if (CodegenVisitor::isPayloadEnum(varSunType)) {
      logAndThrowError(
          "Global variables of payload-carrying enum types are not yet "
          "supported",
          expr.getLocation());
    }
    // Global class variables need runtime initialization
    if (auto* classType = sun::tryGetType<sun::ClassType>(varSunType)) {
      return genGlobalClassVar(expr, *classType);
    }
    return genGlobalVarForConstantExpr(expr, varType);
  }

  return genLocalVar(expr, varType);
}

// -------------------------------------------------------------------
// Lambda variable creation
// -------------------------------------------------------------------

Value* VariableGenerator::genFunctionVariable(const VariableCreationAST& expr) {
  if (!expr.getValue()->isLambda()) {
    logAndThrowError("Expected lambda literal for lambda type variable: " +
                     expr.getName());
  }

  // Use qualified name from semantic analysis
  std::string varName = expr.getMangledName();

  // Generate the lambda
  auto& lambdaAst =
      static_cast<LambdaAST&>(const_cast<ExprAST&>(*expr.getValue()));
  llvm::Value* resultPtr = functionGen().codegenLambda(lambdaAst);

  if (!resultPtr) {
    logAndThrowError("Failed to generate lambda: " + varName);
  }

  // Lambda: resultPtr is an alloca containing the closure struct (or constant
  // for global)
  llvm::Type* varType = resultPtr->getType();

  if (scopes().empty()) {
    // Top-level: use global variable for closure struct
    createGlobalVariable(varName, varType,
                         llvm::dyn_cast<llvm::Constant>(resultPtr));
  } else {
    // Inside a function: resultPtr is already an alloca from createFatClosure
    // that holds the closure struct. Just register it in the scope.
    auto& scope = scopes().back().variables;
    if (auto* fatAlloca = llvm::dyn_cast<AllocaInst>(resultPtr)) {
      // resultPtr is already an alloca containing the closure struct - use it
      // directly
      fatAlloca->setName(varName);
      scope[expr.getName()] = fatAlloca;
      debugDeclareLocal(fatAlloca, expr.getName(), expr.getResolvedType(),
                        expr.getLocation());
    } else {
      // Fallback: create a new alloca and store the value
      Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
      AllocaInst* alloca =
          createEntryBlockAlloca(currentFunc, varName, varType);
      ctx.builder->CreateStore(resultPtr, alloca);
      scope[expr.getName()] = alloca;
      debugDeclareLocal(alloca, expr.getName(), expr.getResolvedType(),
                        expr.getLocation());
    }
  }
  return resultPtr;
}

// -------------------------------------------------------------------
// Local variable creation
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// Local variable creation
// -------------------------------------------------------------------

llvm::Value* VariableGenerator::genLocalVar(const VariableCreationAST& expr,
                                            llvm::Type* varType) {
  // A reference variable binds the referent's address rather than reading
  // through it — that is what makes `var r = v.get(i); r = 5;` write into
  // the Vec. codegen() would read instead (see loadIfRef).
  sun::TypePtr declaredType = expr.getResolvedType();
  Value* value = declaredType && declaredType->isReference()
                     ? codegenBorrowAddress(*expr.getValue())
                     : nullptr;
  if (!value) value = codegen(*expr.getValue());
  if (!value) return nullptr;

  // Inside a function: use local alloca
  auto& scope = scopes().back().variables;
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  sun::TypePtr varSunType = expr.getResolvedType();

  // Payload enums: struct values handled by pointer. The variable OWNS its
  // storage: fresh temporaries are adopted, named sources are MOVED (never
  // implicitly copied), and the result is drop-tracked when payloads own
  // heap resources.
  if (varSunType && CodegenVisitor::isPayloadEnum(varSunType)) {
    auto& enumType = static_cast<sun::EnumType&>(*varSunType);
    llvm::StructType* storageTy = typeResolver.getEnumStorageType(enumType);

    // A fresh temporary (construction, materialized call return) can be
    // adopted directly; a named source (variable, field) is moved out of
    ASTNodeType valueKind = expr.getValue()->getType();
    bool valueIsFreshTemp = valueKind != ASTNodeType::VARIABLE_REFERENCE &&
                            valueKind != ASTNodeType::MEMBER_ACCESS;
    if (valueIsFreshTemp) {
      if (auto* allocaValue = dyn_cast<AllocaInst>(value)) {
        allocaValue->setName(expr.getName());
        scope[expr.getName()] = allocaValue;
        debugDeclareLocal(allocaValue, expr.getName(), varSunType,
                          expr.getLocation());
        scopes().trackClassAllocation(allocaValue, expr.getName(), varSunType);
        return allocaValue;
      }
    }

    AllocaInst* alloca =
        createEntryBlockAlloca(func, expr.getName(), storageTy);
    Value* structVal = value;
    if (value->getType()->isPointerTy()) {
      // Move: load the storage and poison the source tag (the new variable
      // owns the payload now)
      structVal = gen_.applyMoveSemantics(value, varSunType);
    }
    ctx.builder->CreateStore(structVal, alloca);
    scope[expr.getName()] = alloca;
    debugDeclareLocal(alloca, expr.getName(), varSunType, expr.getLocation());
    scopes().trackClassAllocation(alloca, expr.getName(), varSunType);
    return alloca;
  }

  // Handle interface types
  if (auto* ifaceType = sun::tryGetType<sun::InterfaceType>(varSunType)) {
    // Unwrap reference if needed
    sun::TypePtr valueSunType =
        sun::unwrapRef(expr.getValue()->getResolvedType());

    // A concrete value is moved into stable storage owned by the interface.
    if (auto* classType = sun::tryGetType<sun::ClassType>(valueSunType)) {
      Value* fatPtr = classes().createOwnedInterfaceFatPointer(
          value, classType, ifaceType);
      if (!fatPtr) return nullptr;

      AllocaInst* alloca =
          createEntryBlockAlloca(func, expr.getName(), fatPtr->getType());
      ctx.builder->CreateStore(fatPtr, alloca);
      scope[expr.getName()] = alloca;
      debugDeclareLocal(alloca, expr.getName(), varSunType, expr.getLocation());
      scopes().trackClassAllocation(alloca, expr.getName(), varSunType);
      return fatPtr;
    }

    // An interface source transfers its existing erased owner.
    if (valueSunType && valueSunType->isInterface()) {
      llvm::StructType* fatPtrType =
          sun::InterfaceType::getFatPointerType(ctx.getContext());
      Value* fatPtrVal = value;
      if (value->getType()->isPointerTy()) {
        fatPtrVal = gen_.applyMoveSemantics(value, valueSunType);
      }

      AllocaInst* alloca =
          createEntryBlockAlloca(func, expr.getName(), fatPtrType);
      ctx.builder->CreateStore(fatPtrVal, alloca);
      scope[expr.getName()] = alloca;
      debugDeclareLocal(alloca, expr.getName(), varSunType, expr.getLocation());
      scopes().trackClassAllocation(alloca, expr.getName(), varSunType);
      return fatPtrVal;
    }
  }

  // For array and class types, use the alloca directly
  // instead of creating a new alloca and storing a pointer
  if (varType->isArrayTy() || (varSunType && varSunType->isClass())) {
    if (auto* allocaValue = dyn_cast<AllocaInst>(value)) {
      allocaValue->setName(expr.getName());
      scope[expr.getName()] = allocaValue;
      debugDeclareLocal(allocaValue, expr.getName(), varSunType,
                        expr.getLocation());

      // Track class allocation for automatic deinit at scope exit
      if (auto classType = sun::tryGetTypePtr<sun::ClassType>(varSunType)) {
        scopes().trackClassAllocation(allocaValue, expr.getName(), classType);
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
      debugDeclareLocal(alloca, expr.getName(), varSunType, expr.getLocation());

      // Track class allocation for automatic deinit at scope exit
      if (auto classType = sun::tryGetTypePtr<sun::ClassType>(varSunType)) {
        scopes().trackClassAllocation(alloca, expr.getName(), classType);
      }
      return alloca;
    }

    // For class types, if value is a pointer to struct (from member access),
    // implement MOVE SEMANTICS: load the struct, zero out the source field,
    // and track the destination for deinit. This prevents double-free.
    if (value->getType()->isPointerTy()) {
      if (auto classType = sun::tryGetTypePtr<sun::ClassType>(varSunType)) {
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
        debugDeclareLocal(alloca, expr.getName(), varSunType,
                          expr.getLocation());

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
        scopes().trackClassAllocation(alloca, expr.getName(), classType);
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
        value = ops::extendInt(
            *ctx.builder, value, varType,
            expr.getValue() ? expr.getValue()->getResolvedType() : nullptr);
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
  debugDeclareLocal(alloca, expr.getName(), varSunType, expr.getLocation());

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
    case ASTNodeType::CHAR_LITERAL: {
      const auto& lit = static_cast<const CharLiteralAST&>(*elemExpr);
      auto elemType = elemExpr->getResolvedType();
      if (!elemType) return nullptr;
      return llvm::ConstantInt::get(elemType->toLLVMType(llvmCtx),
                                    lit.getValue(), /*isSigned=*/false);
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

llvm::Constant* VariableGenerator::genGlobalArray(
    const VariableCreationAST& expr) {
  assert(scopes().empty() &&
         "genGlobalArray should only be called at top-level");

  auto* arrayType = &sun::requireType<sun::ArrayType>(
      expr, "global array '" + expr.getName() + "'");
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
  std::string varName = expr.getMangledName();
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

llvm::Constant* VariableGenerator::genGlobalVarForConstantExpr(
    const VariableCreationAST& expr, llvm::Type* varType) {
  assert(scopes().empty() &&
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
  std::string varName = expr.getMangledName();
  createGlobalVariable(varName, varType, constValue);
  return constValue;
}

// -------------------------------------------------------------------
// Reference creation codegen
// -------------------------------------------------------------------

Value* VariableGenerator::codegen(const ReferenceCreationAST& expr) {
  sun::TypePtr refSunType = expr.getResolvedType();
  sun::requireType<sun::ReferenceType>(expr, "reference creation");

  // A reference stores a pointer to the target's storage. Any addressable
  // lvalue qualifies: variables, fields (obj.f), and array elements (arr[i]).
  // For ref-typed targets codegenAddress flattens to the referent's address,
  // so rebinding aliases the original storage.
  llvm::Value* targetPtr = codegenBorrowAddress(*expr.getTarget());
  if (!targetPtr) {
    logAndThrowError(
        "Reference target is not addressable (expected a variable, field, or "
        "array element)",
        expr.getLocation());
  }

  // Create an alloca that holds a pointer to the target
  std::string refName = expr.getMangledName();
  llvm::Type* ptrType = llvm::PointerType::getUnqual(ctx.getContext());
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* refAlloca = createEntryBlockAlloca(func, refName, ptrType);
  ctx.builder->CreateStore(targetPtr, refAlloca);

  if (!scopes().empty()) {
    scopes().back().variables[refName] = refAlloca;
    debugDeclareLocal(refAlloca, refName, refSunType, expr.getLocation());
  }
  return refAlloca;
}

// -------------------------------------------------------------------
// Global class variable creation
// -------------------------------------------------------------------

GlobalVariable* VariableGenerator::genGlobalClassVar(
    const VariableCreationAST& expr, sun::ClassType& classType) {
  assert(scopes().empty() &&
         "genGlobalClassVar should only be called at top-level");

  // Get the LLVM struct type for the class
  llvm::StructType* structType = classType.getStructType(ctx.getContext());

  // Create zero-initialized global variable for the class instance
  std::string varName = expr.getMangledName();
  llvm::Constant* zeroInit = llvm::ConstantAggregateZero::get(structType);
  GlobalVariable* gv = new GlobalVariable(
      *module, structType,
      /*isConstant=*/false, GlobalValue::ExternalLinkage, zeroInit, varName);

  // Queue for runtime initialization
  StaticInitInfo info;
  info.globalVar = gv;
  info.varName = expr.getName();
  info.varType = expr.getResolvedType();
  info.classType = sun::tryGetTypePtr<sun::ClassType>(expr);
  info.initExpr = expr.getValue();
  info.location = expr.getLocation();
  staticInits.push_back(std::move(info));

  return gv;
}

// -------------------------------------------------------------------
// Global variable with runtime initialization (non-class)
// -------------------------------------------------------------------

GlobalVariable* VariableGenerator::genGlobalVarWithRuntimeInit(
    const VariableCreationAST& expr, llvm::Type* varType) {
  assert(scopes().empty() &&
         "genGlobalVarWithRuntimeInit should only be called at top-level");

  // Create zero-initialized global variable
  std::string varName = expr.getMangledName();
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
  info.location = expr.getLocation();
  staticInits.push_back(std::move(info));

  return gv;
}

// -------------------------------------------------------------------
// Emit static initialization function
// -------------------------------------------------------------------

// The initializer of a global class variable constructs it in place when it
// names the class: `Class(args)` is a call whose callee resolves to the class,
// and `Class<T>(args)` a generic call that resolves to it. Intrinsics and
// generic functions merely return a class, so they are not constructions.
// Returns the constructor arguments, or null when the initializer is anything
// else.
static const std::vector<std::unique_ptr<ExprAST>>* constructorArgsForGlobal(
    const ExprAST& initExpr) {
  if (initExpr.getType() == ASTNodeType::CALL) {
    const auto& call = static_cast<const CallExprAST&>(initExpr);
    if (sun::tryGetType<sun::ClassType>(*call.getCallee()))
      return &call.getArgs();
    return nullptr;
  }
  if (initExpr.getType() == ASTNodeType::GENERIC_CALL) {
    const auto& call = static_cast<const GenericCallAST&>(initExpr);
    if (call.getGenericFunctionAST()) return nullptr;
    if (sun::isIntrinsic(call.getFunctionName())) return nullptr;
    if (sun::tryGetType<sun::ClassType>(call)) return &call.getArgs();
  }
  return nullptr;
}

void VariableGenerator::emitStaticInitFunction() {
  if (staticInits.empty()) return;

  // Create the initialization function: void __sun_static_init()
  // Internal linkage, on purpose: a .moon bundle carries its own copy of this
  // function, and linking it into a program that also has one must keep both.
  // With internal linkage the IR linker renames one instead of silently
  // replacing the other; llvm.global_ctors (below) is the shared merge point
  // that runs every copy.
  FunctionType* initFuncType =
      FunctionType::get(Type::getVoidTy(ctx.getContext()), false);
  Function* initFunc = Function::Create(initFuncType, Function::InternalLinkage,
                                        "__sun_static_init", module);

  BasicBlock* entryBB = BasicBlock::Create(ctx.getContext(), "entry", initFunc);
  ctx.builder->SetInsertPoint(entryBB);
  // No subprogram on the init function: it must carry no debug locations
  debugInfo.clearLocation(*ctx.builder);

  // Push a scope for any temporaries needed during init
  scopes().push().isFunctionBoundary = true;

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

      // A struct literal names its fields, so store them straight into the
      // global rather than looking for a constructor.
      if (init.initExpr->getType() == ASTNodeType::STRUCT_LITERAL) {
        const auto& literal =
            *static_cast<const StructLiteralAST*>(init.initExpr);
        for (const auto& field : literal.getFields()) {
          const sun::ClassField* classField = classType->getField(field.name);
          if (!classField) continue;  // rejected in semantic analysis

          Value* value = codegen(*field.value);
          if (!value) {
            logAndThrowError("Failed to generate field '" + field.name +
                             "' for global: " + init.varName);
          }
          value = ops::widenNumericIfNeeded(*ctx.builder, typeResolver, value,
                                            classField->type,
                                            field.value->getResolvedType());

          Value* fieldPtr = ctx.builder->CreateStructGEP(
              structType, gv, classField->index, field.name + ".ptr");
          layout::storeIntoSlot(*ctx.builder, module->getDataLayout(), fieldPtr,
                                value, classField->type, classType);
        }
        continue;
      }

      // `Class(args)` and `Class<T>(args)` construct the global in place;
      // anything else produces a value elsewhere and moves it in.
      const std::vector<std::unique_ptr<ExprAST>>* ctorArgs =
          constructorArgsForGlobal(*init.initExpr);
      if (!ctorArgs) {
        Value* value = codegen(*init.initExpr);
        if (!value) {
          logAndThrowError(
              "Failed to generate initializer for global variable: " +
              init.varName);
        }
        // Moving, not copying: the source is invalidated so only the global
        // drops the value.
        ctx.builder->CreateStore(gen_.applyMoveSemantics(value, init.varType),
                                 gv);
        continue;
      }

      // Look up constructor with overload resolution
      std::vector<sun::TypePtr> argTypes;
      argTypes.reserve(ctorArgs->size());
      for (const auto& arg : *ctorArgs) {
        argTypes.push_back(arg->getResolvedType());
      }
      ClassGenerator::ConstructorLookup ctor =
          classes().lookupConstructor(classType, argTypes);

      // Find the constructor; declare an external if the init method exists
      // but isn't in the module yet
      Function* ctorFunc =
          ctor.method ? functions().getOrDeclareMethodFunction(
                            ctor.mangledName, ctor.method->paramTypes,
                            ctor.method->returnType, ctor.method->canThrow)
                      : module->getFunction(ctor.mangledName);

      // A class with no constructor at all is fully described by the zeroed
      // storage; anything else must reach its constructor, so a lookup that
      // comes up empty is a miscompile rather than a silent no-op.
      if (!ctorFunc || ctorFunc->arg_size() != ctorArgs->size() + 1) {
        if (!ctorArgs->empty() || ctorFunc) {
          logAndThrowError("No constructor to initialize global variable '" +
                               init.varName + "' of type " +
                               classType->getDisplayName(),
                           init.location);
        }
        continue;
      }

      const auto& paramTypes =
          ctor.method ? ctor.method->paramTypes : std::vector<sun::TypePtr>{};

      std::vector<Value*> ctorArgValues;
      // Method closure; the receiver is the global variable
      ctorArgValues.push_back(
          gen_.materializeMethodClosure(ctorFunc, gv, "init.closure"));

      size_t argIdx = 0;
      for (const auto& arg : *ctorArgs) {
        sun::TypePtr paramType =
            argIdx < paramTypes.size() ? paramTypes[argIdx] : nullptr;
        bool isRefParam = paramType && paramType->isReference();

        Value* argVal = codegen(*arg);
        if (!argVal) {
          logAndThrowError(
              "Failed to generate argument for global class constructor: " +
              init.varName);
        }

        if (isRefParam) {
          // Reference parameters take the argument's address
          if (!argVal->getType()->isPointerTy()) {
            AllocaInst* tempAlloca =
                createEntryBlockAlloca(initFunc, "ref.temp", argVal->getType());
            ctx.builder->CreateStore(argVal, tempAlloca);
            argVal = tempAlloca;
          }
        } else {
          // By-value compound arguments move into the constructor
          argVal = gen_.applyMoveSemantics(argVal, arg->getResolvedType());
          argVal = ops::widenNumericIfNeeded(*ctx.builder, typeResolver, argVal,
                                             paramType, arg->getResolvedType());
        }

        ctorArgValues.push_back(argVal);
        ++argIdx;
      }

      ctx.builder->CreateCall(ctorFunc, ctorArgValues);
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

  scopes().pop();
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
