/** Describes the primitive scalar and void types. */
#pragma once

#include "llvm/IR/DerivedTypes.h"
#include "types/type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/**
 * Primitive types (int, float, bool, etc.)
 */
class PrimitiveType : public Type {
  Kind kind;

 public:
  /** Creates the semantic descriptor for a primitive type category. */
  explicit PrimitiveType(Kind k) : kind(k) {}

  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return kind; }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    switch (kind) {
      case Kind::Void:
        return "void";
      case Kind::Bool:
        return "bool";
      case Kind::Int8:
        return "i8";
      case Kind::Int16:
        return "i16";
      case Kind::Int32:
        return "i32";
      case Kind::Int64:
        return "i64";
      case Kind::UInt8:
        return "u8";
      case Kind::UInt16:
        return "u16";
      case Kind::UInt32:
        return "u32";
      case Kind::UInt64:
        return "u64";
      case Kind::Float32:
        return "f32";
      case Kind::Float64:
        return "f64";
      case Kind::Char:
        return "char";
      default:
        return "unknown";
    }
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    return kind == other.getKind();
  }

  /** Returns the LLVM type used to represent values of this semantic type. */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    switch (kind) {
      case Kind::Void:
        return llvm::Type::getVoidTy(ctx);
      case Kind::Bool:
        return llvm::Type::getInt1Ty(ctx);
      case Kind::Int8:
        return llvm::Type::getInt8Ty(ctx);
      case Kind::Int16:
        return llvm::Type::getInt16Ty(ctx);
      case Kind::Int32:
        return llvm::Type::getInt32Ty(ctx);
      case Kind::Int64:
        return llvm::Type::getInt64Ty(ctx);
      case Kind::UInt8:
        return llvm::Type::getInt8Ty(
            ctx);  // Same LLVM type, signedness is semantic
      case Kind::UInt16:
        return llvm::Type::getInt16Ty(ctx);
      case Kind::UInt32:
        return llvm::Type::getInt32Ty(ctx);
      case Kind::UInt64:
        return llvm::Type::getInt64Ty(ctx);
      case Kind::Float32:
        return llvm::Type::getFloatTy(ctx);
      case Kind::Float64:
        return llvm::Type::getDoubleTy(ctx);
      case Kind::Char:
        return llvm::Type::getInt32Ty(ctx);  // a Unicode scalar value
      default:
        return nullptr;
    }
  }
};

}  // namespace sun::types
