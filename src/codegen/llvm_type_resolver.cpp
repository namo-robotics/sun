// llvm_type_resolver.cpp — Implementation of sun::Type to llvm::Type resolution

#include "codegen/llvm_type_resolver.h"

#include "llvm/IR/DerivedTypes.h"

using namespace llvm;

// -----------------------------------------------------------------------------
// Closure type management
// -----------------------------------------------------------------------------

StructType* LLVMTypeResolver::getClosureType() {
  if (!closureType) {
    closureType = StructType::create(ctx, sun::StructNames::Closure);
    closureType->setBody({
        PointerType::getUnqual(ctx),  // func*
        PointerType::getUnqual(ctx)   // env*
    });
  }
  return closureType;
}

StructType* LLVMTypeResolver::getStaticPtrType() {
  if (!staticPtrType) {
    // Delegate to StaticPointerType::toLLVMType which handles deduplication
    sun::StaticPointerType tempType(sun::Types::UInt8());
    staticPtrType = llvm::cast<StructType>(tempType.toLLVMType(ctx));
  }
  return staticPtrType;
}

// -----------------------------------------------------------------------------
// Payload enum layout
// -----------------------------------------------------------------------------

void LLVMTypeResolver::prepareEnumFieldStorage(
    const sun::ClassType& classType) {
  for (const auto& field : classType.getFields()) {
    if (!field.type) continue;
    if (field.type->isEnum()) {
      const auto& enumType = static_cast<const sun::EnumType&>(*field.type);
      if (enumType.hasPayload()) getEnumStorageType(enumType);
    } else if (field.type->isClass()) {
      prepareEnumFieldStorage(static_cast<const sun::ClassType&>(*field.type));
    }
  }
}

StructType* LLVMTypeResolver::getEnumStorageType(
    const sun::EnumType& enumType) {
  if (enumType.cachedStorageType) return enumType.cachedStorageType;
  if (!dataLayout) {
    logAndThrowError("payload enum '" + enumType.getDisplayName() +
                     "' layout requires a module DataLayout (compiler bug)");
  }

  uint64_t maxSize = 8;  // at least { i32 tag } rounded to a unit
  Align maxAlign(4);     // at least the i32 tag
  for (const auto& v : enumType.getVariants()) {
    if (!v.hasPayload()) continue;
    StructType* variantStruct = getEnumVariantStruct(enumType, v.name);
    maxSize = std::max(
        maxSize, dataLayout->getTypeAllocSize(variantStruct).getFixedValue());
    maxAlign = std::max(maxAlign, dataLayout->getABITypeAlign(variantStruct));
  }

  const uint64_t unitBytes = maxAlign.value();
  Type* unitTy = Type::getIntNTy(ctx, unitBytes * 8);
  const uint64_t tagArea = alignTo(4, unitBytes);
  const uint64_t payloadBytes = maxSize > tagArea ? maxSize - tagArea : 0;
  const uint64_t numUnits = (payloadBytes + unitBytes - 1) / unitBytes;

  // Share one storage struct per enum name (see ClassType::getStructType)
  std::string storageName = enumType.getName() + "_struct";
  StructType* storage = StructType::getTypeByName(ctx, storageName);
  if (storage && storage->isOpaque()) {
    storage->setBody({Type::getInt32Ty(ctx), ArrayType::get(unitTy, numUnits)});
  } else if (!storage) {
    storage = StructType::create(
        ctx, {Type::getInt32Ty(ctx), ArrayType::get(unitTy, numUnits)},
        storageName);
  }

  // The storage must be able to hold every variant view
  for (const auto& [name, variantStruct] : enumType.cachedVariantStructs) {
    (void)name;
    assert(dataLayout->getTypeAllocSize(storage) >=
               dataLayout->getTypeAllocSize(variantStruct) &&
           "enum storage smaller than a variant");
  }

  enumType.cachedStorageType = storage;
  return storage;
}

