// src/codegen/arrays.cpp - Array type codegen
//
// Arrays are represented as fat structs: { ptr data, i32 ndims, ptr dims }
// This allows arrays of any shape to be passed uniformly via ref array<T>
//
// This file contains codegen for:
// - Array literals: [1, 2, 3] or [[1, 2], [3, 4]]
// - Array indexing: arr[i] or arr[i, j] for n-dimensional arrays

#include "codegen_visitor.h"
#include "error.h"

using namespace llvm;

// -------------------------------------------------------------------
// Array literal codegen: [1, 2, 3] or [[1, 2], [3, 4]]
// Creates a fat struct with stack-allocated data and dims storage
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const ArrayLiteralAST& expr) {
  const auto& elements = expr.getElements();

  if (elements.empty()) {
    logAndThrowError("Array literal cannot be empty");
    return nullptr;
  }

  // Get the resolved type from semantic analysis
  auto arrayType = expr.getResolvedType();
  if (!arrayType || !arrayType->isArray()) {
    logAndThrowError("Array literal must have resolved array type");
    return nullptr;
  }

  auto* sunArrayType = static_cast<sun::ArrayType*>(arrayType.get());
  const auto& dims = sunArrayType->getDimensions();

  if (dims.empty()) {
    logAndThrowError("Cannot create literal for unsized array");
    return nullptr;
  }

  Function* func = ctx.builder->GetInsertBlock()->getParent();

  // Get the raw data storage type (e.g., [3 x [2 x i32]])
  llvm::Type* dataStorageType =
      sunArrayType->getDataStorageType(ctx.getContext());
  if (!dataStorageType) {
    logAndThrowError("Cannot determine data storage type for array");
    return nullptr;
  }

  // Allocate raw data storage on stack
  AllocaInst* dataAlloca =
      createEntryBlockAlloca(func, "arr.data", dataStorageType);

  // For multidimensional arrays, the "slot type" at each outer index
  llvm::Type* slotType;
  if (dims.size() == 1) {
    slotType = sunArrayType->getElementType()->toLLVMType(ctx.getContext());
  } else {
    slotType = sunArrayType->getElementType()->toLLVMType(ctx.getContext());
    for (auto it = dims.rbegin(); it != std::prev(dims.rend()); ++it) {
      slotType = llvm::ArrayType::get(slotType, *it);
    }
  }

  // Store each element into data storage
  for (size_t i = 0; i < elements.size(); ++i) {
    Value* elemVal = codegen(*elements[i]);
    if (!elemVal) return nullptr;

    // GEP to get pointer to element i in data storage
    Value* elemPtr = ctx.builder->CreateGEP(
        dataStorageType, dataAlloca,
        {ctx.builder->getInt64(0), ctx.builder->getInt64(i)}, "arr.elem.ptr");

    // For nested arrays, extract data ptr from the nested fat struct and copy
    if (elements[i]->getType() == ASTNodeType::ARRAY_LITERAL) {
      // elemVal is a fat struct - extract data ptr and copy the data
      Value* nestedDataPtr =
          ctx.builder->CreateExtractValue(elemVal, 0, "nested.data");

      const llvm::DataLayout& DL = module->getDataLayout();
      uint64_t size = DL.getTypeAllocSize(slotType);
      ctx.builder->CreateMemCpy(elemPtr, MaybeAlign(8), nestedDataPtr,
                                MaybeAlign(8), size);
    } else {
      // Simple scalar element - ensure type matches
      llvm::Type* valType = elemVal->getType();
      if (valType != slotType) {
        if (valType->isIntegerTy() && slotType->isIntegerTy()) {
          unsigned valBits = valType->getIntegerBitWidth();
          unsigned slotBits = slotType->getIntegerBitWidth();
          if (valBits < slotBits) {
            elemVal = ctx.builder->CreateSExt(elemVal, slotType, "widen");
          } else if (valBits > slotBits) {
            elemVal = ctx.builder->CreateTrunc(elemVal, slotType, "trunc");
          }
        } else if (valType->isFloatTy() && slotType->isDoubleTy()) {
          elemVal = ctx.builder->CreateFPExt(elemVal, slotType, "fpext");
        } else if (valType->isDoubleTy() && slotType->isFloatTy()) {
          elemVal = ctx.builder->CreateFPTrunc(elemVal, slotType, "fptrunc");
        }
      }
      ctx.builder->CreateStore(elemVal, elemPtr);
    }
  }

  // Allocate dims array on stack: [ndims x i64]
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  llvm::ArrayType* dimsArrayType = llvm::ArrayType::get(i64Ty, dims.size());
  AllocaInst* dimsAlloca =
      createEntryBlockAlloca(func, "arr.dims", dimsArrayType);

  // Store dimension values
  for (size_t i = 0; i < dims.size(); ++i) {
    Value* dimPtr = ctx.builder->CreateGEP(
        dimsArrayType, dimsAlloca,
        {ctx.builder->getInt64(0), ctx.builder->getInt64(i)}, "dim.ptr");
    ctx.builder->CreateStore(ctx.builder->getInt64(dims[i]), dimPtr);
  }

  // Get pointer to first element of data and dims
  Value* dataPtr = ctx.builder->CreateGEP(
      dataStorageType, dataAlloca,
      {ctx.builder->getInt64(0), ctx.builder->getInt64(0)}, "data.ptr");
  Value* dimsPtr = ctx.builder->CreateGEP(
      dimsArrayType, dimsAlloca,
      {ctx.builder->getInt64(0), ctx.builder->getInt64(0)}, "dims.ptr");

  // Build the fat struct: { ptr data, i32 ndims, ptr dims }
  llvm::StructType* fatType =
      sun::ArrayType::getArrayStructType(ctx.getContext());
  Value* fatStruct = UndefValue::get(fatType);
  fatStruct = ctx.builder->CreateInsertValue(fatStruct, dataPtr, 0, "fat.data");
  fatStruct = ctx.builder->CreateInsertValue(
      fatStruct, ctx.builder->getInt32(dims.size()), 1, "fat.ndims");
  fatStruct = ctx.builder->CreateInsertValue(fatStruct, dimsPtr, 2, "fat.dims");

  return fatStruct;
}

