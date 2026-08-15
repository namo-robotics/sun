// llvm_type_resolver.h — Resolves sun::Type to llvm::Type for codegen
//
// This pass runs after semantic analysis and before codegen.
// It creates a mapping from sun::Type to the appropriate llvm::Type,
// handling special cases like function types becoming closure structs.

#pragma once

#include <map>

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "struct_names.h"
#include "types.h"

/**
 * LLVMTypeResolver converts sun::Type to llvm::Type with proper handling of:
 * - Primitive types (i32 -> i32, f64 -> double, etc.)
 * - Function types -> closure struct { ptr, ptr }
 * - Function and lambda types
 *
 * It maintains a shared closure type to ensure type equality across the module.
 */
class LLVMTypeResolver {
  llvm::LLVMContext& ctx;

  // Module DataLayout; required for payload-enum storage sizing. Null only
  // in contexts that never resolve payload enums.
  const llvm::DataLayout* dataLayout = nullptr;

  // Shared closure type { ptr, ptr } for all functions
  llvm::StructType* closureType = nullptr;

  // Shared static pointer type { ptr, i64 } for static_ptr<T>
  llvm::StructType* staticPtrType = nullptr;

  // Cache of resolved types to avoid recreating them
  std::map<sun::Type*, llvm::Type*> typeCache;

 public:
  explicit LLVMTypeResolver(llvm::LLVMContext& context,
                            const llvm::DataLayout* dl = nullptr)
      : ctx(context), dataLayout(dl) {}

  /**
   * Storage struct for a payload enum: { i32 tag, [M x unitTy] } where unitTy
   * matches the max payload alignment, so LLVM's natural layout aligns the
   * payload area. Named "<QualifiedEnum>_struct" (not a well-known internal
   * struct, so materializeStructReturn treats returns like class values).
   */
  llvm::StructType* getEnumStorageType(const sun::EnumType& enumType);

  /**
   * Per-variant view struct { i32 tag, T1, T2, ... }. Construction and
   * pattern extraction GEP through this on the storage pointer, so field
   * offsets are always consistent and naturally aligned.
   */
  // Field index of payload `i` in a variant view struct (field 0 is the
  // tag; a padding field may follow it — see getEnumVariantStruct)
  unsigned enumPayloadFieldIndex(const sun::EnumType& enumType,
                                 const std::string& variantName, size_t i) {
    llvm::StructType* vt = getEnumVariantStruct(enumType, variantName);
    const sun::EnumVariant* v = enumType.getVariant(variantName);
    unsigned base = vt->getNumElements() - static_cast<unsigned>(v->payloadTypes.size());
    return base + static_cast<unsigned>(i);
  }

  llvm::StructType* getEnumVariantStruct(const sun::EnumType& enumType,
                                         const std::string& variantName);

  /**
   * Build storage structs for payload-enum fields (transitively) so
   * ClassType::getStructType can embed them from the enum's cache.
   */
  void prepareEnumFieldStorage(const sun::ClassType& classType);

  /**
   * Get or create the shared closure struct type { ptr, ptr }.
   * All function types use this same struct for type equality.
   */
  llvm::StructType* getClosureType();

  /**
   * Get or create the shared static pointer struct type { ptr, i64 }.
   * Used for static_ptr<T> types.
   */
  llvm::StructType* getStaticPtrType();

  /**
   * Resolve a sun::Type to its corresponding llvm::Type.
   * - Function types become the closure struct type
   * - Primitives map directly
   * - Class types become LLVM struct types
   */
  llvm::Type* resolve(const sun::Type& type);
  llvm::Type* resolve(const sun::TypePtr& type);

  /**
   * Resolve a function type's return type.
   * If the return type is itself a function, returns closure type.
   */
  llvm::Type* resolveReturnType(const sun::FunctionType& funcType);

  /**
   * Resolve function parameter types.
   * Function parameters become closure struct types.
   */
  std::vector<llvm::Type*> resolveParamTypes(const sun::FunctionType& funcType);

  /**
   * Get the LLVM function type for a sun::FunctionType with closure.
   * This includes the hidden closure pointer as the first parameter.
   * Signature: (ptr, user_params...) -> return_type
   */
  llvm::FunctionType* resolveFunctionSignature(
      const sun::FunctionType& funcType);

  /**
   * Get the LLVM function type for a sun::FunctionType WITHOUT closure.
   * This is a direct function with no hidden parameter.
   * Signature: (user_params...) -> return_type
   */
  llvm::FunctionType* resolveDirectFunctionSignature(
      const sun::FunctionType& funcType);

  /**
   * Get the LLVM function type for a sun::LambdaType.
   * This includes the hidden fat pointer as the first parameter.
   * Signature: (ptr, user_params...) -> return_type
   */
  llvm::FunctionType* resolveLambdaSignature(const sun::LambdaType& lambdaType);

  /**
   * Resolve a lambda type's return type.
   */
  llvm::Type* resolveReturnType(const sun::LambdaType& lambdaType);

  /**
   * Resolve lambda parameter types.
   */
  std::vector<llvm::Type*> resolveParamTypes(const sun::LambdaType& lambdaType);

  /**
   * Resolve a type for use as a function return type.
   * For compound types (classes), this returns the struct type by value
   * rather than a pointer, enabling proper return-by-value semantics.
   */
  llvm::Type* resolveForReturn(const sun::TypePtr& type);
  llvm::Type* resolveForReturn(const sun::Type& type);

  /**
   * Check if a sun::Type is a function type (named function, direct call).
   */
  static bool isFunctionType(const sun::Type& type) {
    return type.getKind() == sun::Type::Kind::Function;
  }

  static bool isFunctionType(const sun::TypePtr& type) {
    return type && type->getKind() == sun::Type::Kind::Function;
  }

  /**
   * Check if a sun::Type is a lambda type (anonymous function, fat pointer
   * call).
   */
  static bool isLambdaType(const sun::Type& type) {
    return type.getKind() == sun::Type::Kind::Lambda;
  }

  static bool isLambdaType(const sun::TypePtr& type) {
    return type && type->getKind() == sun::Type::Kind::Lambda;
  }

  /**
   * Check if a sun::Type is callable (either function or lambda).
   */
  static bool isCallable(const sun::Type& type) {
    return isFunctionType(type) || isLambdaType(type);
  }

  static bool isCallable(const sun::TypePtr& type) {
    return type && (isFunctionType(type) || isLambdaType(type));
  }
};
