// src/codegen/intrinsics/generic.cpp - Generic intrinsic function codegen
//
// This file contains codegen for generic (type-parameterized) intrinsics:
// - _sizeof<T>, _init<T>, _load<T>, _store<T>
// - _ptr_as_raw<T>, _address_of<T>, _to_ref<T>, _is<T>

#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics.h"
#include "codegen/intrinsics/intrinsics_generator.h"
#include "support/error.h"

using namespace llvm;

// -------------------------------------------------------------------
// Generic intrinsics codegen
// Called from codegen(GenericCallAST) for _sizeof, _own, _init, _load, _store
// -------------------------------------------------------------------

Value* IntrinsicsGenerator::codegenSizeofIntrinsic(sun::TypePtr targetType) {
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

Value* IntrinsicsGenerator::codegenInitIntrinsic(
    sun::TypePtr targetType, const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<sun::ArgConversion>& conversions) {
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

  // Resolve the constructor the arguments select (variadic packs are already
  // expanded into concrete typed args by semantic analysis). Declare it if
  // the class is processed later in codegen order.
  std::vector<sun::TypePtr> argTypes;
  for (size_t i = 1; i < args.size(); ++i) {
    argTypes.push_back(args[i]->getResolvedType());
  }
  ClassGenerator::ConstructorLookup ctor =
      gen_.classGenerator().lookupConstructor(classType, argTypes);
  const size_t ctorArgCount = args.size();  // 'this' replaces the pointer

  Function* ctorFunc = nullptr;
  Function* candidate =
      ctor.method ? gen_.functionRegistry().getOrDeclareMethodFunction(
                        ctor.mangledName, ctor.method->paramTypes,
                        ctor.method->returnType, ctor.method->canThrow)
                  : module->getFunction(ctor.mangledName);
  if (candidate && candidate->arg_size() == ctorArgCount) {
    ctorFunc = candidate;
  }

  if (!ctorFunc) {
    // Zeroed storage fully describes a class with no constructor. Arguments
    // that reach no constructor would be dropped on the floor, which is a
    // miscompile.
    if (ctorArgCount > 1) {
      logAndThrowError("No constructor to initialize " +
                       classType->getDisplayName() + " with " +
                       std::to_string(ctorArgCount - 1) + " argument(s)");
    }
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
  }

  // The arguments, lowered exactly as a direct constructor call lowers them:
  // semantic analysis recorded one conversion each (a compound argument
  // moves, a class becomes an interface fat pointer, ...). Slot 0 is the
  // destination pointer, which becomes the method closure.
  std::vector<Value*> ctorArgs{
      gen_.materializeMethodClosure(ctorFunc, rawPtr, "init.closure")};
  const auto& paramTypes =
      ctor.method ? ctor.method->paramTypes : std::vector<sun::TypePtr>{};
  if (!gen_.emitCallArguments(args, conversions, paramTypes,
                              ctorFunc->getFunctionType(), ctorArgs, "_init",
                              /*firstArg=*/1)) {
    return nullptr;
  }
  ctx.builder->CreateCall(ctorFunc, ctorArgs);

  return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
}

Value* IntrinsicsGenerator::codegenLoadIntrinsic(
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

  // Compound elements stay addressable so the consumer can move or borrow.
  if (targetType->isClass() || targetType->isInterface() ||
      CodegenVisitor::isPayloadEnum(targetType)) {
    return elemPtr;
  }

  return ctx.builder->CreateLoad(elemType, elemPtr, "elem.val");
}

Value* IntrinsicsGenerator::codegenStoreIntrinsic(
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

  // Compound values may arrive by address or as a by-value parameter. An
  // addressable source moves into the slot and is invalidated.
  if (targetType->isClass() || targetType->isInterface() ||
      CodegenVisitor::isPayloadEnum(targetType)) {
    llvm::Value* structVal = value;
    if (value->getType()->isPointerTy()) {
      structVal = gen_.applyMoveSemantics(value, targetType);
    }
    ctx.builder->CreateStore(structVal, elemPtr);
    return structVal;
  }

  ctx.builder->CreateStore(value, elemPtr);
  return value;
}

Value* IntrinsicsGenerator::codegenPtrAsRawIntrinsic(
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

Value* IntrinsicsGenerator::codegenAddressOfIntrinsic(
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _address_of<T>(ref T) returns raw_ptr<T> - the address of the argument
  // Works on any lvalue: variables, fields, array elements, etc.
  if (args.size() != 1) {
    logAndThrowError("_address_of<T>() requires exactly one argument");
    return nullptr;
  }

  const ExprAST* argExpr = args[0].get();

  Value* addr = gen_.tryCodegenAddress(*argExpr);
  if (!addr) {
    logAndThrowError(
        "_address_of<T>() requires an addressable expression (variable, "
        "field, array element, or this)");
    return nullptr;
  }
  return addr;
}

Value* IntrinsicsGenerator::codegenToRefIntrinsic(
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

Value* IntrinsicsGenerator::codegenIsIntrinsic(
    const std::string& targetName,
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _is<T>(value) - compile-time type check, folded to a constant here.
  // Which types satisfy which trait is sun::traits::satisfies (see
  // semantic_analysis/type_traits.h); a `<T: Trait>` constraint asks that same
  // predicate at a signature.

  if (args.size() != 1) {
    logAndThrowError("_is<T>(value) requires exactly one argument");
    return nullptr;
  }

  sun::TypePtr valueType = args[0]->getResolvedType();
  if (!valueType) {
    logAndThrowError("Cannot determine type of argument to _is<T>");
    return nullptr;
  }

  bool result = sun::traits::satisfies(valueType, targetName);
  return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx.getContext()),
                                result ? 1 : 0);
}

Value* IntrinsicsGenerator::codegenDeinitIntrinsic(
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
    scopes().emitDeinitCall(classType, ptr);

    // Recursively deinit class fields that have deinit methods
    scopes().emitFieldDeinit(ptr, classType, "deinit.intrinsic");
  } else if (typeArg && typeArg->isEnum()) {
    // Payload enums with owning payloads drop through their drop function
    gen_.enumGenerator().emitDrop(static_cast<sun::EnumType&>(*typeArg), ptr);
  }

  return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
}

