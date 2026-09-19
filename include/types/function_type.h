/** Describes callable function-pointer signatures. */
#pragma once

#include <vector>

#include "llvm/IR/DerivedTypes.h"
#include "types/type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/**
 * A non-null, one-word pointer to a module-scope function.
 * Type annotation: function (Args) Result
 */
class FunctionType : public Type {
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;
  bool requiresUnsafe_ = false;
  bool canThrow_ = false;  // declared with 'throws IError' — may unwind

 public:
  /** Creates a function type from its result, parameters, and calling
   * restrictions. */
  FunctionType(TypePtr ret, std::vector<TypePtr> params, bool canThrow = false,
               bool requiresUnsafe = false)
      : returnType(std::move(ret)),
        paramTypes(std::move(params)),
        requiresUnsafe_(requiresUnsafe),
        canThrow_(canThrow) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Function;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the semantic type of the function result. */
  const TypePtr& getReturnType() const { return returnType; }
  /** Provides the ordered semantic types of the function parameters. */
  const std::vector<TypePtr>& getParamTypes() const { return paramTypes; }

  /** Whether calling this value requires an unsafe block. */
  bool requiresUnsafe() const { return requiresUnsafe_; }
  /**
   * Whether calls through this pointer may throw.
   */
  bool canThrow() const { return canThrow_; }
  /** Records whether calls through this signature may throw an error. */
  void setCanThrow(bool v) { canThrow_ = v; }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = "function (";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toString();
    }
    result += ") " + returnType->toString();
    if (requiresUnsafe_) result = "unsafe " + result;
    if (canThrow_) result += " throws IError";
    return result;
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override {
    std::string result = "function (";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toDisplayString();
    }
    result += ") " + returnType->toDisplayString();
    if (requiresUnsafe_) result = "unsafe " + result;
    if (canThrow_) result += " throws IError";
    return result;
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* f = dynamic_cast<const FunctionType*>(&other)) {
      if (requiresUnsafe_ != f->requiresUnsafe_) return false;
      if (canThrow_ != f->canThrow_) return false;
      if (!returnType->equals(*f->returnType)) return false;
      if (paramTypes.size() != f->paramTypes.size()) return false;
      for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (!paramTypes[i]->equals(*f->paramTypes[i])) return false;
      }
      return true;
    }
    return false;
  }

  /**
   * Function values use LLVM's opaque pointer representation.
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return llvm::PointerType::getUnqual(ctx);
  }

  /**
   * Returns a pointer to the function type (for function pointer variables)
   */
  llvm::Type* toPointerType(llvm::LLVMContext& ctx) const {
    return toLLVMType(ctx);
  }

  /**
   * Get the raw LLVM FunctionType (for indirect calls)
   */
  llvm::FunctionType* toLLVMFunctionType(llvm::LLVMContext& ctx) const {
    std::vector<llvm::Type*> llvmParams;
    for (const auto& p : paramTypes) {
      llvmParams.push_back(p->toLLVMType(ctx));
    }
    return llvm::FunctionType::get(returnType->toLLVMType(ctx), llvmParams,
                                   false);
  }

  /**
   * Get the closure struct type { ptr, ptr } (func*, env*)
   */
  llvm::StructType* toLLVMClosureType(llvm::LLVMContext& ctx) const {
    return llvm::StructType::get(
        ctx, {
                 llvm::PointerType::getUnqual(ctx),  // func*
                 llvm::PointerType::getUnqual(ctx)   // env*
             });
  }
};

}  // namespace sun::types
