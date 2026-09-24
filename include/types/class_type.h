/** Describes class fields, methods, layout, and interface relationships. */
#pragma once

#include <algorithm>
#include <unordered_map>

#include "ast/type_constraint.h"
#include "semantic_analysis/qualified_name.h"
#include "semantic_analysis/visibility.h"
#include "types/array_type.h"
#include "types/lambda_type.h"
#include "types/nominal_type.h"
#include "types/pointer_types.h"
#include "types/type_utils.h"

/** Defines shared type descriptions used throughout the compiler. */
namespace sun::types {
using sun::ast::TypeConstraint;

/**
 * Class field information
 */
struct ClassField {
  std::string name;
  TypePtr type;
  size_t index;  // Index in the struct
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;
  DeclarationId declarationId;
};

/**
 * Class method information
 */
struct ClassMethod {
  std::string name;
  std::vector<std::string> typeParameters;  // Generic type params: <T, U>
  TypePtr returnType;
  std::vector<TypePtr> paramTypes;  // Excludes implicit 'this' parameter
  bool isConstructor;               // true if this is the 'init' method
  bool canThrow = false;  // declared with 'throws IError' — may unwind
  bool isUnsafe = false;  // Calls require an unsafe block.
  bool isConst = false;   // `const function`: does not change `this`
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;
  bool isSynthesizedConstructor = false;
  DeclarationId declarationId;
  DeclarationId defaultImplementation;

  /** Reports whether this declaration still has unbound type parameters. */
  bool isGeneric() const { return !typeParameters.empty(); }
};

/** Describes an interface that a class may implement. */
class InterfaceType;

/**
 * Class type for user-defined classes
 * Classes are represented as LLVM structs with methods as separate functions
 * Generic classes have type parameters (e.g., class List&lt;T&gt;)
 * Specialized classes have type arguments (e.g., List<i32>)
 */
class ClassType : public NominalType {
  friend class sun::semantic_analysis::TypeRegistry;
  std::string
      name_;  // Source-qualified spelling for diagnostic type signatures.
  std::string
      baseName_;  // User-written base name (e.g., "Unique") for error messages
  sun::semantic_analysis::QualifiedName
      qualifiedName_;  // Structured qualified name for scoping
  std::vector<std::string>
      typeParameters;  // Type params: ["T", "U"] for generic definitions
  std::vector<TypePtr>
      typeArguments;            // Type args: [i32] for specialized classes
  std::string baseGenericName;  // For specialized: original generic class name
  sun::semantic_analysis::QualifiedName
      genericQualifiedName_;  // For specialized: the generic's qualified name
  std::vector<ClassField> fields;
  std::vector<ClassMethod> methods;
  std::unordered_map<DeclarationId, DeclarationId> interfaceImplementations_;
  std::vector<DeclarationId>
      implementedInterfaces;  // Interfaces this class implements
  std::vector<DeclarationId>
      staticOnlyInterfaces;  // Implemented, but not convertible to (see below)
  bool isPacked_ = false;    // "packed class": lay fields out with no padding
  // Lifetime names the class DECLARES ('class Bus<'a>'). Declarations only,
  // never bindings, so sharing one ClassType per class stays sound; the
  // borrow checker uses them to entangle a method's named parameters with
  // the receiver at call sites.
  std::vector<std::string> lifetimeParams_;
  mutable llvm::StructType* cachedLLVMType = nullptr;

 public:
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

  /** Creates a semantic class descriptor under its declared name. */
  ClassType(std::string className) : name_(std::move(className)) {}

  /**
   * Constructor for generic class definition
   */
  ClassType(std::string className, std::vector<std::string> typeParams)
      : name_(std::move(className)), typeParameters(std::move(typeParams)) {}

  /**
   * Constructor for specialized generic class
   */
  ClassType(std::string name_, std::string baseName,
            std::vector<TypePtr> typeArgs)
      : name_(std::move(name_)),
        typeArguments(std::move(typeArgs)),
        baseGenericName(std::move(baseName)) {}