// -------------------------------------------------------------------
// Array index codegen: arr[i] or arr[i, j, k]
// Also handles class indexing via __index__ and __slice__ methods
// Extracts data and dims from fat struct, computes linear offset
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const IndexAST& expr) {
  // Get the target value (array fat struct, class instance pointer, etc.)
  Value* targetVal = codegen(*expr.getTarget());
  if (!targetVal) return nullptr;

  // Get the target type - unwrap reference if needed
  auto exprType = expr.getTarget()->getResolvedType();
  sun::TypePtr targetType = exprType;

  // If this is a reference, we need to handle it appropriately
  if (exprType && exprType->isReference()) {
    auto* refType = static_cast<const sun::ReferenceType*>(exprType.get());
    targetType = refType->getReferencedType();
  }

  // Check if target is a class with __index__ or __slice__ method
  if (targetType && targetType->isClass()) {
    auto* classType = static_cast<sun::ClassType*>(targetType.get());
    bool hasSlices = expr.hasSlices();
    const auto& indices = expr.getIndices();

    if (hasSlices) {
      // Call __slice__ method with array of SliceRange
      return codegenClassSlice(expr, targetVal, classType);
    } else {
      // Call __index__ method with array of indices
      return codegenClassIndex(expr, targetVal, classType);
    }
  }

  // Handle array reference - load the fat struct from the pointer
  if (exprType && exprType->isReference()) {
    llvm::StructType* fatType =
        sun::ArrayType::getArrayStructType(ctx.getContext());
    targetVal = ctx.builder->CreateLoad(fatType, targetVal, "arr.fat.load");
  }

  if (!targetType || !targetType->isArray()) {
    logAndThrowError("Cannot index non-array type");
    return nullptr;
  }

  auto* sunArrayType = static_cast<sun::ArrayType*>(targetType.get());
  const auto& indices = expr.getIndices();

  // Extract components from fat struct: { ptr data, i32 ndims, ptr dims }
  Value* dataPtr =
      ctx.builder->CreateExtractValue(targetVal, 0, "arr.data.ptr");
  Value* dimsPtr =
      ctx.builder->CreateExtractValue(targetVal, 2, "arr.dims.ptr");

  sun::TypePtr elemSunType = sunArrayType->getElementType();
  llvm::Type* elemType = elemSunType->toLLVMType(ctx.getContext());
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());

  // Compute linear offset using row-major order
  // For arr[i, j, k] with dims [D0, D1, D2]:
  // offset = i * (D1 * D2) + j * D2 + k
  Value* offset = ctx.builder->getInt64(0);

  for (size_t i = 0; i < indices.size(); ++i) {
    const auto& sliceExpr = *indices[i];

    // For now, only handle single indices (not range slices)
    if (sliceExpr.isRange()) {
      logAndThrowError(
          "Array slicing (a:b) not yet implemented - use single indices");
      return nullptr;
    }

    if (!sliceExpr.hasStart()) {
      logAndThrowError("Index expression is empty");
      return nullptr;
    }

    Value* idx = codegen(*sliceExpr.getStart());
    if (!idx) return nullptr;

    if (idx->getType()->isIntegerTy() &&
        idx->getType()->getIntegerBitWidth() < 64) {
      idx = ctx.builder->CreateSExt(idx, i64Ty, "idx.ext");
    }

    // Compute stride for this index: product of dims[i+1..n-1]
    Value* stride = ctx.builder->getInt64(1);
    for (size_t j = i + 1; j < indices.size(); ++j) {
      Value* dimJPtr = ctx.builder->CreateGEP(
          i64Ty, dimsPtr, ctx.builder->getInt64(j), "dim.j.ptr");
      Value* dimJ = ctx.builder->CreateLoad(i64Ty, dimJPtr, "dim.j");
      stride = ctx.builder->CreateMul(stride, dimJ, "stride.mul");
    }

    // offset += idx * stride
    Value* term = ctx.builder->CreateMul(idx, stride, "idx.stride");
    offset = ctx.builder->CreateAdd(offset, term, "offset.add");
  }

  // GEP to the element using linear offset
  Value* elemPtr =
      ctx.builder->CreateGEP(elemType, dataPtr, offset, "arr.elem.ptr");

  return ctx.builder->CreateLoad(elemType, elemPtr, "arr.elem");
}

