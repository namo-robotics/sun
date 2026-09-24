#include "semantic_analysis/portable_declaration_key.h"

#include <algorithm>
#include <set>

#include "ast.h"
#include "ast/ast_children.h"
#include "llvm/Support/SHA256.h"
#include "support/error.h"
#include "types/types.h"

using sun::ast::ExprAST;
using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::types::ArrayType;
using sun::types::ErrorUnionType;
using sun::types::FunctionType;
using sun::types::LambdaType;
using sun::types::NominalType;
using sun::types::RawPointerType;
using sun::types::ReferenceType;
using sun::types::StaticPointerType;
using sun::types::Type;
using sun::types::TypePtr;

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/** Encode integers in a fixed width and byte order on every host. */
std::string integer(uint64_t value) {
  std::string result(8, '\0');
  for (int i = 7; i >= 0; --i) {
    result[i] = static_cast<char>(value & 255);
    value >>= 8;
  }
  return result;
}

/** Encode a tagged tuple with a count and a length for every field. */
std::string tuple(char tag, const std::vector<std::string>& fields) {
  std::string result(1, tag);
  result += integer(fields.size());
  for (const auto& field : fields) {
    result += integer(field.size());
    result += field;
  }
  return result;
}

/** Hash framed derivation inputs without retaining nested keys in the result.
 */
std::string derivedKey(char tag, const std::vector<std::string>& fields) {
  llvm::SHA256 sha;
  sha.update(tuple(tag, fields));
  std::string result = "$";
  constexpr char hex[] = "0123456789abcdef";
  for (auto byte : sha.final()) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}

/** Encode type arguments in their declared order. */
std::string types(const std::vector<PortableTypeKey>& arguments) {
  std::vector<std::string> fields;
  for (const auto& arg : arguments) fields.push_back(arg.encoding());
  return tuple('L', fields);
}

/** Reject attempts to emit or derive an identity before its origin exists. */
const std::string& require(const PortableDeclarationKey& key) {
  if (key.empty())
    logAndThrowError("Portable declaration identity is not assigned");
  return key.encoding();
}

}  // namespace

void PortableDeclarationKey::assignOriginals(const ExprAST& root,
                                             DeclarationTable& table,
                                             const std::string& artifactHash) {
  std::set<DeclarationId> seen;
  uint64_t ordinal = 1;  // The bundle scope occupies the first ordinal.
  auto assign = [&](DeclarationId id) {
    if (!id || !seen.insert(id).second) return;
    const auto& record = table.get(id);
    if (record.specialization || record.origin) return;
    auto key = original(artifactHash, ++ordinal);
    if (record.portableKey) return;
    table.bindPortable(id, key, artifactHash);
  };
  auto identity = [&](const DeclarationIdentity& value) {
    assign(value.id);
    for (auto id : value.typeParameters) assign(id);
    for (auto id : value.lifetimeParameters) assign(id);
    for (auto id : value.parameters) assign(id);
  };
  auto walk = [&](auto&& self, const ExprAST& node) -> void {
    if (auto* moon = dynamic_cast<const sun::ast::MoonScopeAST*>(&node)) {
      if (!moon->isOwnBundle()) return;
    } else if (node.getDeclarationId())
      identity(node.declarationIdentity());
    if (auto* cls = dynamic_cast<const sun::ast::ClassDefinitionAST*>(&node))
      for (const auto& field : cls->getFields()) identity(field.declaration);
    if (auto* iface =
            dynamic_cast<const sun::ast::InterfaceDefinitionAST*>(&node))
      for (const auto& field : iface->getFields()) identity(field.declaration);
    if (auto* enumeration =
            dynamic_cast<const sun::ast::EnumDefinitionAST*>(&node))
      for (const auto& variant : enumeration->getVariants())
        identity(variant.declaration);
    if (auto* match = dynamic_cast<const sun::ast::MatchExprAST*>(&node))
      for (const auto& arm : match->getArms())
        for (const auto& binding : arm.bindings) identity(binding.declaration);
    if (auto* expression =
            dynamic_cast<const sun::ast::TryCatchExprAST*>(&node))
      for (const auto& clause : expression->getCatchClauses())
        identity(clause.declaration);
    std::vector<const ExprAST*> children;
    sun::ast::forEachChild(
        node, [&](const ExprAST& child) { children.push_back(&child); });
    std::stable_sort(children.begin(), children.end(),
                     [](const auto* a, const auto* b) {
                       const auto& first = a->getLocation();
                       const auto& second = b->getLocation();
                       return std::tie(first.filePath, first.offset) <
                              std::tie(second.filePath, second.offset);
                     });
    for (const auto* child : children) self(self, *child);
  };
  walk(walk, root);
}

