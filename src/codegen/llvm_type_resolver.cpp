// llvm_type_resolver.cpp — Implementation of sun::types::Type to
// llvm::Type resolution

#include "codegen/llvm_type_resolver.h"

#include "abi/lowering_descriptors.h"
#include "llvm/IR/DerivedTypes.h"

using sun::types::ClassType;
using sun::types::EnumType;
using sun::types::LambdaType;

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
    sun::types::StaticPointerType tempType(sun::types::Types::UInt8());
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

  std::vector<std::vector<llvm::Type*>> payloads;
  for (const auto& variant : enumType.getVariants()) {
    std::vector<llvm::Type*> fields;
    for (const auto& type : variant.payloadTypes)
      fields.push_back(type->isReference() &&
                               sun::types::unwrapRef(type)->isInterface()
                           ? sun::types::InterfaceType::getFatPointerType(ctx)
                           : resolve(type));
    payloads.push_back(std::move(fields));
  }
  const auto descriptor =
      sun::abi::EnumLayout::build(ctx, *dataLayout, payloads);
  for (size_t i = 0; i < payloads.size(); ++i)
    enumType.cachedVariantStructs[enumType.getVariants()[i].name] =
        descriptor.variants[i];
  enumType.cachedStorageType = descriptor.storage;
  return descriptor.storage;
}

StructType* LLVMTypeResolver::getEnumVariantStruct(
    const EnumType& enumType, const std::string& variantName) {
  getEnumStorageType(enumType);
  const auto found = enumType.cachedVariantStructs.find(variantName);
  assert(found != enumType.cachedVariantStructs.end() &&
         "variant struct requested for unknown variant");
  return found->second;
}

// -----------------------------------------------------------------------------
// Type resolution
// -----------------------------------------------------------------------------

llvm::Type* LLVMTypeResolver::resolve(const sun::types::TypePtr& type) {
  if (!type) return nullptr;
  return resolve(*type);
}

llvm::Type* LLVMTypeResolver::resolve(const sun::types::Type& type) {
  // Check cache first
  auto* typePtr = const_cast<sun::types::Type*>(&type);
  auto cacheIt = typeCache.find(typePtr);
  if (cacheIt != typeCache.end()) {
    return cacheIt->second;
  }

  llvm::Type* result = nullptr;

  switch (type.getKind()) {
    // Primitive types - all use their built-in toLLVMType
    case sun::types::Type::Kind::Void:
    case sun::types::Type::Kind::Bool:
    case sun::types::Type::Kind::Int8:
    case sun::types::Type::Kind::Int16:
    case sun::types::Type::Kind::Int32:
    case sun::types::Type::Kind::Int64:
    case sun::types::Type::Kind::UInt8:
    case sun::types::Type::Kind::UInt16:
    case sun::types::Type::Kind::UInt32:
    case sun::types::Type::Kind::UInt64:
    case sun::types::Type::Kind::Float32:
    case sun::types::Type::Kind::Float64:
    case sun::types::Type::Kind::Char: {
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::types::Type::Kind::Function: {
      // Named function type: stored as a direct function pointer
      result = PointerType::getUnqual(ctx);
      break;
    }

    case sun::types::Type::Kind::Lambda: {
      // Lambda type: stored as a closure struct { ptr, ptr }
      result = getClosureType();
      break;
    }

    case sun::types::Type::Kind::RawPointer: {
      // Raw pointer is stored as a regular pointer
      result = PointerType::getUnqual(ctx);
      break;
    }

    case sun::types::Type::Kind::StaticPointer: {
      // Static pointer is a fat pointer struct { ptr data, i64 length }
      // Use shared type for LLVM type equality across module
      result = getStaticPtrType();
      break;
    }

    case sun::types::Type::Kind::Reference: {
      // A reference is the referent's address; a `ref array<T>` to an
      // unsized array is the view struct itself
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::types::Type::Kind::Class: {
      // Class instances are value types represented as structs. Payload-enum
      // fields embed their storage struct, which needs the DataLayout — build
      // those first so ClassType::getStructType can serve them from cache.
      prepareEnumFieldStorage(static_cast<const ClassType&>(type));
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::types::Type::Kind::Interface: {
      // Interface values are fat pointers: { ptr data, ptr vtable }
      // This enables dynamic dispatch via vtable lookup
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::types::Type::Kind::Enum: {
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

    case sun::types::Type::Kind::NullPointer: {
      // Null pointer literal resolves to opaque pointer
      result = PointerType::getUnqual(ctx);
      break;
    }

    case sun::types::Type::Kind::ErrorUnion: {
      // Error union uses its built-in toLLVMType (creates struct type)
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::types::Type::Kind::Array: {
      // Fixed-size array uses its built-in toLLVMType
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::types::Type::Kind::Slice: {
      // Slice type: { i64 start, i64 end }
      result = type.toLLVMType(ctx);
      break;
    }

    case sun::types::Type::Kind::TypeParameter: {
      // Type parameters should be substituted before codegen
      // This is an error condition - return nullptr
      result = nullptr;
      break;
    }

    case sun::types::Type::Kind::Module: {
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
    const sun::types::FunctionType& funcType) {
  const auto& retType = funcType.getReturnType();
  return resolveForReturn(retType);
}

std::vector<llvm::Type*> LLVMTypeResolver::resolveParamTypes(
    const sun::types::FunctionType& funcType) {
  std::vector<llvm::Type*> result;
  for (const auto& param : funcType.getParamTypes()) {
    result.push_back(resolve(param));
  }
  return result;
}

llvm::FunctionType* LLVMTypeResolver::resolveFunctionSignature(
    const sun::types::FunctionType& funcType) {
  return sun::abi::CallDescriptor::sun(resolveReturnType(funcType),
                                       resolveParamTypes(funcType), true)
      .signature;
}

llvm::FunctionType* LLVMTypeResolver::resolveDirectFunctionSignature(
    const sun::types::FunctionType& funcType) {
  return sun::abi::CallDescriptor::sun(resolveReturnType(funcType),
                                       resolveParamTypes(funcType))
      .signature;
}

// -----------------------------------------------------------------------------
// Lambda type helpers
// -----------------------------------------------------------------------------

llvm::Type* LLVMTypeResolver::resolveReturnType(const LambdaType& lambdaType) {
  const auto& retType = lambdaType.getReturnType();
  return resolveForReturn(retType);
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
  return sun::abi::CallDescriptor::sun(resolveReturnType(lambdaType),
                                       resolveParamTypes(lambdaType), true)
      .signature;
}

// -----------------------------------------------------------------------------
// Return type resolution (for function return values)
// -----------------------------------------------------------------------------

llvm::Type* LLVMTypeResolver::resolveForReturn(
    const sun::types::TypePtr& type) {
  if (!type) return nullptr;
  return resolveForReturn(*type);
}

llvm::Type* LLVMTypeResolver::resolveForReturn(const sun::types::Type& type) {
  // Return an interface borrow as a view value; its storage belongs to the
  // caller, so a view created during an upcast never escapes a callee frame.
  if (auto* reference = dynamic_cast<const sun::types::ReferenceType*>(&type)) {
    if (reference->getReferencedType()->isInterface())
      return sun::types::InterfaceType::getFatPointerType(ctx);
  }
  return resolve(type);
}

}  // namespace sun::codegen
