// src/codegen/arrays.cpp - Array type codegen
//
// A sized array<T, N> owns its elements inline: its LLVM type is [N x T] and,
// like a class, an expression of that type yields a POINTER to the storage.
// Copies are memcpys and moves are memcpys that then invalidate the source.
//
// An unsized array<T> exists only behind `ref`, as a view struct
// { ptr data, i32 ndims, ptr dims } built at the borrow site from a sized
// array. The view travels by value; its dims table is a private constant.
//
// This file contains codegen for:
// - Array literals: [1, 2, 3] or [[1, 2], [3, 4]]
// - Array indexing: arr[i] or arr[i, j] for n-dimensional arrays
// - ndims() and dim(i) on arrays and views
// - The __index__/__setindex__/__slice__ protocol on classes

#include "codegen/codegen_visitor.h"
#include "support/error.h"

using namespace llvm;

// -------------------------------------------------------------------
// Views: the fat struct a sized array decays to at a `ref array<T>` site
// -------------------------------------------------------------------

// The dimension table of a sized array, as a private constant global shared
// by every view of an array with those dimensions. A view may outlive the
// frame that built it (it can be stored in a ref field), so its table must
// live for the whole program.
Constant* CodegenVisitor::arrayDimsTable(const std::vector<size_t>& dims) {
  std::string name = "arr.dims";
  for (size_t dim : dims) name += "." + std::to_string(dim);
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  llvm::ArrayType* tableType = llvm::ArrayType::get(i64Ty, dims.size());
  if (GlobalVariable* existing = module->getGlobalVariable(name, true)) {
    return existing;
  }
  std::vector<Constant*> values;
  for (size_t dim : dims) values.push_back(ConstantInt::get(i64Ty, dim));
  auto* table = new GlobalVariable(*module, tableType, /*isConstant=*/true,
                                   GlobalValue::PrivateLinkage,
                                   ConstantArray::get(tableType, values), name);
  table->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  return table;
}

Value* CodegenVisitor::emitArrayView(Value* storagePtr,
                                     const std::vector<size_t>& dims) {
  StructType* fatType = sun::ArrayType::getArrayStructType(ctx.getContext());
  Value* view = UndefValue::get(fatType);
  view = ctx.builder->CreateInsertValue(view, storagePtr, 0, "view.data");
  view = ctx.builder->CreateInsertValue(
      view, ctx.builder->getInt32(static_cast<uint32_t>(dims.size())), 1,
      "view.ndims");
  view = ctx.builder->CreateInsertValue(view, arrayDimsTable(dims), 2,
                                        "view.dims");
  return view;
}

Value* CodegenVisitor::loadArrayView(Value* value) {
  if (!value) return nullptr;
  if (!value->getType()->isPointerTy()) return value;
  StructType* fatType = sun::ArrayType::getArrayStructType(ctx.getContext());
  return ctx.builder->CreateLoad(fatType, value, "view.load");
}

// -------------------------------------------------------------------
// Moving and copying inline storage
// -------------------------------------------------------------------

void CodegenVisitor::emitArrayTransfer(Value* dest, Value* src,
                                       const sun::ArrayType& type, bool move) {
  llvm::Type* storageType = type.getDataStorageType(ctx.getContext());
  const DataLayout& DL = module->getDataLayout();
  uint64_t size = DL.getTypeAllocSize(storageType);
  Align align = DL.getABITypeAlign(storageType);
  if (!src->getType()->isPointerTy()) {
    // An aggregate value (a by-value parameter's initial value): spill it
    // so the copy has an address to read from
    Function* func = ctx.builder->GetInsertBlock()->getParent();
    AllocaInst* temp = createEntryBlockAlloca(func, "arr.spill", storageType);
    ctx.builder->CreateStore(src, temp);
    src = temp;
    move = false;
  }
  if (dest == src) return;
  ctx.builder->CreateMemCpy(dest, align, src, align, size);
  if (!move) return;
  // The destination owns the elements now. The source's own drop must
  // release nothing, and an all-zero element is exactly that.
  scopes.markClassAllocationAsDeinited(src);
  if (sun::typeNeedsDrop(&type)) {
    ctx.builder->CreateMemSet(
        src, ConstantInt::get(llvm::Type::getInt8Ty(ctx.getContext()), 0), size,
        align);
  }
}

