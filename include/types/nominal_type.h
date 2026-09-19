/** Defines declaration identity shared by class, interface, and enum types. */
#pragma once

#include "semantic_analysis/declaration_table.h"
#include "support/error.h"
#include "types/type.h"

/** Owns registration of nominal types during semantic analysis. */
namespace sun::semantic_analysis {
class TypeRegistry;
}

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
using sun::semantic_analysis::DeclarationId;
using sun::semantic_analysis::DeclarationTable;
using sun::support::logAndThrowError;

/** A nominal type's identity within its analysis session. */
class NominalType : public Type {
  friend class sun::semantic_analysis::TypeRegistry;
  DeclarationId declarationId_;
  std::shared_ptr<const int> declarationSession_;

 protected:
  /** Reports whether both nominal types belong to the same analysis session. */
  bool sameSession(const NominalType& other) const {
    return declarationSession_ == other.declarationSession_;
  }

  /** Reports whether both nominal types refer to the same declaration identity.
   */
  bool sameDeclaration(const NominalType& other) const {
    if (!declarationId_ || !other.declarationId_)
      logAndThrowError("Nominal equality requires declaration identities");
    return declarationId_ == other.declarationId_ &&
           declarationSession_ == other.declarationSession_;
  }

 public:
  /** Return the declaration identity within this analysis session. */
  DeclarationId getDeclarationId() const { return declarationId_; }
  /** Return the original template, or this declaration for a source type. */
  DeclarationId sourceDeclaration(const DeclarationTable& table) const;
  /** Check that a boundary is using this nominal type's owning session. */
  bool belongsTo(const DeclarationTable& table) const {
    return declarationSession_ == table.session();
  }
};

}  // namespace sun::types
