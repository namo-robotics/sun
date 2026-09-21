/** Owns nominal type registration and specialization identities for a session.
 */
#pragma once
#include <unordered_set>

#include "types/types.h"

/** Registers declarations and their nominal types during semantic analysis. */
namespace sun::semantic_analysis {
/** The semantic inputs that distinguish instances of one template. */
struct SpecializationKey {
  DeclarationId source;
  DeclarationId enclosing;
  std::vector<sun::types::TypePtr> arguments;
  std::optional<std::vector<sun::types::TypePtr>> variadic;

  /** Compare semantic types, including nominal declaration identities. */
  bool operator==(const SpecializationKey& other) const;
};

/** Bucket instances by template and argument kinds; equality checks structure.
 */
struct SpecializationKeyHash {
  /** Computes a hash for the supplied value for use in unordered containers. */
  size_t operator()(const SpecializationKey& key) const {
    size_t hash = key.source.index();
    auto combine = [&](size_t value) { hash = hash * 31 + value; };
    combine(key.enclosing.index());
    combine(key.arguments.size());
    for (const auto& arg : key.arguments)
      combine(arg ? static_cast<size_t>(arg->getKind()) + 1 : 0);
    combine(key.variadic.has_value());
    if (key.variadic) {
      combine(key.variadic->size());
      for (const auto& arg : *key.variadic)
        combine(arg ? static_cast<size_t>(arg->getKind()) + 1 : 0);
    }
    return hash;
  }
};

/**
 * TypeRegistry - Per-compilation-unit registry for class and interface types.
 *
 * This replaces the static caches in Types class to avoid cross-test pollution.
 * One exists per compilation, as part of its AnalysisResults, next to the
 * DeclarationTable whose declarations its types are created for.
 */
class TypeRegistry {
  // The declarations types are created for; owned by the analysis results.
  DeclarationTable& declarations_;
  std::unordered_map<DeclarationId, std::shared_ptr<sun::types::NominalType>>
      nominalTypes_;

  /** Looks up the class, interface, or enum type for a declaration identity. */
  template <typename T>
  std::shared_ptr<T> nominalType(DeclarationId id, DeclarationKind kind) {
    const auto& record = declarations_.get(id);
    if (record.kind != kind)
      sun::support::logAndThrowError(
          "Declaration kind does not match nominal type");
    auto found = nominalTypes_.find(id);
    if (found != nominalTypes_.end())
      return std::static_pointer_cast<T>(found->second);
    auto type = std::make_shared<T>(record.name);
    type->declarationId_ = id;
    type->declarationSession_ = declarations_.session();
    nominalTypes_.emplace(id, type);
    return type;
  }

  std::unordered_map<SpecializationKey, DeclarationId, SpecializationKeyHash>
      specializations_;

 public:
  /** The builtin error interface shared throughout this analysis session. */
  std::shared_ptr<sun::types::InterfaceType> errorInterface;

  /**
   * Initializes the collection of primitive and declared semantic types.
   * Every type is created for a declaration in `declarations`, which belongs
   * to the same analysis results and must outlive the registry.
   */
  explicit TypeRegistry(DeclarationTable& declarations)
      : declarations_(declarations) {
    registerBuiltins();
  }

  /**
   * Register built-in types (IError). The iteration protocol
   * (IIterator/IIterable) lives in stdlib/iterator.sun since it names Option.
   */
  void registerBuiltins() {
    // Create IError interface with code() and message() methods.
    // message() starts as static_ptr<u8> — the only string type that exists
    // before any source is read. When the stdlib's String class is registered,
    // the return type is retargeted to it (an owned clone of the message), so
    // errors can carry text composed at runtime. Without the stdlib, message()
    // stays literal-only.
    auto id = declarations_.add(DeclarationKind::Interface, "IError");
    declarations_.bindPortable(
        id,
        PortableDeclarationKey::original(
            "4c7b23a9e50e3bb484c7f661e4700d156bac371ed9dd5d23c3d22e2fefc263c1",
            1));
    auto ierror =
        nominalType<sun::types::InterfaceType>(id, DeclarationKind::Interface);
    // Not const: every user error class would then have to spell
    // `const function code()`, and errors are caught into plain variables.
    ierror->addMethod("code", sun::types::Types::Int32(), {}, true)
        .declarationId =
        declarations_.add(DeclarationKind::Function, "code", id);
    ierror->addMethod("message", sun::types::Types::String(), {}, true)
        .declarationId =
        declarations_.add(DeclarationKind::Function, "message", id);
    uint64_t ordinal = 2;
    for (const auto& method : ierror->getMethods())
      declarations_.bindPortable(
          method.declarationId,
          PortableDeclarationKey::original("4c7b23a9e50e3bb484c7f661e4700d156ba"
                                           "c371ed9dd5d23c3d22e2fefc263c1",
                                           ordinal++));
    errorInterface = ierror;
  }

  /**
   * Check if a type name is a builtin type that cannot be redefined
   * Includes builtin interfaces and type traits used by _is&lt;T&gt;
   */
  bool isBuiltinTypeName(const std::string& name) const {
    static const std::unordered_set<std::string> builtinNames = {
        // Builtin interfaces
        "IError",
        // Type traits for _is<T> intrinsic
        "_Integer", "_Signed", "_Unsigned", "_Float", "_Numeric", "_Primitive"};
    return builtinNames.count(name) > 0;
  }