// -------------------------------------------------------------------
// Array indexed assignment support
// Returns a pointer to the array element for assignment
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenIndexElementPtr(const IndexAST& expr) {
  // Get the array value
  Value* arrayVal = codegen(*expr.getTarget());
  if (!arrayVal) return nullptr;

  // Get the array type - unwrap reference if needed
  auto exprType = expr.getTarget()->getResolvedType();
  sun::TypePtr arrayType = exprType;

  if (exprType && exprType->isReference()) {
    auto* refType = static_cast<const sun::ReferenceType*>(exprType.get());
    arrayType = refType->getReferencedType();
    llvm::StructType* fatType =
        sun::ArrayType::getArrayStructType(ctx.getContext());
    arrayVal = ctx.builder->CreateLoad(fatType, arrayVal, "arr.fat.load");
  }

  if (!arrayType || !arrayType->isArray()) {
    logAndThrowError("Cannot index non-array type");
    return nullptr;
  }

  auto* sunArrayType = static_cast<sun::ArrayType*>(arrayType.get());
  const auto& indices = expr.getIndices();

  // Extract from fat struct
  Value* dataPtr = ctx.builder->CreateExtractValue(arrayVal, 0, "arr.data.ptr");
  Value* dimsPtr = ctx.builder->CreateExtractValue(arrayVal, 2, "arr.dims.ptr");

  llvm::Type* elemType =
      sunArrayType->getElementType()->toLLVMType(ctx.getContext());
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());

  // Compute linear offset
  Value* offset = ctx.builder->getInt64(0);

  for (size_t i = 0; i < indices.size(); ++i) {
    const auto& sliceExpr = *indices[i];

    // For now, only handle single indices (not range slices)
    if (sliceExpr.isRange()) {
      logAndThrowError(
          "Array slicing (a:b) not yet implemented for assignment");
      return nullptr;
    }

    if (!sliceExpr.hasStart()) {
      logAndThrowError("Index expression is empty");
      return nullptr;
    }

    Value* idx = codegen(*sliceExpr.getStart());
    if (!idx) return nullptr;

    if (idx->getType()->isIntegerTy() &&
        idx->getType()->getIntegerBitWidth() < 64) {
      idx = ctx.builder->CreateSExt(idx, i64Ty, "idx.ext");
    }

    Value* stride = ctx.builder->getInt64(1);
    for (size_t j = i + 1; j < indices.size(); ++j) {
      Value* dimJPtr = ctx.builder->CreateGEP(
          i64Ty, dimsPtr, ctx.builder->getInt64(j), "dim.j.ptr");
      Value* dimJ = ctx.builder->CreateLoad(i64Ty, dimJPtr, "dim.j");
      stride = ctx.builder->CreateMul(stride, dimJ, "stride.mul");
    }

    Value* term = ctx.builder->CreateMul(idx, stride, "idx.stride");
    offset = ctx.builder->CreateAdd(offset, term, "offset.add");
  }

  return ctx.builder->CreateGEP(elemType, dataPtr, offset, "arr.elem.ptr");
}