// Store one element into a slot of inline storage: a compound element moves
// in, a nested array is copied out of its temporary, a scalar is widened or
// narrowed to the slot's width.
void CodegenVisitor::storeArrayElement(Value* slotPtr, Value* elemVal,
                                       const sun::TypePtr& elemSunType,
                                       llvm::Type* slotType) {
  if (auto* nested = sun::tryGetType<sun::ArrayType>(elemSunType)) {
    emitArrayTransfer(slotPtr, elemVal, *nested, /*move=*/true);
    return;
  }
  sun::TypePtr valueType = sun::unwrapRef(elemSunType);
  if (valueType && (valueType->isClass() || valueType->isInterface() ||
                    isPayloadEnum(valueType))) {
    Value* moved = applyMoveSemantics(elemVal, valueType);
    if (moved->getType()->isPointerTy() && slotType->isStructTy()) {
      moved = ctx.builder->CreateLoad(slotType, moved, "elem.load");
    }
    ctx.builder->CreateStore(moved, slotPtr);
    return;
  }
  llvm::Type* valType = elemVal->getType();
  if (valType != slotType) {
    if (valType->isIntegerTy() && slotType->isIntegerTy()) {
      unsigned valBits = valType->getIntegerBitWidth();
      unsigned slotBits = slotType->getIntegerBitWidth();
      if (valBits < slotBits) {
        elemVal = extendInt(elemVal, slotType, elemSunType);
      } else if (valBits > slotBits) {
        elemVal = ctx.builder->CreateTrunc(elemVal, slotType, "trunc");
      }
    } else if (valType->isFloatTy() && slotType->isDoubleTy()) {
      elemVal = ctx.builder->CreateFPExt(elemVal, slotType, "fpext");
    } else if (valType->isDoubleTy() && slotType->isFloatTy()) {
      elemVal = ctx.builder->CreateFPTrunc(elemVal, slotType, "fptrunc");
    }
  }
  ctx.builder->CreateStore(elemVal, slotPtr);
}

// -------------------------------------------------------------------
// Array literal codegen: [1, 2, 3] or [[1, 2], [3, 4]]
// Fills fresh inline storage and yields its address
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const ArrayLiteralAST& expr) {
  const auto& elements = expr.getElements();

  if (elements.empty()) {
    logAndThrowError("Array literal cannot be empty");
    return nullptr;
  }

  sun::TypePtr arraySunType = expr.getResolvedType();
  auto* sunArrayType = &sun::requireType<sun::ArrayType>(expr, "array literal");
  const auto& dims = sunArrayType->getDimensions();

  if (dims.empty()) {
    logAndThrowError("Cannot create literal for unsized array");
    return nullptr;
  }

  Function* func = ctx.builder->GetInsertBlock()->getParent();
  llvm::Type* storageType = sunArrayType->getDataStorageType(ctx.getContext());
  AllocaInst* storage = createEntryBlockAlloca(func, "arr.lit", storageType);

  // The slot type at each outer index: the element for a 1-D array, the
  // inner [M x T] for a multi-dimensional one
  llvm::Type* slotType =
      sunArrayType->getElementType()->toLLVMType(ctx.getContext());
  for (auto it = dims.rbegin(); it != std::prev(dims.rend()); ++it) {
    slotType = llvm::ArrayType::get(slotType, *it);
  }

  for (size_t i = 0; i < elements.size(); ++i) {
    Value* elemVal = codegen(*elements[i]);
    if (!elemVal) return nullptr;
    Value* slotPtr = ctx.builder->CreateGEP(
        storageType, storage,
        {ctx.builder->getInt64(0), ctx.builder->getInt64(i)}, "arr.elem.ptr");
    storeArrayElement(slotPtr, elemVal, elements[i]->getResolvedType(),
                      slotType);
  }

  // A literal of owning elements is a temporary that drops them unless a
  // variable, field or call adopts it
  if (sun::typeNeedsDrop(arraySunType)) {
    scopes.trackClassAllocation(storage, "array.literal", arraySunType);
  }
  return storage;
}

