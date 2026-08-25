// src/codegen/intrinsics/generic.cpp - Generic intrinsic function codegen
//
// This file contains codegen for generic (type-parameterized) intrinsics:
// - _sizeof<T>, _init<T>, _load<T>, _store<T>
// - _ptr_as_raw<T>, _address_of<T>, _to_ref<T>, _is<T>

#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics.h"
#include "support/error.h"

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
  auto* classType = sun::tryGetType<sun::ClassType>(targetType);
  if (!classType) {
    // For non-class types, _init is a no-op (primitives are zero-initialized)
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
  }

  // Zero the target first (like stack construction does): field assignments
  // in the constructor drop the field's previous value, which must be the
  // well-defined "nothing" state rather than heap garbage.
  {
    llvm::StructType* structTy = classType->getStructType(ctx.getContext());
    const DataLayout& DL = module->getDataLayout();
    ctx.builder->CreateMemSet(
        rawPtr, ConstantInt::get(Type::getInt8Ty(ctx.getContext()), 0),
        DL.getTypeAllocSize(structTy), llvm::MaybeAlign(1));
  }

  // Collect argument Sun types first (variadic packs are already expanded
  // into concrete typed args by semantic analysis) so the constructor can be
  // resolved before adapting values to its calling convention.
  std::vector<sun::TypePtr> argTypes;
  for (size_t i = 1; i < args.size(); ++i) {
    argTypes.push_back(args[i]->getResolvedType());
  }

  // Look up the constructor (init method) that is compatible with the argument
  // types.
  ConstructorLookup ctor = lookupConstructor(classType, argTypes);

  std::vector<Value*> ctorArgs;
  // Slot 0 is the method closure; patched below once the ctor is resolved.
  ctorArgs.push_back(rawPtr);

  // Skip args[0] (the pointer)
  for (size_t i = 1; i < args.size(); ++i) {
    Value* argVal = codegen(*args[i]);
    if (!argVal) return nullptr;
    sun::TypePtr argType = args[i]->getResolvedType();

    sun::TypePtr paramType =
        (ctor.method && i - 1 < ctor.method->paramTypes.size())
            ? ctor.method->paramTypes[i - 1]
            : nullptr;
    bool paramIsRef = paramType && paramType->isReference();

    // Compound values are addressable, so codegen yields a pointer to the
    // struct. Ref params take that pointer directly; by-value params MOVE
    // the value into the constructor (source zeroed / tag-poisoned and its
    // tracking released) to match the calling convention without copying.
    if (argType && argType->isCompound() && argVal->getType()->isPointerTy() &&
        !paramIsRef) {
      argVal = applyMoveSemantics(argVal, argType);
    }

    ctorArgs.push_back(argVal);
  }

  Function* ctorFunc = nullptr;
  size_t ctorArgCount = ctorArgs.size();  // includes 'this' pointer

  // Try to find existing constructor
  Function* candidate = module->getFunction(ctor.mangledName);
  if (candidate && candidate->arg_size() == ctorArgCount) {
    ctorFunc = candidate;
  }

  // If not found, try to create a declaration for it
  // This handles cases where the class is processed later in codegen order
  if (!ctorFunc && ctor.method &&
      ctor.method->paramTypes.size() + 1 == ctorArgCount) {
    // Build parameter types for the constructor
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // this
    for (const auto& paramType : ctor.method->paramTypes) {
      paramTypes.push_back(typeResolver.resolve(paramType));
    }
    FunctionType* funcType =
        FunctionType::get(Type::getVoidTy(ctx.getContext()), paramTypes, false);
    ctorFunc = Function::Create(funcType, Function::ExternalLinkage,
                                ctor.mangledName, module);
  }

  if (ctorFunc) {
    ctorArgs[0] = materializeMethodClosure(ctorFunc, rawPtr, "init.closure");
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

  // For class and payload-enum types, return the pointer to the element
  // (compounds are addressable; the consumer moves or borrows from it)
  if (targetType->isClass() || isPayloadEnum(targetType)) {
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

  // For class and payload-enum types, value may be a pointer (alloca) or
  // already a struct value (e.g. from a by-value parameter). An addressable
  // source MOVES into the slot (never an implicit copy): it is invalidated
  // and its drop tracking released.
  if (targetType->isClass() || isPayloadEnum(targetType)) {
    llvm::Value* structVal = value;
    if (value->getType()->isPointerTy()) {
      structVal = applyMoveSemantics(value, targetType);
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

Value* CodegenVisitor::codegenAddressOfIntrinsic(
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _address_of<T>(ref T) returns raw_ptr<T> - the address of the argument
  // Works on any lvalue: variables, fields, array elements, etc.
  if (args.size() != 1) {
    logAndThrowError("_address_of<T>() requires exactly one argument");
    return nullptr;
  }

  const ExprAST* argExpr = args[0].get();

  Value* addr = tryCodegenAddress(*argExpr);
  if (!addr) {
    logAndThrowError(
        "_address_of<T>() requires an addressable expression (variable, "
        "field, array element, or this)");
    return nullptr;
  }
  return addr;
}

Value* CodegenVisitor::codegenToRefIntrinsic(
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _to_ref<T>(raw_ptr<T>) -> ref T
  // Converts a raw pointer to a reference (unsafe operation)
  // At LLVM level, both are pointers - this is just a type system operation
  if (args.size() != 1) {
    logAndThrowError("_to_ref<T>() requires exactly one argument");
    return nullptr;
  }

  // Generate the pointer value
  llvm::Value* ptrVal = codegen(*args[0]);
  if (!ptrVal) {
    logAndThrowError("_to_ref<T>(): Failed to generate pointer argument");
    return nullptr;
  }

  // At LLVM level, raw_ptr<T> and ref T are both pointers
  // Just return the value - the type system handles the semantic difference
  return ptrVal;
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
      if (auto* classType = sun::tryGetType<sun::ClassType>(valueType)) {
        // Check if targetName is an interface this class implements
        if (classType->implementsInterface(targetName)) {
          result = true;
        } else {
          // Check for exact class name match
          result = classType->getMangledName() == targetName;
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

Value* CodegenVisitor::codegenDeinitIntrinsic(
    sun::TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _deinit<T>(raw_ptr<T>) - call T.deinit() on the pointee if T is a class
  // with a deinit method, then recursively deinit class fields. No-op for
  // non-class types or classes without deinit.
  if (args.size() != 1) {
    logAndThrowError("_deinit<T>() requires exactly 1 argument: (raw_ptr<T>)");
    return nullptr;
  }

  llvm::Value* ptr = codegen(*args[0]);
  if (!ptr) return nullptr;

  if (auto* classType = sun::tryGetType<sun::ClassType>(typeArg)) {
    emitDeinitCall(classType, ptr);

    // Recursively deinit class fields that have deinit methods
    emitFieldDeinit(ptr, classType, "deinit.intrinsic");
  } else if (typeArg && typeArg->isEnum()) {
    // Payload enums with owning payloads drop through their drop function
    emitEnumDrop(static_cast<sun::EnumType&>(*typeArg), ptr);
  }

  return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
}

Value* CodegenVisitor::codegenConvertIntrinsic(
    sun::TypePtr targetType,
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _convert<T>(value) - explicit numeric conversion. Integers truncate or
  // extend (sign-extending from signed sources, zero-extending from unsigned);
  // int<->float convert by value. The one escape hatch Sun offers for
  // narrowing, since assignments never narrow implicitly.
  //
  // char joins as an integer-only party: a char converts to and from the
  // integer types (unchecked, like every other narrowing here — sun.char_of
  // is the checked form), but never to or from a float or a bool.
  if (args.size() != 1) {
    logAndThrowError("_convert<T>() requires exactly 1 argument");
    return nullptr;
  }
  if (!targetType || (!targetType->isNumeric() && !targetType->isChar())) {
    logAndThrowError("_convert<T>: T must be a numeric type or char");
    return nullptr;
  }
  sun::TypePtr srcType = args[0]->getResolvedType();
  if (targetType->isChar() || (srcType && srcType->isChar())) {
    const sun::TypePtr& other = targetType->isChar() ? srcType : targetType;
    if (other && (other->isFloatingPoint() || other->isBool())) {
      logAndThrowError(
          "_convert<T>: char converts to and from the integer "
          "types only, not '" +
          other->toDisplayString() + "'");
      return nullptr;
    }
  }
  llvm::Value* v = codegen(*args[0]);
  if (!v) return nullptr;
  llvm::Type* dstTy = typeResolver.resolve(targetType);
  llvm::Type* srcTy = v->getType();
  if (srcTy == dstTy) return v;

  // A char is a non-negative scalar value, so it always zero-extends.
  bool srcSigned = srcType && srcType->isIntegral() && !srcType->isUnsigned();
  bool dstSigned = !targetType->isUnsigned();

  if (srcTy->isIntegerTy() && dstTy->isIntegerTy()) {
    return ctx.builder->CreateIntCast(v, dstTy, srcSigned, "convert");
  }
  if (srcTy->isIntegerTy() && dstTy->isFloatingPointTy()) {
    return srcSigned ? ctx.builder->CreateSIToFP(v, dstTy, "convert")
                     : ctx.builder->CreateUIToFP(v, dstTy, "convert");
  }
  if (srcTy->isFloatingPointTy() && dstTy->isIntegerTy()) {
    return dstSigned ? ctx.builder->CreateFPToSI(v, dstTy, "convert")
                     : ctx.builder->CreateFPToUI(v, dstTy, "convert");
  }
  if (srcTy->isFloatingPointTy() && dstTy->isFloatingPointTy()) {
    return ctx.builder->CreateFPCast(v, dstTy, "convert");
  }
  logAndThrowError("_convert<T>: unsupported conversion");
  return nullptr;
}

Value* CodegenVisitor::codegenBitcastIntrinsic(
    sun::TypePtr targetType,
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _bitcast<T>(value) - reinterpret the bits of a same-size numeric value
  // (f32 <-> u32/i32, f64 <-> u64/i64). Used by binary wire formats.
  if (args.size() != 1) {
    logAndThrowError("_bitcast<T>() requires exactly 1 argument");
    return nullptr;
  }
  if (!targetType || !targetType->isNumeric()) {
    logAndThrowError("_bitcast<T>: T must be a numeric type");
    return nullptr;
  }
  llvm::Value* v = codegen(*args[0]);
  if (!v) return nullptr;
  llvm::Type* dstTy = typeResolver.resolve(targetType);
  const DataLayout& DL = module->getDataLayout();
  if (DL.getTypeSizeInBits(v->getType()) != DL.getTypeSizeInBits(dstTy)) {
    logAndThrowError("_bitcast<T>: source and target sizes differ");
    return nullptr;
  }
  if (v->getType() == dstTy) return v;
  return ctx.builder->CreateBitCast(v, dstTy, "bitcast");
}
