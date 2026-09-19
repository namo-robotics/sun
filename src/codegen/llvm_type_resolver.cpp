// llvm_type_resolver.cpp — Implementation of sun::semantic_analysis::Type to
// llvm::Type resolution

#include "codegen/llvm_type_resolver.h"

#include "llvm/IR/DerivedTypes.h"

using sun::semantic_analysis::ClassType;
using sun::semantic_analysis::EnumType;
using sun::semantic_analysis::LambdaType;

using namespace llvm;

/** Translates analyzed Sun programs into LLVM instructions. */
namespace sun::codegen {

// -----------------------------------------------------------------------------
// Closure type management
// -----------------------------------------------------------------------------

StructType* LLVMTypeResolver::getClosureType() {
  if (!closureType) {
    closureType = StructType::create(ctx, sun::semantic_analysis::Closure);
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
    sun::semantic_analysis::StaticPointerType tempType(
        sun::semantic_analysis::Types::UInt8());
    staticPtrType = llvm::cast<StructType>(tempType.toLLVMType(ctx));
  }
  return staticPtrType;
}

// -----------------------------------------------------------------------------
// Payload enum layout
// -----------------------------------------------------------------------------

void LLVMTypeResolver::prepareEnumFieldStorage(const ClassType& classType) {
  for (const auto& field : classType.getFields()) {
    if (!field.type) continue;
    if (field.type->isEnum()) {
      const auto& enumType = static_cast<const EnumType&>(*field.type);
      if (enumType.hasPayload()) getEnumStorageType(enumType);
    } else if (field.type->isClass()) {
      prepareEnumFieldStorage(static_cast<const ClassType&>(*field.type));
    }
  }
}

StructType* LLVMTypeResolver::getEnumStorageType(const EnumType& enumType) {
  if (enumType.cachedStorageType) return enumType.cachedStorageType;
  if (!dataLayout) {
    sun::support::logAndThrowError(
        "payload enum '" + enumType.getDisplayName() +
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
  llvm::Type* unitTy = llvm::Type::getIntNTy(ctx, unitBytes * 8);
  const uint64_t tagArea = alignTo(4, unitBytes);
  const uint64_t payloadBytes = maxSize > tagArea ? maxSize - tagArea : 0;
  const uint64_t numUnits = (payloadBytes + unitBytes - 1) / unitBytes;

  // Share equal layouts without merging different same-named declarations.
  StructType* storage = StructType::get(
      ctx,
      {llvm::Type::getInt32Ty(ctx), llvm::ArrayType::get(unitTy, numUnits)});

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
    const EnumType& enumType, const std::string& variantName) {
  auto it = enumType.cachedVariantStructs.find(variantName);
  if (it != enumType.cachedVariantStructs.end()) return it->second;

  const sun::semantic_analysis::EnumVariant* variant =
      enumType.getVariant(variantName);
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

  std::vector<llvm::Type*> fields;
  fields.push_back(llvm::Type::getInt32Ty(ctx));  // tag
  if (tagArea > 4) {
    fields.push_back(
        llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx), tagArea - 4));
  }
  for (const auto& payloadType : variant->payloadTypes) {
    fields.push_back(resolve(payloadType));
  }
  auto* variantStruct = StructType::get(ctx, fields);
  enumType.cachedVariantStructs[variantName] = variantStruct;
  return variantStruct;
}

// -----------------------------------------------------------------------------
// Type resolution
// -----------------------------------------------------------------------------

llvm::Type* LLVMTypeResolver::resolve(
    const sun::semantic_analysis::TypePtr& type) {
  if (!type) return nullptr;
  return resolve(*type);
}

llvm::Type* LLVMTypeResolver::resolve(
    const sun::semantic_analysis::Type& type) {
  // Check cache first
  auto* typePtr = const_cast<sun::semantic_analysis::Type*>(&type);
  auto cacheIt = typeCache.find(typePtr);
  if (cacheIt != typeCache.end()) {
    return cacheIt->second;
  }

  llvm::Type* result = nullptr;

  switch (type.getKind()) {
    // Primitive types - all use their built-in toLLVMType
    case sun::semantic_analysis::Type::Kind::Void:
    case sun::semantic_analysis::Type::Kind::Bool:
    case sun::semantic_analysis::Type::Kind::Int8:
    case sun::semantic_analysis::Type::Kind::Int16:
    case sun::semantic_analysis::Type::Kind::Int32:
    case sun::semantic_analysis::Type::Kind::Int64:
    case sun::semantic_analysis::Type::Kind::UInt8:
    case sun::semantic_analysis::Type::Kind::UInt16:
    case sun::semantic_analysis::Type::Kind::UInt32:
    case sun::semantic_analysis::Type::Kind::UInt64:
    case sun::semantic_analysis::Type::Kind::Float32:
    case sun::semantic_analysis::Type::Kind::Float64:
    case sun::semantic_analysis::Type::Kind::Char: {
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::semantic_analysis::Type::Kind::Function: {
      // Named function type: stored as a direct function pointer
      result = PointerType::getUnqual(ctx);
      break;
    }

    case sun::semantic_analysis::Type::Kind::Lambda: {
      // Lambda type: stored as a closure struct { ptr, ptr }
      result = getClosureType();
      break;
    }

    case sun::semantic_analysis::Type::Kind::RawPointer: {
      // Raw pointer is stored as a regular pointer
      result = PointerType::getUnqual(ctx);
      break;
    }

    case sun::semantic_analysis::Type::Kind::StaticPointer: {
      // Static pointer is a fat pointer struct { ptr data, i64 length }
      // Use shared type for LLVM type equality across module
      result = getStaticPtrType();
      break;
    }

    case sun::semantic_analysis::Type::Kind::Reference: {
      // A reference is the referent's address; a `ref array<T>` to an
      // unsized array is the view struct itself
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::semantic_analysis::Type::Kind::Class: {
      // Class instances are value types represented as structs. Payload-enum
      // fields embed their storage struct, which needs the DataLayout — build
      // those first so ClassType::getStructType can serve them from cache.
      prepareEnumFieldStorage(static_cast<const ClassType&>(type));
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::semantic_analysis::Type::Kind::Interface: {
      // Interface values are fat pointers: { ptr data, ptr vtable }
      // This enables dynamic dispatch via vtable lookup
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::semantic_analysis::Type::Kind::Enum: {
      // Payload-free enums use their integer type; payload enums are tagged
      // unions
      const auto& enumType = static_cast<const EnumType&>(type);
      if (enumType.hasPayload()) {
        result = getEnumStorageType(enumType);
      } else {
        result = enumType.toLLVMType(ctx);
      }
      break;
    }

    case sun::semantic_analysis::Type::Kind::NullPointer: {
      // Null pointer literal resolves to opaque pointer
      result = PointerType::getUnqual(ctx);
      break;
    }

    case sun::semantic_analysis::Type::Kind::ErrorUnion: {
      // Error union uses its built-in toLLVMType (creates struct type)
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::semantic_analysis::Type::Kind::Array: {
      // Fixed-size array uses its built-in toLLVMType
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::semantic_analysis::Type::Kind::Slice: {
      // Slice type: { i64 start, i64 end }
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::semantic_analysis::Type::Kind::TypeParameter: {
      // Type parameters should be substituted before codegen
      // This is an error condition - return nullptr
      result = nullptr;
      break;
    }

    case sun::semantic_analysis::Type::Kind::Module: {
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

llvm::Type* LLVMTypeResolver::resolveReturnType(
    const sun::semantic_analysis::FunctionType& funcType) {
  const auto& retType = funcType.getReturnType();
  return resolve(retType);
}

std::vector<llvm::Type*> LLVMTypeResolver::resolveParamTypes(
    const sun::semantic_analysis::FunctionType& funcType) {
  std::vector<llvm::Type*> result;
  for (const auto& param : funcType.getParamTypes()) {
    result.push_back(resolve(param));
  }
  return result;
}

llvm::FunctionType* LLVMTypeResolver::resolveFunctionSignature(
    const sun::semantic_analysis::FunctionType& funcType) {
  // Build parameter types: first is hidden closure pointer, then user params
  std::vector<llvm::Type*> paramTypes;
  paramTypes.push_back(PointerType::getUnqual(ctx));  // Hidden closure ptr

  for (const auto& param : funcType.getParamTypes()) {
    paramTypes.push_back(resolve(param));
  }

  // Resolve return type
  llvm::Type* returnType = resolveReturnType(funcType);

  return llvm::FunctionType::get(returnType, paramTypes, false);
}

llvm::FunctionType* LLVMTypeResolver::resolveDirectFunctionSignature(
    const sun::semantic_analysis::FunctionType& funcType) {
  // Build parameter types: NO hidden closure pointer, just user params
  std::vector<llvm::Type*> paramTypes;

  for (const auto& param : funcType.getParamTypes()) {
    paramTypes.push_back(resolve(param));
  }

  // Resolve return type
  llvm::Type* returnType = resolveReturnType(funcType);

  return llvm::FunctionType::get(returnType, paramTypes, false);
}

// -----------------------------------------------------------------------------
// Lambda type helpers
// -----------------------------------------------------------------------------

llvm::Type* LLVMTypeResolver::resolveReturnType(const LambdaType& lambdaType) {
  const auto& retType = lambdaType.getReturnType();
  return resolve(retType);
}

std::vector<llvm::Type*> LLVMTypeResolver::resolveParamTypes(
    const LambdaType& lambdaType) {
  std::vector<llvm::Type*> result;
  for (const auto& param : lambdaType.getParamTypes()) {
    result.push_back(resolve(param));
  }
  return result;
}

llvm::FunctionType* LLVMTypeResolver::resolveLambdaSignature(
    const LambdaType& lambdaType) {
  // Build parameter types: first is hidden fat pointer, then user params
  std::vector<llvm::Type*> paramTypes;
  paramTypes.push_back(PointerType::getUnqual(ctx));  // Hidden fat ptr

  for (const auto& param : lambdaType.getParamTypes()) {
    paramTypes.push_back(resolve(param));
  }

  // Resolve return type
  llvm::Type* returnType = resolveReturnType(lambdaType);

  return llvm::FunctionType::get(returnType, paramTypes, false);
}

// -----------------------------------------------------------------------------
// Return type resolution (for function return values)
// -----------------------------------------------------------------------------

llvm::Type* LLVMTypeResolver::resolveForReturn(
    const sun::semantic_analysis::TypePtr& type) {
  if (!type) return nullptr;
  return resolveForReturn(*type);
}

llvm::Type* LLVMTypeResolver::resolveForReturn(
    const sun::semantic_analysis::Type& type) {
  // Now that resolve() returns struct for class types and
  // ErrorUnionType::toLLVMType() embeds the correct struct type,
  // resolveForReturn is equivalent to resolve().
  return resolve(type);
}

}  // namespace sun::codegen