// -------------------------------------------------------------------
// Array index codegen: arr[i] or arr[i, j, k]
// Also handles class indexing via __index__ and __slice__ methods
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const IndexAST& expr) {
  sun::TypePtr targetType = sun::unwrapRef(expr.getTarget()->getResolvedType());

  // Class targets dispatch to the __index__/__slice__ method protocol
  if (auto* classType = sun::tryGetType<sun::ClassType>(targetType)) {
    Value* targetVal = codegen(*expr.getTarget());
    if (!targetVal) return nullptr;
    if (expr.hasSlices()) {
      return codegenClassSlice(expr, targetVal, classType);
    }
    return codegenClassIndex(expr, targetVal, classType);
  }

  auto& sunArrayType = sun::requireType<sun::ArrayType>(
      targetType, "index target", expr.getLocation());

  Value* elemPtr = codegenIndexElementPtr(expr);
  if (!elemPtr) return nullptr;

  // A compound element is reached by its address, like a class field: it is
  // borrowed or moved from there, never loaded as a second copy
  const sun::TypePtr& elemSunType = sunArrayType.getElementType();
  if (elemSunType->isClass() || elemSunType->isInterface() ||
      elemSunType->isArray() || isPayloadEnum(elemSunType)) {
    return elemPtr;
  }

  llvm::Type* elemType = elemSunType->toLLVMType(ctx.getContext());
  return ctx.builder->CreateLoad(elemType, elemPtr, "arr.elem");
}

// -------------------------------------------------------------------
// Element address: static strides for a sized array, the view's dims table
// for an unsized one
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenIndexElementPtr(const IndexAST& expr) {
  Value* base = codegen(*expr.getTarget());
  if (!base) return nullptr;

  sun::TypePtr arrayType = sun::unwrapRef(expr.getTarget()->getResolvedType());
  auto* sunArrayType = &sun::requireType<sun::ArrayType>(
      arrayType, "index target", expr.getLocation());
  const auto& indices = expr.getIndices();
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());

  std::vector<Value*> indexValues;
  for (const auto& sliceExpr : indices) {
    if (sliceExpr->isRange()) {
      logAndThrowError(
          "Array slicing (a:b) not yet implemented - use single indices");
      return nullptr;
    }
    if (!sliceExpr->hasStart()) {
      logAndThrowError("Index expression is empty");
      return nullptr;
    }
    Value* idx = codegen(*sliceExpr->getStart());
    if (!idx) return nullptr;
    if (idx->getType()->isIntegerTy() &&
        idx->getType()->getIntegerBitWidth() < 64) {
      idx = ctx.builder->CreateSExt(idx, i64Ty, "idx.ext");
    }
    indexValues.push_back(idx);
  }

  if (!sunArrayType->isUnsized()) {
    llvm::Type* storageType =
        sunArrayType->getDataStorageType(ctx.getContext());
    if (!base->getType()->isPointerTy()) {
      Function* func = ctx.builder->GetInsertBlock()->getParent();
      AllocaInst* temp = createEntryBlockAlloca(func, "arr.spill", storageType);
      ctx.builder->CreateStore(base, temp);
      base = temp;
    }
    std::vector<Value*> gepIndices = {ctx.builder->getInt64(0)};
    gepIndices.insert(gepIndices.end(), indexValues.begin(), indexValues.end());
    return ctx.builder->CreateGEP(storageType, base, gepIndices,
                                  "arr.elem.ptr");
  }

  // A view: linear offset from the runtime dims table
  Value* view = loadArrayView(base);
  Value* dataPtr = ctx.builder->CreateExtractValue(view, 0, "arr.data.ptr");
  Value* dimsPtr = ctx.builder->CreateExtractValue(view, 2, "arr.dims.ptr");
  llvm::Type* elemType =
      sunArrayType->getElementType()->toLLVMType(ctx.getContext());

  Value* offset = ctx.builder->getInt64(0);
  for (size_t i = 0; i < indexValues.size(); ++i) {
    Value* stride = ctx.builder->getInt64(1);
    for (size_t j = i + 1; j < indexValues.size(); ++j) {
      Value* dimJPtr = ctx.builder->CreateGEP(
          i64Ty, dimsPtr, ctx.builder->getInt64(j), "dim.j.ptr");
      Value* dimJ = ctx.builder->CreateLoad(i64Ty, dimJPtr, "dim.j");
      stride = ctx.builder->CreateMul(stride, dimJ, "stride.mul");
    }
    Value* term = ctx.builder->CreateMul(indexValues[i], stride, "idx.stride");
    offset = ctx.builder->CreateAdd(offset, term, "offset.add");
  }
  return ctx.builder->CreateGEP(elemType, dataPtr, offset, "arr.elem.ptr");
}