PortableDeclarationKey PortableDeclarationKey::fromDeclaration(
    DeclarationId id, const DeclarationTable& table) {
  const auto& record = table.get(id);
  if (record.portableKey) return *record.portableKey;
  if (record.specialization) {
    const auto& instance = *record.specialization;
    auto origin = fromDeclaration(instance.source, table);
    if (instance.enclosing)
      origin = inInstance(origin, fromDeclaration(instance.enclosing, table));
    std::vector<PortableTypeKey> arguments;
    for (const auto& type : instance.arguments) {
      if (!type)
        logAndThrowError("Portable specialization has an unresolved argument");
      arguments.push_back(PortableTypeKey::fromType(*type, table));
    }
    std::optional<std::vector<PortableTypeKey>> variadic;
    if (instance.variadic) {
      variadic.emplace();
      for (const auto& type : *instance.variadic) {
        if (!type)
          logAndThrowError(
              "Portable specialization has an unresolved pack element");
        variadic->push_back(PortableTypeKey::fromType(*type, table));
      }
    }
    return specialization(origin, arguments, variadic);
  }
  if (record.origin && record.owner) {
    auto origin = fromDeclaration(record.origin, table);
    auto owner = fromDeclaration(record.owner, table);
    auto member = inInstance(origin, owner);
    if (!record.generatedRole.empty())
      return generated(member, record.generatedRole, record.generatedSlot);
    if (record.kind != table.get(record.origin).kind)
      logAndThrowError(
          "Generated portable declaration requires an explicit role");
    return member;
  }
  logAndThrowError("Declaration has no portable source identity: " +
                   record.name);
}

PortableTypeKey PortableTypeKey::fromType(const Type& type,
                                          const DeclarationTable& table) {
  if (type.isPrimitive()) return primitive(type.toString());
  auto encode = [&](const TypePtr& value) {
    if (!value) logAndThrowError("Cannot export an unresolved type identity");
    return fromType(*value, table);
  };
  auto parameters = [&](const auto& callable) {
    std::vector<PortableTypeKey> result;
    for (const auto& param : callable.getParamTypes())
      result.push_back(encode(param));
    return result;
  };
  switch (type.getKind()) {
    case Type::Kind::Slice:
      return primitive("slice");
    case Type::Kind::NullPointer:
      return primitive("null");
    case Type::Kind::Class:
    case Type::Kind::Interface:
    case Type::Kind::Enum: {
      const auto& value = static_cast<const NominalType&>(type);
      if (!value.belongsTo(table))
        logAndThrowError(
            "Portable type identity belongs to another analysis session");
      return nominal(PortableDeclarationKey::fromDeclaration(
          value.getDeclarationId(), table));
    }
    case Type::Kind::Reference: {
      const auto& value = static_cast<const ReferenceType&>(type);
      return reference(encode(value.getReferencedType()), value.isMutable());
    }
    case Type::Kind::RawPointer:
      return pointer(
          encode(static_cast<const RawPointerType&>(type).getPointeeType()),
          false);
    case Type::Kind::StaticPointer:
      return pointer(
          encode(static_cast<const StaticPointerType&>(type).getPointeeType()),
          true);
    case Type::Kind::Array: {
      const auto& value = static_cast<const ArrayType&>(type);
      return array(
          encode(value.getElementType()),
          {value.getDimensions().begin(), value.getDimensions().end()});
    }
    case Type::Kind::Function: {
      const auto& value = static_cast<const FunctionType&>(type);
      return function(encode(value.getReturnType()), parameters(value),
                      value.canThrow(), false, false, value.requiresUnsafe());
    }
    case Type::Kind::Lambda: {
      const auto& value = static_cast<const LambdaType&>(type);
      return function(encode(value.getReturnType()), parameters(value),
                      value.canThrow(), true, value.hasRefCaptures(),
                      value.requiresUnsafe());
    }
    case Type::Kind::ErrorUnion:
      return errorUnion(
          encode(static_cast<const ErrorUnionType&>(type).getValueType()));
    default:
      logAndThrowError("Portable identity requires a concrete value type");
  }
}

