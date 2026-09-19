/** Describes generic type parameters and deferred projections. */
#pragma once

#include <cassert>

#include "ast/type_constraint.h"
#include "semantic_analysis/declaration_id.h"
#include "types/type.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
using sun::ast::TypeConstraint;
using sun::semantic_analysis::DeclarationId;

/**
 * Something computed from a type parameter rather than the parameter itself.
 * `_return_type_of<F>` names a type that is only known once F is, so until
 * then it travels as the parameter F plus the projection to apply to it.
 */
enum class TypeProjection : uint8_t {
  None,        // the parameter itself
  ReturnType,  // _return_type_of<F>: what F returns
};

/**
 * Type parameter (used in generic class/function definitions)
 * Represents a type variable like T, U, V in class List&lt;T&gt;
 */
class TypeParameterType : public Type {
  std::string name;   // Parameter name: T, U, etc. (F$ret when projected)
  std::string base_;  // The parameter the projection applies to
  TypeProjection projection_ = TypeProjection::None;
  DeclarationId declaration_;
  std::shared_ptr<const int> session_;
  // What `<T: Trait>` promised about whatever T stands for. Metadata only —
  // intentionally excluded from equals()/toString() so it never disturbs
  // substitution or identity. It travels with the parameter so a body being
  // analyzed with T still standing for itself can see what T is known to be.
  TypeConstraint constraint_;

 public:
  /** Creates a generic type parameter retaining its constraint and declaration
   * identity. */
  explicit TypeParameterType(std::string paramName,
                             TypeConstraint constraint = {},
                             TypeProjection projection = TypeProjection::None,
                             DeclarationId declaration = {},
                             std::shared_ptr<const int> session = {})
      : name(paramName),
        base_(std::move(paramName)),
        projection_(projection),
        declaration_(declaration),
        session_(std::move(session)),
        constraint_(std::move(constraint)) {
    // Give projected parameters a distinct diagnostic spelling.
    if (projection_ == TypeProjection::ReturnType) name = base_ + "$ret";
  }

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::TypeParameter;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name; }
  /** Returns the projection base stored by this object. */
  const std::string& getProjectionBase() const { return base_; }
  /** Returns the projection stored by this object. */
  TypeProjection getProjection() const { return projection_; }
  /** The complete requirement carried by this parameter. */
  const TypeConstraint& getConstraint() const { return constraint_; }
  /** Reports whether this object has constraint. */
  bool hasConstraint() const { return !constraint_.name.empty(); }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return name; }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override {
    return projection_ == TypeProjection::ReturnType
               ? "_return_type_of<" + base_ + ">"
               : name;
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* p = dynamic_cast<const TypeParameterType*>(&other)) {
      if (!declaration_ || !p->declaration_) return this == p;
      return declaration_ == p->declaration_ && session_ == p->session_ &&
             projection_ == p->projection_;
    }
    return false;
  }

  /** Retain the binder identity when projecting its eventual concrete type. */
  TypePtr project(TypeProjection projection) const {
    return std::make_shared<TypeParameterType>(base_, constraint_, projection,
                                               declaration_, session_);
  }

  /**
   * Type parameters can't be directly converted to LLVM types
   * They must be substituted first
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    // This should never be called for unsubstituted type parameters
    assert(false && "Cannot convert type parameter to LLVM type");
    return nullptr;
  }
};

}  // namespace sun::types