// -------------------------------------------------------------------
// Indexed assignment: arr[i, j] = value or obj[i, j] = value
// For arrays: drops the old element and moves or stores the new one
// For classes: calls __setindex__
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const IndexedAssignmentAST& expr) {
  const ExprAST* indexTarget = expr.getTarget();
  if (indexTarget->getType() != ASTNodeType::INDEX) {
    logAndThrowError("Indexed assignment target must be an index expression");
    return nullptr;
  }

  const auto& indexExpr = static_cast<const IndexAST&>(*indexTarget);
  sun::TypePtr targetType =
      sun::unwrapRef(indexExpr.getTarget()->getResolvedType());

  if (auto* classType = sun::tryGetType<sun::ClassType>(targetType)) {
    if (classType->getMethod("__setindex__")) {
      return codegenClassSetIndex(indexExpr, expr.getValue(), classType);
    }
    logAndThrowError("Class " + classType->getDisplayName() +
                     " does not implement __setindex__ for indexed assignment");
    return nullptr;
  }

  auto& sunArrayType = sun::requireType<sun::ArrayType>(
      targetType, "index target", indexExpr.getLocation());

  Value* elemPtr = codegenIndexElementPtr(indexExpr);
  if (!elemPtr) return nullptr;

  Value* value = codegen(*expr.getValue());
  if (!value) return nullptr;

  // The slot held a value: release it before the new one moves in
  const sun::TypePtr& elemSunType = sunArrayType.getElementType();
  if (sun::typeNeedsDrop(elemSunType)) {
    scopes.emitDropInPlace(elemSunType, elemPtr, "elem");
  }
  storeArrayElement(elemPtr, value, expr.getValue()->getResolvedType(),
                    elemSunType->toLLVMType(ctx.getContext()));
  return value;
}

// -------------------------------------------------------------------
// arr.ndims() and arr.dim(i): the rank and one dimension size, as i64.
// Constants for a sized array; reads of the view struct for an unsized one.
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenArrayQuery(const CallExprAST& call,
                                         const MemberAccessAST& member) {
  const std::string& name = member.getMemberName();
  sun::TypePtr objectType =
      sun::unwrapRef(member.getObject()->getResolvedType());
  auto* arrayType = &sun::requireType<sun::ArrayType>(
      objectType, name + "() receiver", member.getLocation());
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  const auto& dims = arrayType->getDimensions();

  if (name == "ndims") {
    if (!arrayType->isUnsized()) return ctx.builder->getInt64(dims.size());
    Value* view = loadArrayView(codegen(*member.getObject()));
    Value* ndims = ctx.builder->CreateExtractValue(view, 1, "view.ndims");
    return ctx.builder->CreateZExt(ndims, i64Ty, "ndims.i64");
  }

  Value* index = codegen(*call.getArgs()[0]);
  if (!index) return nullptr;
  if (index->getType()->isIntegerTy() &&
      index->getType()->getIntegerBitWidth() < 64) {
    index = ctx.builder->CreateSExt(index, i64Ty, "dim.idx");
  }

  Value* dimsPtr;
  if (!arrayType->isUnsized()) {
    if (auto* constIndex = dyn_cast<ConstantInt>(index)) {
      uint64_t i = constIndex->getZExtValue();
      if (i < dims.size()) return ctx.builder->getInt64(dims[i]);
    }
    dimsPtr = arrayDimsTable(dims);
  } else {
    Value* view = loadArrayView(codegen(*member.getObject()));
    dimsPtr = ctx.builder->CreateExtractValue(view, 2, "view.dims");
  }
  Value* dimPtr = ctx.builder->CreateGEP(i64Ty, dimsPtr, index, "dim.ptr");
  return ctx.builder->CreateLoad(i64Ty, dimPtr, "dim");
}

// -------------------------------------------------------------------
// Class indexing via __index__ method: obj[i, j, k] -> obj.__index__([i,j,k])
// Synthesizes a call to the __index__ method with indices as a view
// -------------------------------------------------------------------

