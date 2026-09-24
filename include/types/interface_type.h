/** Describes interface requirements, generic arguments, and vtable layout. */
#pragma once

#include "ast/type_constraint.h"
#include "llvm/IR/DerivedTypes.h"
#include "semantic_analysis/qualified_name.h"
#include "semantic_analysis/struct_names.h"
#include "semantic_analysis/visibility.h"
#include "types/nominal_type.h"
#include "types/type_utils.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
using sun::ast::TypeConstraint;

/**
 * Interface field information
 */
struct InterfaceField {
  std::string name;
  TypePtr type;
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;
  DeclarationId declarationId;
};

/**
 * Interface method information
 */
struct InterfaceMethod {
  std::string name;
  std::vector<std::string> typeParameters;  // Generic type params: <T, U>
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;  // Excludes implicit 'this' parameter
  bool hasDefaultImpl;    // true if this method has a default implementation
  bool isUnsafe = false;  // Calls require an unsafe block.
  bool isConst = false;   // `const function`: does not change `this`
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;

  DeclarationId declarationId;
  std::vector<DeclarationId> inheritedDeclarations;
  std::vector<TypePtr> genericArguments;
  std::vector<TypePtr> genericConstraints;

  /** Reports whether this declaration still has unbound type parameters. */
  bool isGeneric() const { return !typeParameters.empty(); }
};

// Forward declaration for InterfaceType
class InterfaceType;
/** Shared ownership of a semantic interface description. */
using InterfaceTypePtr = std::shared_ptr<InterfaceType>;

/**
 * Interface type for user-defined interfaces
 * Interfaces define a contract that classes must implement
 */
class InterfaceType : public NominalType {
  friend class sun::semantic_analysis::TypeRegistry;
  std::string name;       // Fully qualified name (includes library hash)
  std::string baseName_;  // User-written base name for error messages
  std::vector<std::string> typeParameters;  // Generic type params: T, U, etc.
  std::vector<TypePtr>
      typeArguments;  // Type args: [i32] for specialized interfaces
  std::string
      baseGenericName;  // For specialized: original generic interface name
  std::vector<InterfaceField> fields;
  std::vector<InterfaceMethod> methods;
  InterfaceTypePtr parent_;
  sun::semantic_analysis::QualifiedName qualifiedName_;
  // Lifetime names the interface DECLARES ('interface ISink<'a>').
  // Declarations only, never bindings - see ClassType::lifetimeParams_.
  std::vector<std::string> lifetimeParams_;

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

  /** Provides the lifetime parameters associated with this declaration. */
  const std::vector<std::string>& getLifetimeParams() const {
    return lifetimeParams_;
  }
  /** Stores the lifetime parameters associated with this declaration. */
  void setLifetimeParams(std::vector<std::string> names) {
    lifetimeParams_ = std::move(names);
  }

  /** Creates a semantic interface descriptor under its declared name. */
  InterfaceType(std::string interfaceName) : name(std::move(interfaceName)) {}

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

  /**
   * Constructor for generic interface definition
   */
  InterfaceType(std::string interfaceName, std::vector<std::string> typeParams)
      : name(std::move(interfaceName)), typeParameters(std::move(typeParams)) {}