StructType* LLVMTypeResolver::getEnumVariantStruct(
    const sun::EnumType& enumType, const std::string& variantName) {
  auto it = enumType.cachedVariantStructs.find(variantName);
  if (it != enumType.cachedVariantStructs.end()) return it->second;

  const sun::EnumVariant* variant = enumType.getVariant(variantName);
  assert(variant && variant->hasPayload() &&
         "variant struct requested for unknown or unit variant");

  // Payloads must start at the enum's unit boundary (the storage struct is
  // { i32 tag, [N x unit] }): a small payload placed in the tag's alignment
  // padding would live outside the storage aggregate's fields and be lost
  // by aggregate load/store moves. Pad the tag up to the enum-wide max
  // payload alignment first.
  Align maxAlign(4);
  if (dataLayout) {
    for (const auto& v : enumType.getVariants()) {
      for (const auto& pt : v.payloadTypes) {
        maxAlign = std::max(maxAlign, dataLayout->getABITypeAlign(resolve(pt)));
      }
    }
  }
  const uint64_t tagArea = alignTo(4, maxAlign.value());

  std::vector<Type*> fields;
  fields.push_back(Type::getInt32Ty(ctx));  // tag
  if (tagArea > 4) {
    fields.push_back(ArrayType::get(Type::getInt8Ty(ctx), tagArea - 4));
  }
  for (const auto& payloadType : variant->payloadTypes) {
    fields.push_back(resolve(payloadType));
  }
  auto* variantStruct = StructType::create(
      ctx, fields, enumType.getName() + "_" + variantName + "_struct");
  enumType.cachedVariantStructs[variantName] = variantStruct;
  return variantStruct;
}

// -----------------------------------------------------------------------------
// Type resolution
// -----------------------------------------------------------------------------

Type* LLVMTypeResolver::resolve(const sun::TypePtr& type) {
  if (!type) return nullptr;
  return resolve(*type);
}