Value* IntrinsicsGenerator::codegenConvertIntrinsic(
    sun::TypePtr targetType,
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _convert<T>(value) - explicit numeric conversion. Integers truncate or
  // extend (sign-extending from signed sources, zero-extending from unsigned);
  // int<->float convert by value. The one escape hatch Sun offers for
  // narrowing, since assignments never narrow implicitly.
  //
  // char joins as an integer-only party: a char converts to and from the
  // integer types (unchecked, like every other narrowing here — std.char_of
  // is the checked form), but never to or from a float or a bool.
  if (args.size() != 1) {
    logAndThrowError("_convert<T>() requires exactly 1 argument");
    return nullptr;
  }
  if (!targetType || (!targetType->isNumeric() && !targetType->isChar())) {
    logAndThrowError("_convert<T>: T must be a numeric type or char");
    return nullptr;
  }
  sun::TypePtr srcType = sun::unwrapRef(args[0]->getResolvedType());
  if (srcType && srcType->isEnum()) {
    if (static_cast<sun::EnumType*>(srcType.get())->hasPayload() ||
        !targetType->isIntegral()) {
      logAndThrowError(
          "_convert<T>: enums without payloads convert to integers only");
    }
  }
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

  // Enum conversions use the signedness of their integer representation.
  if (srcType && srcType->isEnum()) {
    srcType = static_cast<sun::EnumType*>(srcType.get())->getUnderlyingType();
  }
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

Value* IntrinsicsGenerator::codegenBitcastIntrinsic(
    sun::TypePtr targetType,
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // _bitcast<T>(value) - reinterpret a value's bits as a same-size type T.
  //
  // Two shapes, and a bitcast never mixes them:
  //   numeric -> numeric      f32 <-> u32/i32, f64 <-> u64/i64. Used by
  //                           binary wire formats to read and write floats.
  //   raw_ptr<A> -> raw_ptr<B>  the pointer cast C spells `(B*)p`. Needed to
  //                           hand a pointer to any type to a C function that
  //                           takes `void*` (declared `raw_ptr<u8>` in Sun),
  //                           and to read a byte buffer as a packed header.
  //
  // The cast only renames the pointer; reading through the result still needs
  // an unsafe block, and it is on the writer to have pointed at bytes that
  // really are a B.
  if (args.size() != 1) {
    logAndThrowError("_bitcast<T>() requires exactly 1 argument");
    return nullptr;
  }
  bool targetIsPointer = targetType && targetType->isRawPointer();
  if (!targetType || (!targetType->isNumeric() && !targetIsPointer)) {
    logAndThrowError("_bitcast<T>: T must be a numeric type or a raw_ptr");
    return nullptr;
  }
  sun::TypePtr srcType = args[0]->getResolvedType();
  if (srcType && srcType->isRawPointer() != targetIsPointer) {
    std::string hint =
        srcType->isStaticPointer()
            ? "; take the data pointer out of the static_ptr with '.raw()' "
              "first"
            : "; a bitcast is numeric to numeric or raw_ptr to raw_ptr, never "
              "one to the other";
    logAndThrowError("_bitcast<T>: cannot reinterpret '" +
                     srcType->toDisplayString() + "' as '" +
                     targetType->toDisplayString() + "'" + hint);
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
  // Pointers are opaque in LLVM, so a pointer cast is already a no-op here.
  if (v->getType() == dstTy) return v;
  return ctx.builder->CreateBitCast(v, dstTy, "bitcast");
}

Value* IntrinsicsGenerator::codegenEnumFromIntIntrinsic(
    const GenericCallAST& expr) {
  auto& target =
      *static_cast<sun::EnumType*>(expr.getResolvedTypeArgs()[0].get());
  auto& result = *static_cast<sun::EnumType*>(expr.getResolvedType().get());
  const auto& arg = expr.getArgs()[0];
  auto sourceType = sun::unwrapRef(arg->getResolvedType());
  Value* value = codegen(*arg);
  // Compare before narrowing, preserving both signed and unsigned inputs.
  auto* comparisonType = IntegerType::get(ctx.getContext(), 65);
  value = ctx.builder->CreateIntCast(value, comparisonType,
                                     !sourceType->isUnsigned());
  Value* valid = ConstantInt::getFalse(ctx.getContext());
  for (const auto& variant : target.getVariants()) {
    Value* matches = ctx.builder->CreateICmpEQ(
        value,
        ConstantInt::get(
            ctx.getContext(),
            target.getUnderlyingType()->isUnsigned()
                ? APInt(64, static_cast<uint64_t>(variant.value)).zext(65)
                : APInt(64, static_cast<uint64_t>(variant.value)).sext(65)));
    valid = ctx.builder->CreateOr(valid, matches);
  }
  auto* storageType = typeResolver.getEnumStorageType(result);
  auto* function = ctx.builder->GetInsertBlock()->getParent();
  auto* storage =
      gen_.createEntryBlockAlloca(function, "enum.option", storageType);
  ctx.builder->CreateStore(Constant::getNullValue(storageType), storage);
  auto* tagType = Type::getInt32Ty(ctx.getContext());
  Value* tag = ctx.builder->CreateSelect(
      valid, ConstantInt::get(tagType, result.getVariant("Some")->value),
      ConstantInt::get(tagType, result.getVariant("None")->value));
  ctx.builder->CreateStore(
      tag, ctx.builder->CreateStructGEP(storageType, storage, 0));
  auto* someType = typeResolver.getEnumVariantStruct(result, "Some");
  unsigned field = typeResolver.enumPayloadFieldIndex(result, "Some", 0);
  ctx.builder->CreateStore(
      ctx.builder->CreateTrunc(value, target.toLLVMType(ctx.getContext())),
      ctx.builder->CreateStructGEP(someType, storage, field));
  return storage;
}
