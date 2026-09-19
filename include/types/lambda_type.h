/** Describes closure signatures and capture lifetimes. */
#pragma once

#include <vector>

#include "llvm/IR/DerivedTypes.h"
#include "types/type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/**
 * Lambda type for anonymous functions (fat pointer call, returnable)
 * Type annotation: () => {}
 */
class LambdaType : public Type {
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;
  bool requiresUnsafe_ = false;
  bool canThrow_ = false;  // declared with 'throws IError' — may unwind
  // Part of the type's identity: `<'_>() => T` in source. True when the
  // lambda carries a captured environment that lives in a stack frame
  // (capture lists, bound methods). A plain '() => T' is reserved for
  // environment-free lambdas, so this is what keeps a frame-bound lambda
  // from laundering through an annotation-typed parameter or field.
  bool hasRefCaptures_ = false;
  // Metadata, NOT identity: the lifetime name written on this position
  // ('<'a>(i32) => i32'), empty when elided. Names only mean something
  // relative to one signature's lifetime list, so equals(), toString() and
  // specialization identity ignore them - '<'a>' and '<'b>' are one type.
  std::string lifetimeName_;

 public:
  /** Creates a captured-callable type from its result, parameters, and
   * restrictions. */
  LambdaType(TypePtr ret, std::vector<TypePtr> params, bool canThrow = false,
             bool requiresUnsafe = false)
      : returnType(std::move(ret)),
        paramTypes(std::move(params)),
        requiresUnsafe_(requiresUnsafe),
        canThrow_(canThrow) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Lambda;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the semantic type of the function result. */
  const TypePtr& getReturnType() const { return returnType; }
  /** Provides the ordered semantic types of the function parameters. */
  const std::vector<TypePtr>& getParamTypes() const { return paramTypes; }
  /** Whether calling this value requires an unsafe block. */
  bool requiresUnsafe() const { return requiresUnsafe_; }
  /** Reports whether calls through this type may produce an error. */
  bool canThrow() const { return canThrow_; }
  /** Reports whether this object has ref captures. */
  bool hasRefCaptures() const { return hasRefCaptures_; }
  /** Records whether the callable retains any borrowed captures. */
  void setHasRefCaptures(bool v) { hasRefCaptures_ = v; }
  /** Returns the named lifetime associated with the borrowed value. */
  const std::string& getLifetimeName() const { return lifetimeName_; }
  /** Assigns the named lifetime associated with the borrowed value. */
  void setLifetimeName(std::string name) { lifetimeName_ = std::move(name); }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = hasRefCaptures_ ? "<'_>(" : "(";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toString();
    }
    result += ") => " + returnType->toString();
    if (requiresUnsafe_) result = "unsafe " + result;
    if (canThrow_) result += " throws IError";
    return result;
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override {
    std::string result = hasRefCaptures_ ? "<'_>(" : "(";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += paramTypes[i]->toDisplayString();
    }
    result += ") => " + returnType->toDisplayString();
    if (requiresUnsafe_) result = "unsafe " + result;
    if (canThrow_) result += " throws IError";
    return result;
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* l = dynamic_cast<const LambdaType*>(&other)) {
      if (requiresUnsafe_ != l->requiresUnsafe_) return false;
      if (canThrow_ != l->canThrow_) return false;
      if (hasRefCaptures_ != l->hasRefCaptures_) return false;
      if (!returnType->equals(*l->returnType)) return false;
      if (paramTypes.size() != l->paramTypes.size()) return false;
      for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (!paramTypes[i]->equals(*l->paramTypes[i])) return false;
      }
      return true;
    }
    return false;
  }

  /**
   * Same signature ignoring throwing-ness and the lifetime marker. Used by the
   * bound-method overload chooser, which picks a method by shape before the
   * chosen value is flagged as frame-bound; assignability rejects it later.
   */
  bool equalsIgnoringThrow(const LambdaType& other) const {
    if (!returnType->equals(*other.returnType)) return false;
    if (paramTypes.size() != other.paramTypes.size()) return false;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
      if (!paramTypes[i]->equals(*other.paramTypes[i])) return false;
    }
    return true;
  }

  /**
   * Can a by-value 'from' argument bind to a parameter of this type?
   * Same signature, and each marker only widens: a non-throwing lambda may
   * go where a throwing one is expected, and an environment-free lambda may
   * go where a '<'_>' one is expected — never the other way around.
   */
  bool acceptsValueOf(const LambdaType& from) const {
    if (!equalsIgnoringThrow(from)) return false;
    if (!requiresUnsafe_ && from.requiresUnsafe_) return false;
    if (!canThrow_ && from.canThrow_) return false;
    if (!hasRefCaptures_ && from.hasRefCaptures_) return false;
    return true;
  }

  /**
   * Returns the closure struct type { ptr, ptr } (func*, env*)
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return llvm::StructType::get(
        ctx, {
                 llvm::PointerType::getUnqual(ctx),  // func*
                 llvm::PointerType::getUnqual(ctx)   // env*
             });
  }

  /**
   * Get the raw LLVM FunctionType (for the actual function signature with
   * closure param)
   */
  llvm::FunctionType* toLLVMFunctionType(llvm::LLVMContext& ctx) const {
    std::vector<llvm::Type*> llvmParams;
    // First param is the fat pointer (closure struct pointer)
    llvmParams.push_back(llvm::PointerType::getUnqual(ctx));
    for (const auto& p : paramTypes) {
      llvmParams.push_back(p->toLLVMType(ctx));
    }
    return llvm::FunctionType::get(returnType->toLLVMType(ctx), llvmParams,
                                   false);
  }
};

}  // namespace sun::types
