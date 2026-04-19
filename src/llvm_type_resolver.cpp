// llvm_type_resolver.cpp — Implementation of sun::Type to llvm::Type resolution

#include "llvm_type_resolver.h"

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
    // Check if the type already exists in the context (e.g., from linked code)
    staticPtrType = StructType::getTypeByName(ctx, sun::StructNames::StaticPtr);
    if (!staticPtrType) {
      staticPtrType = StructType::create(ctx, sun::StructNames::StaticPtr);
      staticPtrType->setBody({
          PointerType::getUnqual(ctx),  // data ptr
          Type::getInt64Ty(ctx)         // length
      });
    }
  }
  return staticPtrType;
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
    case sun::Type::Kind::Float64: {
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
      // Class instances are value types represented as structs
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
      // Enum values are stored as i32
      result = llvm::Type::getInt32Ty(ctx);
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