// -------------------------------------------------------------------
// Indexed assignment: arr[i, j] = value or obj[i, j] = value
// For arrays: gets element pointer and stores value
// For classes: calls __setindex__ method
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const IndexedAssignmentAST& expr) {
  // The target should be an IndexAST
  const ExprAST* indexTarget = expr.getTarget();
  if (indexTarget->getType() != ASTNodeType::INDEX) {
    logAndThrowError("Indexed assignment target must be an index expression");
    return nullptr;
  }

  const auto& indexExpr = static_cast<const IndexAST&>(*indexTarget);

  // Check if target is a class with __setindex__ method
  auto targetExprType = indexExpr.getTarget()->getResolvedType();
  sun::TypePtr targetType = targetExprType;

  if (targetExprType && targetExprType->isReference()) {
    auto* refType =
        static_cast<const sun::ReferenceType*>(targetExprType.get());
    targetType = refType->getReferencedType();
  }

  if (targetType && targetType->isClass()) {
    auto* classType = static_cast<sun::ClassType*>(targetType.get());

    // Look for __setindex__ method
    const sun::ClassMethod* method = classType->getMethod("__setindex__");
    if (method) {
      return codegenClassSetIndex(indexExpr, expr.getValue(), classType);
    }
    // If no __setindex__, fall through to error (classes need explicit method)
    logAndThrowError("Class " + classType->getName() +
                     " does not implement __setindex__ for indexed assignment");
    return nullptr;
  }

  // Array indexed assignment
  // Get address of the array element
  Value* elemPtr = codegenIndexElementPtr(indexExpr);
  if (!elemPtr) return nullptr;

  // Generate the value to assign
  Value* value = codegen(*expr.getValue());
  if (!value) return nullptr;

  // Store the value
  ctx.builder->CreateStore(value, elemPtr);

  return value;
}
// -------------------------------------------------------------------
// Array shape: arr.shape() returns 1D array of dimension sizes
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenArrayShape(const MemberAccessAST& expr) {
  // Get the array value
  Value* arrayVal = codegen(*expr.getObject());
  if (!arrayVal) return nullptr;

  // Get the array type - unwrap reference if needed
  sun::TypePtr objectType = expr.getObject()->getResolvedType();
  sun::TypePtr arrayType = objectType;

  if (objectType && objectType->isReference()) {
    auto* refType = static_cast<sun::ReferenceType*>(objectType.get());
    arrayType = refType->getReferencedType();
    // Load the fat struct from the reference
    llvm::StructType* fatType =
        sun::ArrayType::getArrayStructType(ctx.getContext());
    arrayVal = ctx.builder->CreateLoad(fatType, arrayVal, "arr.fat.load");
  }

  if (!arrayType || !arrayType->isArray()) {
    logAndThrowError("Cannot get shape of non-array type");
    return nullptr;
  }

  auto* sunArrayType = static_cast<sun::ArrayType*>(arrayType.get());
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());

  // Extract ndims and dims pointer from fat struct
  Value* ndimsVal = ctx.builder->CreateExtractValue(arrayVal, 1, "shape.ndims");
  Value* dimsPtr =
      ctx.builder->CreateExtractValue(arrayVal, 2, "shape.dims.ptr");

  // For sized arrays, use compile-time dimensions; for unsized, use runtime
  // ndims
  size_t staticNdims = sunArrayType->getDimensions().size();
  bool isUnsized = sunArrayType->isUnsized();

  if (isUnsized) {
    // Runtime ndims - for now, limit to max 8 dimensions and use a fixed alloca
    // The actual size is determined by ndimsVal at runtime
    size_t maxDims = 8;
    llvm::ArrayType* shapeDataType = llvm::ArrayType::get(i64Ty, maxDims);
    Value* shapeData =
        ctx.builder->CreateAlloca(shapeDataType, nullptr, "shape.data");

    // Copy dimension values in a loop (compile-time unrolled for small arrays)
    // For simplicity, just copy up to maxDims
    for (size_t i = 0; i < maxDims; ++i) {
      // Create conditional copy based on i < ndims
      Value* iVal = ctx.builder->getInt32(i);
      Value* shouldCopy =
          ctx.builder->CreateICmpULT(iVal, ndimsVal, "should.copy");

      // Create basic blocks for conditional
      Function* func = ctx.builder->GetInsertBlock()->getParent();
      BasicBlock* copyBB =
          BasicBlock::Create(ctx.getContext(), "copy.dim", func);
      BasicBlock* mergeBB =
          BasicBlock::Create(ctx.getContext(), "merge.dim", func);

      ctx.builder->CreateCondBr(shouldCopy, copyBB, mergeBB);

      ctx.builder->SetInsertPoint(copyBB);
      Value* srcPtr = ctx.builder->CreateGEP(
          i64Ty, dimsPtr, ctx.builder->getInt64(i), "dim.src.ptr");
      Value* dimVal = ctx.builder->CreateLoad(i64Ty, srcPtr, "dim.val");
      Value* dstPtr = ctx.builder->CreateGEP(
          i64Ty, shapeData, ctx.builder->getInt64(i), "dim.dst.ptr");
      ctx.builder->CreateStore(dimVal, dstPtr);
      ctx.builder->CreateBr(mergeBB);

      ctx.builder->SetInsertPoint(mergeBB);
    }

    // Create dims array for the shape result (1D array with runtime ndims)
    llvm::ArrayType* shapeDimsType = llvm::ArrayType::get(i64Ty, 1);
    Value* shapeDims =
        ctx.builder->CreateAlloca(shapeDimsType, nullptr, "shape.dims");
    Value* ndimsAsI64 = ctx.builder->CreateZExt(ndimsVal, i64Ty, "ndims.i64");
    ctx.builder->CreateStore(ndimsAsI64, shapeDims);

    // Build fat struct for shape array
    llvm::StructType* fatType =
        sun::ArrayType::getArrayStructType(ctx.getContext());
    Value* result = llvm::UndefValue::get(fatType);
    result =
        ctx.builder->CreateInsertValue(result, shapeData, 0, "shape.fat.data");
    result = ctx.builder->CreateInsertValue(result, ctx.builder->getInt32(1), 1,
                                            "shape.fat.ndims");
    result =
        ctx.builder->CreateInsertValue(result, shapeDims, 2, "shape.fat.dims");

    return result;
  } else {
    // Static ndims - original implementation
    llvm::ArrayType* shapeDataType = llvm::ArrayType::get(i64Ty, staticNdims);
    Value* shapeData =
        ctx.builder->CreateAlloca(shapeDataType, nullptr, "shape.data");

    // Copy dimension values
    for (size_t i = 0; i < staticNdims; ++i) {
      Value* srcPtr = ctx.builder->CreateGEP(
          i64Ty, dimsPtr, ctx.builder->getInt64(i), "dim.src.ptr");
      Value* dimVal = ctx.builder->CreateLoad(i64Ty, srcPtr, "dim.val");
      Value* dstPtr = ctx.builder->CreateGEP(
          i64Ty, shapeData, ctx.builder->getInt64(i), "dim.dst.ptr");
      ctx.builder->CreateStore(dimVal, dstPtr);
    }

    // Create dims array for the shape result (1D array, so just one dimension)
    llvm::ArrayType* shapeDimsType = llvm::ArrayType::get(i64Ty, 1);
    Value* shapeDims =
        ctx.builder->CreateAlloca(shapeDimsType, nullptr, "shape.dims");
    ctx.builder->CreateStore(ctx.builder->getInt64(staticNdims), shapeDims);

    // Build fat struct for shape array
    llvm::StructType* fatType =
        sun::ArrayType::getArrayStructType(ctx.getContext());
    Value* result = llvm::UndefValue::get(fatType);
    result =
        ctx.builder->CreateInsertValue(result, shapeData, 0, "shape.fat.data");
    result = ctx.builder->CreateInsertValue(result, ctx.builder->getInt32(1), 1,
                                            "shape.fat.ndims");
    result =
        ctx.builder->CreateInsertValue(result, shapeDims, 2, "shape.fat.dims");

    return result;
  }
}