PortableDeclarationKey PortableDeclarationKey::fromString(
    const std::string& value) {
  if (value.empty())
    logAndThrowError("Portable declaration identity is not assigned");
  return PortableDeclarationKey(value);
}

PortableDeclarationKey PortableDeclarationKey::original(
    const std::string& bundleHash, uint64_t declarationNumber) {
  if (bundleHash.size() != 64 ||
      !std::all_of(bundleHash.begin(), bundleHash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }))
    logAndThrowError("Bundle identity must be a full lowercase SHA-256 digest");
  if (!declarationNumber)
    logAndThrowError("Bundle declaration number is unassigned");
  return PortableDeclarationKey("$" + bundleHash + "$_" +
                                std::to_string(declarationNumber));
}

PortableDeclarationKey PortableDeclarationKey::specialization(
    const PortableDeclarationKey& origin,
    const std::vector<PortableTypeKey>& arguments,
    const std::optional<std::vector<PortableTypeKey>>& variadicArguments) {
  return PortableDeclarationKey(derivedKey(
      'S', {require(origin), types(arguments),
            variadicArguments ? types(*variadicArguments) : tuple('V', {})}));
}

PortableDeclarationKey PortableDeclarationKey::inInstance(
    const PortableDeclarationKey& origin,
    const PortableDeclarationKey& instance) {
  return PortableDeclarationKey(
      derivedKey('I', {require(origin), require(instance)}));
}

PortableDeclarationKey PortableDeclarationKey::generated(
    const PortableDeclarationKey& origin, const std::string& role,
    uint64_t slot) {
  if (role.empty())
    logAndThrowError("Generated declaration role must not be empty");
  return PortableDeclarationKey(
      derivedKey('G', {require(origin), role, integer(slot)}));
}

std::string PortableDeclarationKey::symbol(const std::string& role) const {
  if (role.empty()) logAndThrowError("Symbol emission role must not be empty");
  llvm::SHA256 sha;
  sha.update(tuple('E', {"SUN1", require(*this), role}));
  std::string result = "_SUN1_";
  constexpr char hex[] = "0123456789abcdef";
  for (auto byte : sha.final()) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}

PortableTypeKey PortableTypeKey::primitive(const std::string& name) {
  static const std::set<std::string> names = {
      "void", "bool", "i8",  "i16", "i32",  "i64",   "u8",  "u16",
      "u32",  "u64",  "f32", "f64", "char", "slice", "null"};
  if (!names.count(name))
    logAndThrowError("Unknown primitive in portable type identity: " + name);
  return PortableTypeKey(tuple('P', {name}));
}

PortableTypeKey PortableTypeKey::nominal(
    const PortableDeclarationKey& declaration) {
  return PortableTypeKey(tuple('N', {require(declaration)}));
}

PortableTypeKey PortableTypeKey::reference(const PortableTypeKey& value,
                                           bool isMutable) {
  return PortableTypeKey(tuple('R', {value.encoding(), integer(isMutable)}));
}

PortableTypeKey PortableTypeKey::pointer(const PortableTypeKey& value,
                                         bool isStatic) {
  return PortableTypeKey(tuple('Q', {value.encoding(), integer(isStatic)}));
}

PortableTypeKey PortableTypeKey::array(
    const PortableTypeKey& element, const std::vector<uint64_t>& dimensions) {
  std::vector<std::string> fields{element.encoding()};
  for (auto dimension : dimensions) fields.push_back(integer(dimension));
  return PortableTypeKey(tuple('A', fields));
}

PortableTypeKey PortableTypeKey::function(
    const PortableTypeKey& result,
    const std::vector<PortableTypeKey>& parameters, bool canThrow,
    bool isLambda, bool hasRefEnvironment, bool requiresUnsafe) {
  if (hasRefEnvironment && !isLambda)
    logAndThrowError("Only lambda types can carry a reference environment");
  return PortableTypeKey(
      tuple('F', {result.encoding(), types(parameters), integer(canThrow),
                  integer(isLambda), integer(hasRefEnvironment),
                  integer(requiresUnsafe)}));
}

PortableTypeKey PortableTypeKey::errorUnion(const PortableTypeKey& value) {
  return PortableTypeKey(tuple('U', {value.encoding()}));
}

}  // namespace sun::semantic_analysis