  // The kind every value of this class carries; TypeCheck<T> keys off it
  static constexpr Kind StaticKind = Kind::Class;
  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const override { return StaticKind; }

  /**
   * Base name accessors (user-written name for error messages)
   */
  const std::string& getBaseName() const {
    return baseName_.empty() ? name_ : baseName_;
  }
  /** Sets the unqualified declaration name without changing its enclosing
   * scopes. */
  void setBaseName(std::string bn) { baseName_ = std::move(bn); }
  /** Reports whether the unqualified declaration name is available. */
  bool hasBaseName() const { return !baseName_.empty(); }

  /**
   * Qualified name accessors
   */
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return qualifiedName_;
  }
  /** Records the declaration name together with its enclosing scopes. */
  void setQualifiedName(sun::semantic_analysis::QualifiedName qn) {
    qualifiedName_ = std::move(qn);
  }
  /** Reports whether a name including the enclosing scopes has been assigned.
   */
  bool hasQualifiedName() const { return !qualifiedName_.baseName.empty(); }

  /**
   * Get user-friendly display name for error messages
   * For specialized classes: "Vec<i32>" or "std.Vec<i32>"
   * For non-specialized classes, preserve the source spelling.
   */
  std::string getDisplayName() const {
    // Prefer the structured name: QualifiedName::display() spells the scope
    // path with dots and drops bundle hash segments. A specialization shows
    // the generic it came from, with its arguments spelled out.
    std::string base;
    if (!baseName_.empty()) {
      base = baseName_;
    } else if (!genericQualifiedName_.empty()) {
      base = genericQualifiedName_.display();
    } else if (hasQualifiedName()) {
      base = qualifiedName_.display();
    } else {
      base = name_;
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

  /** Provides the generic parameters declared by this type or function. */
  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  /** Provides the concrete types supplied for generic specialization. */
  const std::vector<TypePtr>& getTypeArguments() const { return typeArguments; }
  /** Returns the original generic name used to identify this specialization. */
  const std::string& getBaseGenericName() const { return baseGenericName; }
  /**
   * Qualified name of the generic this specialization was instantiated from
   * (scope path + plain base name), for scope-tree lookups
   */
  const sun::semantic_analysis::QualifiedName& getGenericQualifiedName() const {
    return genericQualifiedName_;
  }
  /** Updates the generic qualified name stored by this object. */
  void setGenericQualifiedName(sun::semantic_analysis::QualifiedName qn) {
    genericQualifiedName_ = std::move(qn);
  }
  /** Reports whether this is a generic declaration rather than a concrete
   * instance. */
  bool isGenericDefinition() const { return !typeParameters.empty(); }
  /** Reports whether this type was instantiated with concrete type arguments.
   */
  bool isSpecialized() const { return !typeArguments.empty(); }
  /** Provides the field declarations belonging to this type. */
  const std::vector<ClassField>& getFields() const { return fields; }
  /** The selected cleanup method, if this class defines one. */
  DeclarationId deinitializer;

  /** Provides the method declarations belonging to this type. */
  const std::vector<ClassMethod>& getMethods() const { return methods; }

  /** Updates a builtin method contract when the standard String type is known.
   */
  void setMethodReturnType(const std::string& name, TypePtr type) {
    for (auto& method : methods)
      if (method.name == name) method.returnType = type;
  }

  /** Record the concrete method selected for an interface declaration. */
  void bindInterfaceMethod(DeclarationId requirement,
                           DeclarationId implementation) {
    interfaceImplementations_[requirement] = implementation;
  }

  /** Retrieve the concrete method selected during conformance checking. */
  DeclarationId getInterfaceMethod(DeclarationId requirement) const {
    auto found = interfaceImplementations_.find(requirement);
    if (found == interfaceImplementations_.end())
      logAndThrowError("Interface method implementation has not been resolved");
    return found->second;
  }
  /** Provides the interfaces implemented by this class. */
  const std::vector<DeclarationId>& getImplementedInterfaces() const {
    return implementedInterfaces;
  }

  /** Reports whether this object has field. */
  bool hasField(const std::string& fieldName) const {
    return getField(fieldName) != nullptr;
  }

  /**
   * Returns the new record so callers can set its access info.
   */
  ClassField& addField(const std::string& fieldName, TypePtr fieldType,
                       DeclarationId id = {}) {
    // Caller should check hasField() first and report error with position
    fields.push_back({fieldName, std::move(fieldType), fields.size(),
                      sun::semantic_analysis::Visibility::Private, id});
    return fields.back();
  }

  /** Registers a method signature on this type for lookup and dispatch. */
  ClassMethod& addMethod(const std::string& methodName, TypePtr returnType,
                         std::vector<TypePtr> paramTypes,
                         bool isConstructor = false,
                         std::vector<std::string> typeParams = {},
                         bool canThrow = false) {
    methods.push_back({methodName, std::move(typeParams), std::move(returnType),
                       std::move(paramTypes), isConstructor, canThrow});
    return methods.back();
  }

  /** Record a resolved interface implemented by this class. */
  void addImplementedInterface(const InterfaceType& interface);

  /** Check conformance using the interface's session and declaration ID. */
  bool implementsInterface(const InterfaceType& interface) const;

  /** Mark an implementation whose return ABI prevents dynamic dispatch. */
  void markStaticOnlyInterface(const InterfaceType& interface);

  /** Report whether a resolved interface can be used as a fat pointer. */
  bool convertibleToInterface(const InterfaceType& interface) const;

  /** Retrieve a selected field independently of its spelling and layout. */
  const ClassField* getField(DeclarationId id) const {
    if (!id) return nullptr;
    for (const auto& field : fields)
      if (field.declarationId == id) return &field;
    return nullptr;
  }

  /** Returns the field stored by this object. */
  const ClassField* getField(const std::string& fieldName) const {
    for (const auto& field : fields) {
      if (field.name == fieldName) return &field;
    }
    return nullptr;
  }

  /** Retrieve a selected method without repeating overload resolution. */
  const ClassMethod* getMethod(DeclarationId declaration) const {
    if (!declaration) return nullptr;
    for (const auto& method : methods) {
      if (method.declarationId == declaration) return &method;
    }
    logAndThrowError("Selected method does not belong to this class");
  }

  /** Returns the method stored by this object. */
  const ClassMethod* getMethod(const std::string& methodName) const {
    for (const auto& method : methods) {
      if (method.name == methodName) return &method;
    }
    return nullptr;
  }

  /**
   * Returns true if an array argument of type `from` is compatible with an
   * array parameter of type `to` (same element type, with an unsized parameter
   * accepting any sized array). Mirrors the array coercion allowed elsewhere so
   * that e.g. array<i32, 3, 2> can be passed where array<i32> is expected.
   */
  static bool isArrayCompatible(const TypePtr& from, const TypePtr& to) {
    if (!from || !to || !from->isArray() || !to->isArray()) {
      return false;
    }
    auto* toArr = static_cast<const ArrayType*>(to.get());
    auto* fromArr = static_cast<const ArrayType*>(from.get());
    return toArr->isCompatibleWith(*fromArr);
  }

  /**
   * Returns true if a value of type `from` can be implicitly widened to type
   * `to` (integer-to-wider-integer or float-to-wider-float). This mirrors the
   * numeric widening allowed by free-function overload resolution so that
   * method/constructor overloads accept the same arguments (e.g. an i32 literal
   * passed where an i64 parameter is expected).
   */
  static bool isNumericWidenable(const TypePtr& from, const TypePtr& to) {
    if (!from || !to || !from->isPrimitive() || !to->isPrimitive()) {
      return false;
    }
    auto fromKind = from->getKind();
    auto toKind = to->getKind();

    auto intBitWidth = [](Type::Kind k) -> int {
      switch (k) {
        case Type::Kind::Int8:
        case Type::Kind::UInt8:
          return 8;
        case Type::Kind::Int16:
        case Type::Kind::UInt16:
          return 16;
        case Type::Kind::Int32:
        case Type::Kind::UInt32:
          return 32;
        case Type::Kind::Int64:
        case Type::Kind::UInt64:
          return 64;
        default:
          return 0;
      }
    };

    int fromWidth = intBitWidth(fromKind);
    int toWidth = intBitWidth(toKind);
    if (fromWidth != 0 && toWidth != 0) {
      return fromWidth <= toWidth;
    }

    bool fromFloat =
        fromKind == Type::Kind::Float32 || fromKind == Type::Kind::Float64;
    bool toFloat =
        toKind == Type::Kind::Float32 || toKind == Type::Kind::Float64;
    return fromFloat && toFloat;
  }

  /**
   * True if an argument of type `from` reaches an interface-typed parameter
   * `to` by conversion to a fat pointer: an owned class where the interface
   * is taken by value, or a class where `ref Interface` is expected.
   * Mirrors the interface rules of isAssignableTo
   * (type_analysis/type_checking.cpp) so that overload selection accepts what a
   * single known signature accepts. Implemented with complete interface and
   * lifetime information.
   */
  static bool isInterfaceConvertible(const TypePtr& from, const TypePtr& to);

  /**
   * Get method with overload resolution based on argument types.
   * Returns the method whose parameter types best match the provided arg
   * types. An exact match wins; otherwise the first overload reachable by an
   * implicit argument conversion (borrow, array view, numeric or lambda
   * widening, class to interface) is chosen.
   */
  const ClassMethod* getMethodForArgs(
      const std::string& methodName,
      const std::vector<TypePtr>& argTypes) const {
    const ClassMethod* bestMatch = nullptr;
    bool foundExact = false;

    for (const auto& method : methods) {
      if (method.name != methodName) continue;
      if (method.paramTypes.size() != argTypes.size()) continue;

      bool allMatch = true;
      bool allExact = true;
      for (size_t i = 0; i < argTypes.size(); ++i) {
        if (!argTypes[i] || !method.paramTypes[i]) {
          allMatch = false;
          break;
        }

        // Exact type match
        if (method.paramTypes[i]->equals(*argTypes[i])) {
          continue;
        }
        allExact = false;

        // A borrowed scalar can be read into a value parameter, including
        // numeric widening. Borrowed compound values must keep their owner.
        if (argTypes[i]->isReference() &&
            !method.paramTypes[i]->isReference() &&
            typeCopiesByRead(method.paramTypes[i])) {
          TypePtr valueType = unwrapRef(argTypes[i]);
          if (method.paramTypes[i]->equals(*valueType) ||
              isNumericWidenable(valueType, method.paramTypes[i])) {
            continue;
          }
        }

        // A static_ptr argument narrows to a raw_ptr parameter of the same
        // pointee: the data pointer is passed. Never the other way around.
        if (method.paramTypes[i]->isRawPointer() &&
            argTypes[i]->isStaticPointer()) {
          auto* r =
              static_cast<const RawPointerType*>(method.paramTypes[i].get());
          auto* s = static_cast<const StaticPointerType*>(argTypes[i].get());
          if (s->getPointeeType()->equals(*r->getPointeeType())) {
            continue;
          }
        }

        // Reference parameter accepts the referenced type
        if (method.paramTypes[i]->isReference()) {
          auto* refType =
              static_cast<const ReferenceType*>(method.paramTypes[i].get());
          const TypePtr& referenced = refType->getReferencedType();
          if (referenced->equals(*argTypes[i])) {
            continue;
          }
          // A borrow of the other mutability: only ref -> const ref
          if (argTypes[i]->isReference()) {
            auto* argRef = static_cast<const ReferenceType*>(argTypes[i].get());
            if (refMutabilityConvertible(*argRef, *refType) &&
                referenced->equals(*argRef->getReferencedType())) {
              continue;
            }
          }
          // ref to an (unsized) array accepts a compatible sized array, e.g.
          // passing array<i32, 3, 2> where ref array<i32> is expected.
          if (isArrayCompatible(argTypes[i], referenced)) {
            continue;
          }
        }

        // Array compatibility for by-value array parameters (sized -> unsized).
        if (isArrayCompatible(argTypes[i], method.paramTypes[i])) {
          continue;
        }

        // Numeric widening: e.g. an i32 literal argument for an i64 parameter
        if (isNumericWidenable(argTypes[i], method.paramTypes[i])) {
          continue;
        }

        // Lambda widening: non-throwing where throwing is expected, and
        // environment-free where '<'_>' is expected
        if (method.paramTypes[i]->isLambda() && argTypes[i]->isLambda()) {
          auto* paramL =
              static_cast<const LambdaType*>(method.paramTypes[i].get());
          auto* argL = static_cast<const LambdaType*>(argTypes[i].get());
          if (paramL->acceptsValueOf(*argL)) {
            continue;
          }
        }

        // A class becomes a fat pointer where an interface it implements is
        // expected (issue #219).
        if (isInterfaceConvertible(argTypes[i], method.paramTypes[i])) {
          continue;
        }

        // No match for this parameter
        allMatch = false;
        break;
      }

      if (allMatch) {
        // Prefer exact matches over ref-compatible matches
        if (allExact) {
          return &method;  // Exact match - return immediately
        }
        if (!foundExact) {
          bestMatch = &method;
        }
      }
    }

    return bestMatch;
  }

  /**
   * Get the constructor method (named "init")
   */
  const ClassMethod* getConstructor() const {
    for (const auto& method : methods) {
      if (method.isConstructor) return &method;
    }
    return nullptr;
  }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    if (isSpecialized() && !baseGenericName.empty()) {
      // Show specialized type like "List<i32>"
      std::string result = baseGenericName + "<";
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toString();
      }
      result += ">";
      return result;
    }
    if (isGenericDefinition()) {
      // Show generic definition like "List<T>"
      std::string result = name_ + "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i];
      }
      result += ">";
      return result;
    }
    return name_;
  }

  /** Returns a source-facing type name without internal compiler prefixes. */
  std::string toDisplayString() const override { return getDisplayName(); }

  /** Reports whether the other type has the same semantic identity. */
  bool equals(const Type& other) const override {
    if (auto* c = dynamic_cast<const ClassType*>(&other)) {
      return sameDeclaration(*c);
    }
    return false;
  }

  /**
   * Classes are value types represented as LLVM structs
   */
  llvm::Type* toLLVMType(llvm::LLVMContext& ctx) const override {
    return getStructType(ctx);
  }

  /**
   * Get the actual struct type for the class
   */
  llvm::StructType* getStructType(llvm::LLVMContext& ctx) const {
    if (cachedLLVMType) return cachedLLVMType;

    // LLVM shares layouts by their fields and packing. Source names do not
    // identify layouts: separate declarations may have the same spelling.
    std::vector<llvm::Type*> fieldTypes;
    for (const auto& field : fields) {
      // For class-typed fields, embed the struct directly (not a pointer)
      if (field.type->isClass()) {
        auto* classType = static_cast<const ClassType*>(field.type.get());
        fieldTypes.push_back(classType->getStructType(ctx));
      } else {
        fieldTypes.push_back(field.type->toLLVMType(ctx));
      }
    }
    cachedLLVMType = llvm::StructType::get(ctx, fieldTypes, isPacked_);
    return cachedLLVMType;
  }

  /**
   * Packed classes have no inter-field padding and struct alignment 1.
   * Must be set before the first getStructType() call, which memoizes.
   */
  bool isPacked() const { return isPacked_; }
  /** Controls whether the class uses a packed memory layout. */
  void setPacked(bool v) { isPacked_ = v; }
};

}  // namespace sun::types