// -------------------------------------------------------------------
// Class indexing via __index__ method: obj[i, j, k] -> obj.__index__([i,j,k])
// Synthesizes a call to the __index__ method with indices as array
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenClassIndex(const IndexAST& expr, Value* objectPtr,
                                         sun::ClassType* classType) {
  const auto& indices = expr.getIndices();
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());

  // Look up the __index__ method
  const sun::ClassMethod* method = classType->getMethod("__index__");
  if (!method) {
    logAndThrowError("Class " + classType->getName() +
                     " does not have __index__ method");
    return nullptr;
  }

  // Build array of indices on the stack
  // Create array data: [i64 x N]
  size_t numIndices = indices.size();
  llvm::ArrayType* arrDataType = llvm::ArrayType::get(i64Ty, numIndices);
  Value* arrData =
      ctx.builder->CreateAlloca(arrDataType, nullptr, "idx.arr.data");

  // Store each index value
  for (size_t i = 0; i < numIndices; ++i) {
    const auto& sliceExpr = *indices[i];
    if (!sliceExpr.hasStart()) {
      logAndThrowError("Index expression is empty");
      return nullptr;
    }

    Value* idxVal = codegen(*sliceExpr.getStart());
    if (!idxVal) return nullptr;

    // Extend to i64 if needed
    if (idxVal->getType()->isIntegerTy() &&
        idxVal->getType()->getIntegerBitWidth() < 64) {
      idxVal = ctx.builder->CreateSExt(idxVal, i64Ty, "idx.ext");
    }

    Value* elemPtr = ctx.builder->CreateGEP(
        i64Ty, arrData, ctx.builder->getInt64(i), "idx.elem.ptr");
    ctx.builder->CreateStore(idxVal, elemPtr);
  }

  // Create dims array (1D array with shape [numIndices])
  llvm::ArrayType* dimsType = llvm::ArrayType::get(i64Ty, 1);
  Value* dimsAlloca = ctx.builder->CreateAlloca(dimsType, nullptr, "idx.dims");
  ctx.builder->CreateStore(ctx.builder->getInt64(numIndices), dimsAlloca);

  // Build the fat struct for the array { ptr data, i32 ndims, ptr dims }
  llvm::StructType* fatType =
      sun::ArrayType::getArrayStructType(ctx.getContext());
  AllocaInst* arrAlloca =
      ctx.builder->CreateAlloca(fatType, nullptr, "idx.arr");

  Value* fatVal = llvm::UndefValue::get(fatType);
  fatVal = ctx.builder->CreateInsertValue(fatVal, arrData, 0);
  fatVal = ctx.builder->CreateInsertValue(fatVal, ctx.builder->getInt32(1), 1);
  fatVal = ctx.builder->CreateInsertValue(fatVal, dimsAlloca, 2);
  ctx.builder->CreateStore(fatVal, arrAlloca);

  // Get the method function
  std::string mangledName = classType->getMangledMethodName("__index__");
  Function* methodFunc = module->getFunction(mangledName);

  if (!methodFunc) {
    // Create function declaration on-demand
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // this ptr
    // The array parameter is passed by reference (pointer to fat struct)
    paramTypes.push_back(
        PointerType::getUnqual(ctx.getContext()));  // ref array<i64>

    llvm::Type* returnType;
    if (method->returnType && method->returnType->toString() != "void") {
      returnType = typeResolver.resolveForReturn(method->returnType);
    } else {
      returnType = Type::getVoidTy(ctx.getContext());
    }

    FunctionType* funcType = FunctionType::get(returnType, paramTypes, false);
    methodFunc = Function::Create(funcType, Function::ExternalLinkage,
                                  mangledName, module);
  }

  // Build arguments: this pointer, array reference
  std::vector<Value*> argValues;
  argValues.push_back(objectPtr);  // 'this' pointer
  argValues.push_back(arrAlloca);  // reference to array (pointer to fat struct)

  Value* result =
      ctx.builder->CreateCall(methodFunc, argValues, "index.result");

  // Handle struct return values (classes returned by value)
  if (result->getType()->isStructTy()) {
    auto* structType = cast<StructType>(result->getType());
    bool isErrorUnion = structType->getNumElements() == 2 &&
                        structType->getElementType(0)->isIntegerTy(1);
    bool isArrayFat = structType->getNumElements() == 3 &&
                      structType->getElementType(1)->isIntegerTy(32);
    if (!isErrorUnion && !isArrayFat) {
      Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
      AllocaInst* resultAlloca =
          createEntryBlockAlloca(currentFunc, "ret.struct", structType);
      ctx.builder->CreateStore(result, resultAlloca);
      result = resultAlloca;
    }
  }

  return result;
}