// Box an IndexAST's index expressions into a `ref array<i64>` view over a
// fresh [N x i64] on this frame's stack. Shared by the __index__ and
// __setindex__ call paths so compound assignment can evaluate indices once.
Value* CodegenVisitor::boxIndicesToArrayRef(const IndexAST& expr) {
  const auto& indices = expr.getIndices();
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());

  size_t numIndices = indices.size();
  llvm::ArrayType* arrDataType = llvm::ArrayType::get(i64Ty, numIndices);
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* arrData = createEntryBlockAlloca(func, "idx.arr", arrDataType);

  for (size_t i = 0; i < numIndices; ++i) {
    const auto& sliceExpr = *indices[i];
    if (sliceExpr.isRange()) {
      logAndThrowError("Cannot use range slices in indexed access");
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
        arrDataType, arrData,
        {ctx.builder->getInt64(0), ctx.builder->getInt64(i)}, "idx.elem.ptr");
    ctx.builder->CreateStore(idxVal, elemPtr);
  }

  return emitArrayView(arrData, {numIndices});
}

// Declare a class's __index__/__slice__/__setindex__ method on demand when
// it has not been emitted yet: (closure, view [, value]) -> return type
Function* CodegenVisitor::declareIndexProtocolMethod(
    sun::ClassType* classType, const sun::ClassMethod& method,
    const std::string& mangledName, llvm::Type* valueParamType) {
  if (Function* existing = module->getFunction(mangledName)) return existing;
  std::vector<llvm::Type*> paramTypes;
  paramTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // closure
  paramTypes.push_back(
      sun::ArrayType::getArrayStructType(ctx.getContext()));  // the view
  if (valueParamType) paramTypes.push_back(valueParamType);

  llvm::Type* returnType;
  if (method.returnType && method.returnType->toString() != "void") {
    returnType = typeResolver.resolveForReturn(method.returnType);
  } else {
    returnType = Type::getVoidTy(ctx.getContext());
  }
  FunctionType* funcType = FunctionType::get(returnType, paramTypes, false);
  return Function::Create(funcType, Function::ExternalLinkage, mangledName,
                          module);
}

// Call obj.__index__(indices) with a pre-boxed index view
Value* CodegenVisitor::emitClassIndexCall(Value* objectPtr, Value* idxView,
                                          sun::ClassType* classType) {
  const sun::ClassMethod* method = classType->getMethod("__index__");
  if (!method) {
    logAndThrowError("Class " + classType->getDisplayName() +
                     " does not have __index__ method");
    return nullptr;
  }

  std::string mangledName =
      classType->getMangledMethodName("__index__", method->paramTypes);
  Function* methodFunc =
      declareIndexProtocolMethod(classType, *method, mangledName, nullptr);

  std::vector<Value*> argValues;
  argValues.push_back(materializeMethodClosure(methodFunc, objectPtr));
  argValues.push_back(idxView);

  Value* result =
      ctx.builder->CreateCall(methodFunc, argValues, "index.result");
  return materializeStructReturn(result);
}

Value* CodegenVisitor::codegenClassIndex(const IndexAST& expr, Value* objectPtr,
                                         sun::ClassType* classType) {
  Value* idxView = boxIndicesToArrayRef(expr);
  if (!idxView) return nullptr;
  return emitClassIndexCall(objectPtr, idxView, classType);
}

