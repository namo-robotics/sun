/** Describes enum variants, payloads, and representation metadata. */
#pragma once

#include <unordered_map>

#include "llvm/IR/DerivedTypes.h"
#include "semantic_analysis/qualified_name.h"
#include "semantic_analysis/visibility.h"
#include "types/nominal_type.h"
#include "types/primitive_type.h"
#include "types/type_utils.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
/**
 * Enum variant information
 */
struct EnumVariant {
  std::string name;
  int64_t value;  // Tag bits; the enum representation determines signedness
  std::vector<TypePtr> payloadTypes;  // empty = unit variant
  DeclarationId declarationId;

  /** Reports whether this object has payload. */
  bool hasPayload() const { return !payloadTypes.empty(); }
};

// Forward declaration for EnumType
class EnumType;
/** Shared ownership of a semantic enum description. */
using EnumTypePtr = std::shared_ptr<EnumType>;

/**
 * Enum type for user-defined enums
 * Enums use an integer representation, with variants as named constants
 * Example: enum Color { Red, Green, Blue }
 */
class EnumType : public NominalType {
  friend class sun::semantic_analysis::TypeRegistry;
  std::string
      name_;  // Source-qualified spelling for diagnostic type signatures.
  std::string baseName_;  // User-written base name (e.g., "Color")
  std::vector<EnumVariant> variants;
  TypePtr underlyingType_ = std::make_shared<PrimitiveType>(Kind::Int32);
  std::string genericBase_;           // e.g. "Option" for Option_i32
  std::vector<TypePtr> genericArgs_;  // e.g. [i32] for Option_i32
  sun::semantic_analysis::QualifiedName qualifiedName_;

  sun::semantic_analysis::QualifiedName genericQualifiedName_;

 public:
  /** The original template name, independent of specialization and source
   * aliases. */
  const sun::semantic_analysis::QualifiedName& getGenericQualifiedName() const {
    return genericQualifiedName_;
  }
  /** Record the template that produced this type. */
  void setGenericQualifiedName(sun::semantic_analysis::QualifiedName name) {
    genericQualifiedName_ = std::move(name);
  }

  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;

  /**
   * Source spelling; declaration records carry module ownership.
   */
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return qualifiedName_;
  }
  /** Records the declaration name together with its enclosing scopes. */
  void setQualifiedName(sun::semantic_analysis::QualifiedName qn) {
    qualifiedName_ = std::move(qn);
  }

  /** Creates a semantic enum descriptor with its name and optional variants. */
  EnumType(std::string name_, std::string baseName = "")
      : name_(std::move(name_)), baseName_(std::move(baseName)) {}

  /** Creates a semantic enum descriptor with its name and optional variants. */
  EnumType(std::string name_, std::vector<EnumVariant> vars,
           std::string baseName = "")
      : name_(std::move(name_)),
        variants(std::move(vars)),
        baseName_(std::move(baseName)) {}

  /** Return the integer type used to store the enum tag. */
  const TypePtr& getUnderlyingType() const { return underlyingType_; }
  /** Set the integer representation before generating enum storage. */
  void setUnderlyingType(TypePtr type) {
    assert(type && type->isIntegral());
    underlyingType_ = std::move(type);
  }

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Enum;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name_; }
  /** Provides the alternatives declared by this enum. */
  const std::vector<EnumVariant>& getVariants() const { return variants; }

  /**
   * Base name accessor
   */
  const std::string& getBaseName() const {
    return baseName_.empty() ? name_ : baseName_;
  }
  /** Reports whether the unqualified declaration name is available. */
  bool hasBaseName() const { return !baseName_.empty(); }
  /** Sets the unqualified declaration name without changing its enclosing
   * scopes. */
  void setBaseName(std::string baseName) { baseName_ = std::move(baseName); }

  /**
   * Generic specialization origin (e.g. Option_i32 records base "Option" and
   * args [i32]); empty for non-generic enums.
   */
  void setGenericOrigin(std::string base, std::vector<TypePtr> args) {
    genericBase_ = std::move(base);
    genericArgs_ = std::move(args);
  }
  /** Returns the original generic declaration. */
  const std::string& getGenericBase() const { return genericBase_; }
  /** Returns the generic type arguments. */
  const std::vector<TypePtr>& getGenericArgs() const { return genericArgs_; }
  /** Reports whether this type represents generic specialization values. */
  bool isGenericSpecialization() const { return !genericBase_.empty(); }

  /**
   * Get user-friendly display name for error messages
   */
  std::string getDisplayName() const {
    if (isGenericSpecialization()) {
      std::string result = genericBase_ + "<";
      for (size_t i = 0; i < genericArgs_.size(); ++i) {
        if (i > 0) result += ", ";
        result += genericArgs_[i]->toDisplayString();
      }
      return result + ">";
    }
    if (!baseName_.empty()) return baseName_;
    if (!qualifiedName_.empty()) return qualifiedName_.display();
    return name_;
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override { return getDisplayName(); }

  /** Register a variant once, retaining its declaration identity. */
  void addVariant(const std::string& variantName, int64_t value,
                  DeclarationId declarationId = {}) {
    for (const auto& v : variants) {
      if (v.name == variantName) return;
    }
    variants.push_back({variantName, value, {}, declarationId});
  }

  /**
   * Attach resolved payload types to a variant (full-analysis phase; payload
   * annotations may reference classes not yet registered during declaration
   * collection).
   */
  void setVariantPayloadTypes(const std::string& variantName,
                              std::vector<TypePtr> payloadTypes) {
    for (auto& v : variants) {
      if (v.name == variantName) {
        v.payloadTypes = std::move(payloadTypes);
        return;
      }
    }
    assert(false && "setVariantPayloadTypes: unknown variant");
  }

  /**
   * True if any variant carries a payload (tagged-union representation)
   */
  bool hasPayload() const {
    for (const auto& v : variants) {
      if (v.hasPayload()) return true;
    }
    return false;
  }

  /** Returns the variant stored by this object. */
  const EnumVariant* getVariant(const std::string& variantName) const {
    for (const auto& variant : variants) {
      if (variant.name == variantName) return &variant;
    }
    return nullptr;
  }

  /**
   * Check if a variant exists by name
   */
  bool hasVariant(const std::string& variantName) const {
    return getVariant(variantName) != nullptr;
  }

  /**
   * Get the number of variants
   */
  size_t getNumVariants() const { return variants.size(); }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return name_; }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* e = dynamic_cast<const EnumType*>(&other)) {
      return sameDeclaration(*e);
    }
    return false;
  }

  /**
   * Payload-free enums use their integer type. Payload enums need the
   * module DataLayout for storage sizing: LLVMTypeResolver computes the
   * storage struct and caches it here; afterwards toLLVMType serves the
   * cache (e.g. for class field embedding via ClassType::getStructType).
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    if (hasPayload()) {
      if (cachedStorageType) return cachedStorageType;
      logAndThrowError("payload enum '" + getDisplayName() +
                       "' LLVM type requires DataLayout - resolve it via "
                       "LLVMTypeResolver first");
    }
    return underlyingType_->toLLVMType(ctx);
  }

  // LLVM struct caches, populated by LLVMTypeResolver (mirrors
  // ClassType::cachedLLVMType). storage = { i32 tag, [M x unitTy] }; per
  // variant = { i32 tag, T1, T2, ... } GEP'd on the same base pointer.
  mutable llvm::StructType* cachedStorageType = nullptr;
  mutable std::unordered_map<std::string, llvm::StructType*>
      cachedVariantStructs;
};

}  // namespace sun::types