// -------------------------------------------------------------------
// Class slicing via __slice__ method: obj[a:b, :, c:] -> obj.__slice__(ranges)
// Synthesizes a call to the __slice__ method with SliceRange array
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenClassSlice(const IndexAST& expr, Value* objectPtr,
                                         sun::ClassType* classType) {
  const auto& indices = expr.getIndices();
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  llvm::Type* i1Ty = llvm::Type::getInt1Ty(ctx.getContext());

  // Look up the __slice__ method
  const sun::ClassMethod* method = classType->getMethod("__slice__");
  if (!method) {
    logAndThrowError("Class " + classType->getName() +
                     " does not have __slice__ method");
    return nullptr;
  }

  // SliceRange struct type matches Sun's definition: { i64 start, i64 end, i1
  // hasStart, i1 hasEnd } Use named struct type for compatibility with
  // SliceRange class methods
  llvm::StructType* sliceRangeType =
      llvm::StructType::getTypeByName(ctx.getContext(), "SliceRange_struct");
  if (!sliceRangeType) {
    // Fallback: create the type matching Sun's definition with i1 bools
    sliceRangeType = llvm::StructType::create(
        ctx.getContext(), {i64Ty, i64Ty, i1Ty, i1Ty}, "SliceRange_struct");
  }

  size_t numSlices = indices.size();

  // Allocate array of SliceRange structs
  llvm::ArrayType* rangesArrType =
      llvm::ArrayType::get(sliceRangeType, numSlices);
  Value* rangesData =
      ctx.builder->CreateAlloca(rangesArrType, nullptr, "slice.ranges.data");

  // Fill in each SliceRange
  for (size_t i = 0; i < numSlices; ++i) {
    const auto& sliceExpr = *indices[i];

    Value* startVal;
    Value* endVal;
    Value* hasStartVal;
    Value* hasEndVal;

    if (sliceExpr.isRange()) {
      // This is a range slice: a:b, a:, :b, or :
      hasStartVal = ctx.builder->getInt1(sliceExpr.hasStart());
      hasEndVal = ctx.builder->getInt1(sliceExpr.hasEnd());

      if (sliceExpr.hasStart()) {
        startVal = codegen(*sliceExpr.getStart());
        if (!startVal) return nullptr;
        if (startVal->getType()->isIntegerTy() &&
            startVal->getType()->getIntegerBitWidth() < 64) {
          startVal = ctx.builder->CreateSExt(startVal, i64Ty, "start.ext");
        }
      } else {
        startVal = ctx.builder->getInt64(0);
      }

      if (sliceExpr.hasEnd()) {
        endVal = codegen(*sliceExpr.getEnd());
        if (!endVal) return nullptr;
        if (endVal->getType()->isIntegerTy() &&
            endVal->getType()->getIntegerBitWidth() < 64) {
          endVal = ctx.builder->CreateSExt(endVal, i64Ty, "end.ext");
        }
      } else {
        endVal = ctx.builder->getInt64(0);
      }
    } else {
      // This is a single index - treat as a:a+1 range
      hasStartVal = ctx.builder->getInt1(true);
      hasEndVal = ctx.builder->getInt1(true);

      if (!sliceExpr.hasStart()) {
        logAndThrowError("Index expression is empty in slice context");
        return nullptr;
      }

      startVal = codegen(*sliceExpr.getStart());
      if (!startVal) return nullptr;
      if (startVal->getType()->isIntegerTy() &&
          startVal->getType()->getIntegerBitWidth() < 64) {
        startVal = ctx.builder->CreateSExt(startVal, i64Ty, "start.ext");
      }
      // end = start + 1 for single index in slice context
      endVal = ctx.builder->CreateAdd(startVal, ctx.builder->getInt64(1),
                                      "end.single");
    }

    // Store SliceRange fields
    Value* rangePtr = ctx.builder->CreateGEP(
        rangesArrType, rangesData,
        {ctx.builder->getInt64(0), ctx.builder->getInt64(i)}, "range.ptr");

    // Store start
    Value* startPtr = ctx.builder->CreateStructGEP(sliceRangeType, rangePtr, 0,
                                                   "range.start.ptr");
    ctx.builder->CreateStore(startVal, startPtr);

    // Store end
    Value* endPtr = ctx.builder->CreateStructGEP(sliceRangeType, rangePtr, 1,
                                                 "range.end.ptr");
    ctx.builder->CreateStore(endVal, endPtr);

    // Store hasStart
    Value* hasStartPtr = ctx.builder->CreateStructGEP(sliceRangeType, rangePtr,
                                                      2, "range.hasStart.ptr");
    ctx.builder->CreateStore(hasStartVal, hasStartPtr);

    // Store hasEnd
    Value* hasEndPtr = ctx.builder->CreateStructGEP(sliceRangeType, rangePtr, 3,
                                                    "range.hasEnd.ptr");
    ctx.builder->CreateStore(hasEndVal, hasEndPtr);
  }

  // Create dims array for the SliceRange array (1D: [numSlices])
  llvm::ArrayType* dimsType = llvm::ArrayType::get(i64Ty, 1);
  Value* dimsAlloca =
      ctx.builder->CreateAlloca(dimsType, nullptr, "slice.dims");
  ctx.builder->CreateStore(ctx.builder->getInt64(numSlices), dimsAlloca);

  // Build the fat struct for the array of SliceRanges
  llvm::StructType* fatType =
      sun::ArrayType::getArrayStructType(ctx.getContext());
  AllocaInst* arrAlloca =
      ctx.builder->CreateAlloca(fatType, nullptr, "slice.arr");

  Value* fatVal = llvm::UndefValue::get(fatType);
  fatVal = ctx.builder->CreateInsertValue(fatVal, rangesData, 0);
  fatVal = ctx.builder->CreateInsertValue(fatVal, ctx.builder->getInt32(1), 1);
  fatVal = ctx.builder->CreateInsertValue(fatVal, dimsAlloca, 2);
  ctx.builder->CreateStore(fatVal, arrAlloca);

  // Get the method function
  std::string mangledName = classType->getMangledMethodName("__slice__");
  Function* methodFunc = module->getFunction(mangledName);

  if (!methodFunc) {
    // Create function declaration on-demand
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // this ptr
    paramTypes.push_back(
        PointerType::getUnqual(ctx.getContext()));  // ref array<SliceRange>

    llvm::Type* returnType;
    if (method->returnType && method->returnType->toString() != "void") {
      returnType = typeResolver.resolveForReturn(*method->returnType);
    } else {
      returnType = Type::getVoidTy(ctx.getContext());
    }

    FunctionType* funcType = FunctionType::get(returnType, paramTypes, false);
    methodFunc = Function::Create(funcType, Function::ExternalLinkage,
                                  mangledName, module);
  }

  // Build arguments: this pointer, array reference
  std::vector<Value*> argValues;
  argValues.push_back(objectPtr);  // 'this' pointer
  argValues.push_back(arrAlloca);  // reference to array of SliceRanges

  Value* result =
      ctx.builder->CreateCall(methodFunc, argValues, "slice.result");

  // Handle struct return values (classes returned by value like MatrixView)
  if (result->getType()->isStructTy()) {
    auto* structType = cast<StructType>(result->getType());
    bool isErrorUnion = structType->getNumElements() == 2 &&
                        structType->getElementType(0)->isIntegerTy(1);
    bool isArrayFat = structType->getNumElements() == 3 &&
                      structType->getElementType(1)->isIntegerTy(32);
    if (!isErrorUnion && !isArrayFat) {
      Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
      AllocaInst* resultAlloca =
          createEntryBlockAlloca(currentFunc, "ret.struct", structType);
      ctx.builder->CreateStore(result, resultAlloca);
      result = resultAlloca;
    }
  }

  return result;
}