// -------------------------------------------------------------------
// Class slicing via __slice__ method: obj[a:b, :, c:] -> obj.__slice__(ranges)
// Synthesizes a call to the __slice__ method with a view of SliceRanges
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenClassSlice(const IndexAST& expr, Value* objectPtr,
                                         sun::ClassType* classType) {
  const auto& indices = expr.getIndices();
  llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.getContext());
  llvm::Type* i1Ty = llvm::Type::getInt1Ty(ctx.getContext());

  const sun::ClassMethod* method = classType->getMethod("__slice__");
  if (!method) {
    logAndThrowError("Class " + classType->getDisplayName() +
                     " does not have __slice__ method");
    return nullptr;
  }

  // SliceRange struct type matches Sun's definition: { i64 start, i64 end, i1
  // hasStart, i1 hasEnd } Use named struct type for compatibility with
  // SliceRange class methods
  llvm::StructType* sliceRangeType =
      llvm::StructType::getTypeByName(ctx.getContext(), "SliceRange_struct");
  if (!sliceRangeType) {
    sliceRangeType = llvm::StructType::create(
        ctx.getContext(), {i64Ty, i64Ty, i1Ty, i1Ty}, "SliceRange_struct");
  }

  size_t numSlices = indices.size();
  llvm::ArrayType* rangesArrType =
      llvm::ArrayType::get(sliceRangeType, numSlices);
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* rangesData =
      createEntryBlockAlloca(func, "slice.ranges", rangesArrType);

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
      // A single index in a slice context is the range a:a+1
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
      endVal = ctx.builder->CreateAdd(startVal, ctx.builder->getInt64(1),
                                      "end.single");
    }

    Value* rangePtr = ctx.builder->CreateGEP(
        rangesArrType, rangesData,
        {ctx.builder->getInt64(0), ctx.builder->getInt64(i)}, "range.ptr");
    ctx.builder->CreateStore(
        startVal, ctx.builder->CreateStructGEP(sliceRangeType, rangePtr, 0,
                                               "range.start.ptr"));
    ctx.builder->CreateStore(
        endVal, ctx.builder->CreateStructGEP(sliceRangeType, rangePtr, 1,
                                             "range.end.ptr"));
    ctx.builder->CreateStore(
        hasStartVal, ctx.builder->CreateStructGEP(sliceRangeType, rangePtr, 2,
                                                  "range.hasStart.ptr"));
    ctx.builder->CreateStore(
        hasEndVal, ctx.builder->CreateStructGEP(sliceRangeType, rangePtr, 3,
                                                "range.hasEnd.ptr"));
  }

  Value* rangesView = emitArrayView(rangesData, {numSlices});

  std::string mangledName =
      classType->getMangledMethodName("__slice__", method->paramTypes);
  Function* methodFunc =
      declareIndexProtocolMethod(classType, *method, mangledName, nullptr);

  std::vector<Value*> argValues;
  argValues.push_back(materializeMethodClosure(methodFunc, objectPtr));
  argValues.push_back(rangesView);

  Value* result =
      ctx.builder->CreateCall(methodFunc, argValues, "slice.result");
  return materializeStructReturn(result);
}

// -------------------------------------------------------------------
// Class indexed assignment via __setindex__ method: obj[i, j] = val
// Synthesizes a call to __setindex__(indices, value)
// -------------------------------------------------------------------

// Call obj.__setindex__(indices, value) with a pre-boxed index view
Value* CodegenVisitor::emitClassSetIndexCall(Value* objectPtr, Value* idxView,
                                             Value* value,
                                             sun::ClassType* classType) {
  const sun::ClassMethod* method = classType->getMethod("__setindex__");
  if (!method) {
    logAndThrowError("Class " + classType->getDisplayName() +
                     " does not have __setindex__ method");
    return nullptr;
  }

  std::string mangledName =
      classType->getMangledMethodName("__setindex__", method->paramTypes);
  llvm::Type* valueParamType = method->paramTypes.size() >= 2
                                   ? typeResolver.resolve(method->paramTypes[1])
                                   : value->getType();
  Function* methodFunc = declareIndexProtocolMethod(
      classType, *method, mangledName, valueParamType);

  // Coerce the value to the __setindex__ value-parameter type (e.g. an i64
  // loop counter assigned into a Vec<i32>)
  if (method->paramTypes.size() >= 2) {
    llvm::Type* paramTy = typeResolver.resolve(method->paramTypes[1]);
    if (value->getType() != paramTy && value->getType()->isIntegerTy() &&
        paramTy->isIntegerTy()) {
      unsigned valueBits = value->getType()->getIntegerBitWidth();
      unsigned paramBits = paramTy->getIntegerBitWidth();
      value = valueBits > paramBits
                  ? ctx.builder->CreateTrunc(value, paramTy, "setidx.trunc")
                  : extendInt(value, paramTy, method->paramTypes[1]);
    }
  }

  std::vector<Value*> argValues;
  argValues.push_back(materializeMethodClosure(methodFunc, objectPtr));
  argValues.push_back(idxView);
  argValues.push_back(value);
  ctx.builder->CreateCall(methodFunc, argValues);
  return value;
}

Value* CodegenVisitor::codegenClassSetIndex(const IndexAST& indexExpr,
                                            const ExprAST* valueExpr,
                                            sun::ClassType* classType) {
  Value* objectPtr = codegen(*indexExpr.getTarget());
  if (!objectPtr) return nullptr;

  Value* idxView = boxIndicesToArrayRef(indexExpr);
  if (!idxView) return nullptr;

  Value* valueVal = codegen(*valueExpr);
  if (!valueVal) return nullptr;

  return emitClassSetIndexCall(objectPtr, idxView, valueVal, classType);
}