  /**
   * Constructor for specialized generic interface
   */
  InterfaceType(std::string name_, std::string baseName,
                std::vector<TypePtr> typeArgs)
      : name(std::move(name_)),
        typeArguments(std::move(typeArgs)),
        baseGenericName(std::move(baseName)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Interface;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }
  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name; }

  /**
   * Base name accessors (user-written name for error messages)
   */
  const std::string& getBaseName() const {
    return baseName_.empty() ? name : baseName_;
  }
  /** Sets the unqualified declaration name without changing its enclosing
   * scopes. */
  void setBaseName(std::string bn) { baseName_ = std::move(bn); }
  /** Reports whether the unqualified declaration name is available. */
  bool hasBaseName() const { return !baseName_.empty(); }

  /** Provides the generic parameters declared by this type or function. */
  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  /** Provides the concrete types supplied for generic specialization. */
  const std::vector<TypePtr>& getTypeArguments() const { return typeArguments; }
  /** Returns the original generic name used to identify this specialization. */
  const std::string& getBaseGenericName() const { return baseGenericName; }
  /** Reports whether this is a generic declaration rather than a concrete
   * instance. */
  bool isGenericDefinition() const { return !typeParameters.empty(); }
  /** Reports whether this type was instantiated with concrete type arguments.
   */
  bool isSpecialized() const { return !typeArguments.empty(); }
  /** Provides the field declarations belonging to this type. */
  const std::vector<InterfaceField>& getFields() const { return fields; }
  /** Provides the method declarations belonging to this type. */
  const std::vector<InterfaceMethod>& getMethods() const { return methods; }

  /** Returns the resolved direct parent, or null for a root interface. */
  const InterfaceTypePtr& getParent() const { return parent_; }
  /** Records the resolved parent and its effective members. */
  void setInheritedShape(InterfaceTypePtr parent,
                         std::vector<InterfaceField> effectiveFields,
                         std::vector<InterfaceMethod> effectiveMethods) {
    parent_ = std::move(parent);
    fields = std::move(effectiveFields);
    methods = std::move(effectiveMethods);
  }
  /** Reports nominal conformance to this interface or one of its ancestors. */
  bool extendsInterface(const InterfaceType& target) const {
    return equals(target) || (parent_ && parent_->extendsInterface(target));
  }
  /** Returns the destruction slot after the dynamically dispatchable methods.
   */
  unsigned getDropIndex() const {
    unsigned result = 0;
    for (const auto& method : methods)
      if (!method.isGeneric()) ++result;
    return result;
  }
  /** Returns the direct parent's dispatch-table slot. */
  unsigned getParentIndex() const { return getDropIndex() + 1; }

  /**
   * Returns the (possibly pre-existing) record so callers can set access.
   */
  InterfaceField& addField(const std::string& fieldName, TypePtr fieldType,
                           DeclarationId id = {}) {
    for (auto& existingField : fields) {
      if (existingField.name == fieldName) return existingField;
    }
    fields.push_back({fieldName, std::move(fieldType),
                      sun::semantic_analysis::Visibility::Private, id});
    return fields.back();
  }

  /** Registers a method signature on this type for lookup and dispatch. */
  InterfaceMethod& addMethod(const std::string& methodName, TypePtr returnType,
                             std::vector<TypePtr> paramTypes,
                             bool hasDefaultImpl = false,
                             std::vector<std::string> typeParams = {}) {
    methods.push_back({methodName, std::move(typeParams), std::move(returnType),
                       std::move(paramTypes), hasDefaultImpl});
    return methods.back();
  }

  /** Returns the field stored by this object. */
  const InterfaceField* getField(const std::string& fieldName) const {
    for (const auto& field : fields) {
      if (field.name == fieldName) return &field;
    }
    return nullptr;
  }

  /** Returns the method stored by this object. */
  const InterfaceMethod* getMethod(const std::string& methodName) const {
    for (const auto& method : methods) {
      if (method.name == methodName) return &method;
    }
    return nullptr;
  }

  /**
   * Rebind one method's return type. Exists for the builtin IError: it is
   * registered before any source is read, so message() starts as
   * static_ptr<u8> and is retargeted to the String class when the stdlib
   * registers one (see SemanticAnalyzer::registerClassShape).
   */
  void setMethodReturnType(const std::string& methodName, TypePtr returnType) {
    for (auto& method : methods) {
      if (method.name == methodName) {
        method.returnType = std::move(returnType);
        return;
      }
    }
  }

  /**
   * Get methods that don't have default implementations (must be implemented by
   * class)
   */
  std::vector<const InterfaceMethod*> getRequiredMethods() const {
    std::vector<const InterfaceMethod*> required;
    for (const auto& method : methods) {
      if (!method.hasDefaultImpl) {
        required.push_back(&method);
      }
    }
    return required;
  }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    if (isSpecialized()) {
      // Show as BaseInterface<Arg1, Arg2>
      std::string result = baseGenericName + "<";
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toString();
      }
      result += ">";
      return result;
    }
    if (isGenericDefinition()) {
      // Show as Interface<T, U>
      std::string result = name + "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i];
      }
      result += ">";
      return result;
    }
    return name;
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override {
    std::string base;
    if (!baseName_.empty()) {
      base = baseName_;
    } else if (!baseGenericName.empty()) {
      base = baseGenericName;
      for (size_t i = 0; i < base.size(); ++i) {
        if (base[i] == '_') base[i] = '.';
      }
    } else {
      base = name;
      for (size_t i = 0; i < base.size(); ++i) {
        if (base[i] == '_') base[i] = '.';
      }
    }
    if (isSpecialized() && !typeArguments.empty()) {
      std::string result = base + "<";
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toDisplayString();
      }
      result += ">";
      return result;
    }
    return base;
  }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* i = dynamic_cast<const InterfaceType*>(&other)) {
      return sameDeclaration(*i);
    }
    return false;
  }

  /**
   * Interfaces are represented as fat pointers: { ptr data, ptr vtable }.
   * The vtable contains methods, drop glue, and an optional parent table link.
   * Owned and borrowed handles share the table; only owned values run cleanup.
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return getFatPointerType(ctx);
  }

  // ===================================================================
  // Dynamic Dispatch Support (vtable-based polymorphism)
  // ===================================================================

  /**
   * Get the fat pointer struct type for interface values: { ptr data, ptr
   * vtable }
   * - data: pointer to the concrete class instance
   * - vtable: pointer to the implementing class's vtable for this interface
   */
  static llvm::StructType* getFatPointerType(llvm::LLVMContext& ctx) {
    // Check for existing named type to avoid duplicates
    if (auto* existing = llvm::StructType::getTypeByName(
            ctx, sun::semantic_analysis::InterfaceFat)) {
      return existing;
    }
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    return llvm::StructType::create(ctx, {ptrTy, ptrTy},
                                    sun::semantic_analysis::InterfaceFat);
  }

  /**
   * Get the vtable struct type for this interface.
   * Non-generic methods precede drop glue and an optional parent table link.
   */
  llvm::StructType* getVtableType(llvm::LLVMContext& ctx) const {
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    std::vector<llvm::Type*> slotTypes(getDropIndex() + 1 + (parent_ ? 1 : 0), ptrTy);
    return llvm::StructType::get(ctx, slotTypes);
  }

  /**
   * Get the slot index for a method in the vtable.
   * Returns -1 if method not found or if the method is generic.
   * Only non-generic methods can be dispatched via vtable.
   */
  int getMethodIndex(DeclarationId declaration) const {
    int index = 0;
    for (const auto& method : methods) {
      if (method.isGeneric()) {
        continue;  // Skip generic methods - they're not in the vtable
      }
      if (method.declarationId == declaration) {
        return index;
      }
      ++index;
    }
    return -1;  // Method not found or is generic
  }
};

}  // namespace sun::types