Type* LLVMTypeResolver::resolve(const sun::Type& type) {
  // Check cache first
  auto* typePtr = const_cast<sun::Type*>(&type);
  auto cacheIt = typeCache.find(typePtr);
  if (cacheIt != typeCache.end()) {
    return cacheIt->second;
  }

  Type* result = nullptr;

  switch (type.getKind()) {
    // Primitive types - all use their built-in toLLVMType
    case sun::Type::Kind::Void:
    case sun::Type::Kind::Bool:
    case sun::Type::Kind::Int8:
    case sun::Type::Kind::Int16:
    case sun::Type::Kind::Int32:
    case sun::Type::Kind::Int64:
    case sun::Type::Kind::UInt8:
    case sun::Type::Kind::UInt16:
    case sun::Type::Kind::UInt32:
    case sun::Type::Kind::UInt64:
    case sun::Type::Kind::Float32:
    case sun::Type::Kind::Float64:
    case sun::Type::Kind::Char: {
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::Type::Kind::Function: {
      // Named function type: stored as a direct function pointer
      result = PointerType::getUnqual(ctx);
      break;
    }

    case sun::Type::Kind::Lambda: {
      // Lambda type: stored as a closure struct { ptr, ptr }
      result = getClosureType();
      break;
    }

    case sun::Type::Kind::RawPointer: {
      // Raw pointer is stored as a regular pointer
      result = PointerType::getUnqual(ctx);
      break;
    }

    case sun::Type::Kind::StaticPointer: {
      // Static pointer is a fat pointer struct { ptr data, i64 length }
      // Use shared type for LLVM type equality across module
      result = getStaticPtrType();
      break;
    }

    case sun::Type::Kind::Reference: {
      // Reference is stored as a pointer (passed by address)
      result = PointerType::getUnqual(ctx);
      break;
    }

    case sun::Type::Kind::Class: {
      // Class instances are value types represented as structs. Payload-enum
      // fields embed their storage struct, which needs the DataLayout — build
      // those first so ClassType::getStructType can serve them from cache.
      prepareEnumFieldStorage(static_cast<const sun::ClassType&>(type));
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::Type::Kind::Interface: {
      // Interface values are fat pointers: { ptr data, ptr vtable }
      // This enables dynamic dispatch via vtable lookup
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::Type::Kind::Enum: {
      // Payload-free enums are i32 values; payload enums are tagged unions
      const auto& enumType = static_cast<const sun::EnumType&>(type);
      if (enumType.hasPayload()) {
        result = getEnumStorageType(enumType);
      } else {
        result = llvm::Type::getInt32Ty(ctx);
      }
      break;
    }

    case sun::Type::Kind::NullPointer: {
      // Null pointer literal resolves to opaque pointer
      result = PointerType::getUnqual(ctx);
      break;
    }

    case sun::Type::Kind::ErrorUnion: {
      // Error union uses its built-in toLLVMType (creates struct type)
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::Type::Kind::Array: {
      // Fixed-size array uses its built-in toLLVMType
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::Type::Kind::Slice: {
      // Slice type: { i64 start, i64 end }
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::Type::Kind::TypeParameter: {
      // Type parameters should be substituted before codegen
      // This is an error condition - return nullptr
      result = nullptr;
      break;
    }

    case sun::Type::Kind::Module: {
      // A module name is only ever the left side of `m.item`; it has no
      // runtime value
      result = nullptr;
      break;
    }
  }

  // Cache the result
  if (result) {
    typeCache[typePtr] = result;
  }

  return result;
}

// -----------------------------------------------------------------------------
// Function type helpers
// -----------------------------------------------------------------------------

Type* LLVMTypeResolver::resolveReturnType(const sun::FunctionType& funcType) {
  const auto& retType = funcType.getReturnType();
  return resolve(retType);
}

std::vector<Type*> LLVMTypeResolver::resolveParamTypes(
    const sun::FunctionType& funcType) {
  std::vector<Type*> result;
  for (const auto& param : funcType.getParamTypes()) {
    result.push_back(resolve(param));
  }
  return result;
}

FunctionType* LLVMTypeResolver::resolveFunctionSignature(
    const sun::FunctionType& funcType) {
  // Build parameter types: first is hidden closure pointer, then user params
  std::vector<Type*> paramTypes;
  paramTypes.push_back(PointerType::getUnqual(ctx));  // Hidden closure ptr

  for (const auto& param : funcType.getParamTypes()) {
    paramTypes.push_back(resolve(param));
  }

  // Resolve return type
  Type* returnType = resolveReturnType(funcType);

  return FunctionType::get(returnType, paramTypes, false);
}

FunctionType* LLVMTypeResolver::resolveDirectFunctionSignature(
    const sun::FunctionType& funcType) {
  // Build parameter types: NO hidden closure pointer, just user params
  std::vector<Type*> paramTypes;

  for (const auto& param : funcType.getParamTypes()) {
    paramTypes.push_back(resolve(param));
  }

  // Resolve return type
  Type* returnType = resolveReturnType(funcType);

  return FunctionType::get(returnType, paramTypes, false);
}

// -----------------------------------------------------------------------------
// Lambda type helpers
// -----------------------------------------------------------------------------

Type* LLVMTypeResolver::resolveReturnType(const sun::LambdaType& lambdaType) {
  const auto& retType = lambdaType.getReturnType();
  return resolve(retType);
}

std::vector<Type*> LLVMTypeResolver::resolveParamTypes(
    const sun::LambdaType& lambdaType) {
  std::vector<Type*> result;
  for (const auto& param : lambdaType.getParamTypes()) {
    result.push_back(resolve(param));
  }
  return result;
}

FunctionType* LLVMTypeResolver::resolveLambdaSignature(
    const sun::LambdaType& lambdaType) {
  // Build parameter types: first is hidden fat pointer, then user params
  std::vector<Type*> paramTypes;
  paramTypes.push_back(PointerType::getUnqual(ctx));  // Hidden fat ptr

  for (const auto& param : lambdaType.getParamTypes()) {
    paramTypes.push_back(resolve(param));
  }

  // Resolve return type
  Type* returnType = resolveReturnType(lambdaType);

  return FunctionType::get(returnType, paramTypes, false);
}

// -----------------------------------------------------------------------------
// Return type resolution (for function return values)
// -----------------------------------------------------------------------------

Type* LLVMTypeResolver::resolveForReturn(const sun::TypePtr& type) {
  if (!type) return nullptr;
  return resolveForReturn(*type);
}

Type* LLVMTypeResolver::resolveForReturn(const sun::Type& type) {
  // Now that resolve() returns struct for class types and
  // ErrorUnionType::toLLVMType() embeds the correct struct type,
  // resolveForReturn is equivalent to resolve().
  return resolve(type);
}