  /**
   * Non-copyable to prevent accidental duplication
   */
  TypeRegistry(const TypeRegistry&) = delete;
  /** Disallows assignment so ownership and object identity cannot be
   * duplicated. */
  TypeRegistry& operator=(const TypeRegistry&) = delete;

  /**
   * Movable
   */
  TypeRegistry(TypeRegistry&&) = default;
  /** Transfers the stored state from another instance during move assignment.
   */
  TypeRegistry& operator=(TypeRegistry&&) = default;

  /** Get a source class before its source name has been assigned. */
  std::shared_ptr<sun::types::ClassType> getClass(DeclarationId id) {
    return nominalType<sun::types::ClassType>(id, DeclarationKind::Class);
  }

  /** Bind the current source name to a source class's existing identity. */
  std::shared_ptr<sun::types::ClassType> getClass(DeclarationId id,
                                                  const QualifiedName& name) {
    auto type = getClass(id);
    if (!type->getQualifiedName().baseName.empty() &&
        type->getQualifiedName() != name)
      sun::support::logAndThrowError(
          "Cannot change a nominal type's assigned source name");
    type->name_ = name.lookupName();
    type->setQualifiedName(name);
    type->setBaseName(name.baseName);
    return type;
  }

  /** Get a source interface before its source name has been assigned. */
  std::shared_ptr<sun::types::InterfaceType> getInterface(DeclarationId id) {
    return nominalType<sun::types::InterfaceType>(id,
                                                  DeclarationKind::Interface);
  }

  /** Bind the current source name to a source interface's identity. */
  std::shared_ptr<sun::types::InterfaceType> getInterface(
      DeclarationId id, const QualifiedName& name) {
    auto type = getInterface(id);
    if (!type->getQualifiedName().baseName.empty() &&
        type->getQualifiedName() != name)
      sun::support::logAndThrowError(
          "Cannot change a nominal type's assigned source name");
    type->name = name.lookupName();
    type->setQualifiedName(name);
    type->setBaseName(name.baseName);
    return type;
  }

  /** Get a source enum before its source name has been assigned. */
  std::shared_ptr<sun::types::EnumType> getEnum(DeclarationId id) {
    return nominalType<sun::types::EnumType>(id, DeclarationKind::Enum);
  }

  /** Bind the current source name to a source enum's existing identity. */
  std::shared_ptr<sun::types::EnumType> getEnum(DeclarationId id,
                                                const QualifiedName& name) {
    auto type = getEnum(id);
    if (!type->getQualifiedName().baseName.empty() &&
        type->getQualifiedName() != name)
      sun::support::logAndThrowError(
          "Cannot change a nominal type's assigned source name");
    type->name_ = name.lookupName();
    type->setQualifiedName(name);
    type->setBaseName(name.baseName);
    return type;
  }

  /** Intern an instance before resolving its members or body. */
  DeclarationId specialize(const SpecializationKey& key) {
    auto found = specializations_.find(key);
    if (found != specializations_.end()) return found->second;
    const auto& source = declarations_.get(key.source);
    auto id = declarations_.add(
        source.kind, source.name, key.enclosing ? key.enclosing : source.owner,
        source.module, std::make_shared<const SpecializationKey>(key),
        key.source);
    specializations_.emplace(key, id);
    return id;
  }

  /** Return an existing instance without allocating a declaration. */
  DeclarationId findSpecialization(const SpecializationKey& key) const {
    auto found = specializations_.find(key);
    return found == specializations_.end() ? DeclarationId{} : found->second;
  }

  /** Configure the class attached to an interned specialization. */
  std::shared_ptr<sun::types::ClassType> getSpecializedClass(
      DeclarationId id, const QualifiedName& name, const QualifiedName& source,
      const std::vector<sun::types::TypePtr>& arguments) {
    auto type = getClass(id, name);
    type->baseGenericName = source.lookupName();
    type->setBaseName(source.display());
    type->typeArguments = arguments;
    type->setGenericQualifiedName(source);
    return type;
  }

  /** Intern a generic interface template by its source declaration. */
  std::shared_ptr<sun::types::InterfaceType> getGenericInterface(
      DeclarationId declaration, const QualifiedName& name,
      std::vector<std::string> typeParams) {
    auto type = getInterface(declaration, name);
    type->typeParameters = std::move(typeParams);
    return type;
  }

  /** Configure the interface attached to an interned specialization. */
  std::shared_ptr<sun::types::InterfaceType> getSpecializedInterface(
      DeclarationId id, const QualifiedName& name, const QualifiedName& source,
      const std::vector<sun::types::TypePtr>& arguments) {
    auto type = getInterface(id, name);
    type->baseGenericName = source.lookupName();
    type->setBaseName(source.baseName);
    type->typeArguments = arguments;
    type->setGenericQualifiedName(source);
    return type;
  }

  /**
   * Clear all caches (useful for REPL reset)
   */
  void clear() {
    nominalTypes_.clear();
    specializations_.clear();
    errorInterface.reset();
  }
};

}  // namespace sun::semantic_analysis