// -------------------------------------------------------------------
// Class indexed assignment via __setindex__ method: obj[i, j] = val
// Synthesizes a call to __setindex__(indices, value)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenClassSetIndex(const IndexAST& indexExpr,
                                            const ExprAST* valueExpr,
                                            sun::ClassType* classType) {
  // Get the object pointer
  Value* objectPtr = codegen(*indexExpr.getTarget());
  if (!objectPtr) return nullptr;

  const auto& indices = indexExpr.getIndices();
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());

  // Look up the __setindex__ method
  const sun::ClassMethod* method = classType->getMethod("__setindex__");
  if (!method) {
    logAndThrowError("Class " + classType->getName() +
                     " does not have __setindex__ method");
    return nullptr;
  }

  // Build array of indices on the stack
  size_t numIndices = indices.size();
  llvm::ArrayType* arrDataType = llvm::ArrayType::get(i64Ty, numIndices);
  Value* arrData =
      ctx.builder->CreateAlloca(arrDataType, nullptr, "setidx.arr.data");

  // Store each index value
  for (size_t i = 0; i < numIndices; ++i) {
    const auto& sliceExpr = *indices[i];
    if (sliceExpr.isRange()) {
      logAndThrowError("Cannot use range slices in indexed assignment");
      return nullptr;
    }
    if (!sliceExpr.hasStart()) {
      logAndThrowError("Index expression is empty");
      return nullptr;
    }

    Value* idxVal = codegen(*sliceExpr.getStart());
    if (!idxVal) return nullptr;

    if (idxVal->getType()->isIntegerTy() &&
        idxVal->getType()->getIntegerBitWidth() < 64) {
      idxVal = ctx.builder->CreateSExt(idxVal, i64Ty, "idx.ext");
    }

    Value* elemPtr = ctx.builder->CreateGEP(
        i64Ty, arrData, ctx.builder->getInt64(i), "idx.elem.ptr");
    ctx.builder->CreateStore(idxVal, elemPtr);
  }

  // Create dims array (1D: [numIndices])
  llvm::ArrayType* dimsType = llvm::ArrayType::get(i64Ty, 1);
  Value* dimsAlloca =
      ctx.builder->CreateAlloca(dimsType, nullptr, "setidx.dims");
  ctx.builder->CreateStore(ctx.builder->getInt64(numIndices), dimsAlloca);

  // Build fat struct for the array
  llvm::StructType* fatType =
      sun::ArrayType::getArrayStructType(ctx.getContext());
  AllocaInst* arrAlloca =
      ctx.builder->CreateAlloca(fatType, nullptr, "setidx.arr");

  Value* fatVal = llvm::UndefValue::get(fatType);
  fatVal = ctx.builder->CreateInsertValue(fatVal, arrData, 0);
  fatVal = ctx.builder->CreateInsertValue(fatVal, ctx.builder->getInt32(1), 1);
  fatVal = ctx.builder->CreateInsertValue(fatVal, dimsAlloca, 2);
  ctx.builder->CreateStore(fatVal, arrAlloca);

  // Generate the value to set
  Value* valueVal = codegen(*valueExpr);
  if (!valueVal) return nullptr;

  // Get the method function
  std::string mangledName = classType->getMangledMethodName("__setindex__");
  Function* methodFunc = module->getFunction(mangledName);

  if (!methodFunc) {
    // Create function declaration on-demand
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // this ptr
    paramTypes.push_back(
        PointerType::getUnqual(ctx.getContext()));  // ref array<i64>

    // Value type - get from method signature
    if (method->paramTypes.size() >= 2) {
      paramTypes.push_back(typeResolver.resolve(method->paramTypes[1]));
    } else {
      paramTypes.push_back(valueVal->getType());  // fallback
    }

    llvm::Type* returnType = Type::getVoidTy(ctx.getContext());

    FunctionType* funcType = FunctionType::get(returnType, paramTypes, false);
    methodFunc = Function::Create(funcType, Function::ExternalLinkage,
                                  mangledName, module);
  }

  // Build arguments: this pointer, array reference, value
  std::vector<Value*> argValues;
  argValues.push_back(objectPtr);  // 'this' pointer
  argValues.push_back(arrAlloca);  // reference to index array
  argValues.push_back(valueVal);   // value to set

  ctx.builder->CreateCall(methodFunc, argValues);

  return valueVal;
}