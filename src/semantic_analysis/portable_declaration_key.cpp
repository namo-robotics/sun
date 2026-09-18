#include "semantic_analysis/portable_declaration_key.h"

#include <algorithm>
#include <set>

#include "ast.h"
#include "ast/ast_children.h"
#include "llvm/Support/SHA256.h"
#include "semantic_analysis/types.h"
#include "support/error.h"

namespace sun {
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
    table.bindPortable(id, key);
  };
  auto identity = [&](const DeclarationIdentity& value) {
    assign(value.id);
    for (auto id : value.typeParameters) assign(id);
    for (auto id : value.lifetimeParameters) assign(id);
    for (auto id : value.parameters) assign(id);
  };
  auto walk = [&](auto&& self, const ExprAST& node) -> void {
    if (auto* moon = dynamic_cast<const MoonScopeAST*>(&node)) {
      if (!moon->isOwnBundle()) return;
    } else if (node.getDeclarationId())
      identity(node.declarationIdentity());
    if (auto* cls = dynamic_cast<const ClassDefinitionAST*>(&node))
      for (const auto& field : cls->getFields()) identity(field.declaration);
    if (auto* iface = dynamic_cast<const InterfaceDefinitionAST*>(&node))
      for (const auto& field : iface->getFields()) identity(field.declaration);
    if (auto* enumeration = dynamic_cast<const EnumDefinitionAST*>(&node))
      for (const auto& variant : enumeration->getVariants())
        identity(variant.declaration);
    if (auto* match = dynamic_cast<const MatchExprAST*>(&node))
      for (const auto& arm : match->getArms())
        for (const auto& binding : arm.bindings) identity(binding.declaration);
    if (auto* expression = dynamic_cast<const TryCatchExprAST*>(&node))
      for (const auto& clause : expression->getCatchClauses())
        identity(clause.declaration);
    std::vector<const ExprAST*> children;
    forEachChild(node,
                 [&](const ExprAST& child) { children.push_back(&child); });
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

PortableDeclarationKey PortableDeclarationKey::parse(
    const std::string& encoded) {
  auto invalid = [] {
    logAndThrowError("Malformed portable declaration identity");
  };
  auto validate = [&](auto&& self, std::string_view bytes,
                      std::string_view allowed, unsigned depth) -> void {
    if (depth > 128 || bytes.size() < 9 ||
        allowed.find(bytes[0]) == std::string_view::npos)
      invalid();
    size_t offset = 1;
    auto number = [&]() {
      if (bytes.size() - offset < 8) invalid();
      uint64_t value = 0;
      for (int i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[offset++]);
      return value;
    };
    const auto count = number();
    if (count > (bytes.size() - offset) / 8) invalid();
    std::vector<std::string_view> fields;
    for (uint64_t i = 0; i < count; ++i) {
      const auto length = number();
      if (length > bytes.size() - offset) invalid();
      fields.push_back(bytes.substr(offset, length));
      offset += length;
    }
    if (offset != bytes.size()) invalid();
    auto arity = [&](size_t size) {
      if (fields.size() != size) invalid();
    };
    auto child = [&](size_t index, std::string_view kinds) {
      self(self, fields[index], kinds, depth + 1);
    };
    auto integerField = [&](size_t index, bool boolean = false) {
      if (fields[index].size() != 8) invalid();
      if (boolean && fields[index] != integer(0) && fields[index] != integer(1))
        invalid();
    };
    constexpr std::string_view declarations = "DSIG";
    constexpr std::string_view types = "PNRQAFU";
    switch (bytes[0]) {
      case 'D':
        parseOriginal(std::string(bytes));
        break;
      case 'S':
        arity(3);
        child(0, declarations);
        child(1, "L");
        child(2, "LV");
        break;
      case 'I':
        arity(2);
        child(0, declarations);
        child(1, declarations);
        break;
      case 'G':
        arity(3);
        child(0, declarations);
        if (fields[1].empty()) invalid();
        integerField(2);
        break;
      case 'P':
        arity(1);
        PortableTypeKey::primitive(std::string(fields[0]));
        break;
      case 'N':
        arity(1);
        child(0, declarations);
        break;
      case 'R':
      case 'Q':
        arity(2);
        child(0, types);
        integerField(1, true);
        break;
      case 'A':
        if (fields.empty()) invalid();
        child(0, types);
        for (size_t i = 1; i < fields.size(); ++i) integerField(i);
        break;
      case 'F':
        arity(6);
        child(0, types);
        child(1, "L");
        for (size_t i = 2; i < fields.size(); ++i) integerField(i, true);
        if (fields[4] == integer(1) && fields[3] != integer(1)) invalid();
        break;
      case 'U':
        arity(1);
        child(0, types);
        break;
      case 'V':
        arity(0);
        break;
      case 'L':
        for (size_t i = 0; i < fields.size(); ++i) child(i, types);
        break;
      default:
        invalid();
    }
  };
  validate(validate, encoded, "DSIG", 0);
  return PortableDeclarationKey(encoded);
}

PortableDeclarationKey PortableDeclarationKey::parseOriginal(
    const std::string& encoded) {
  constexpr size_t ordinalOffset = 89;
  if (encoded.size() != ordinalOffset + 8)
    logAndThrowError("Malformed original declaration identity");
  uint64_t ordinal = 0;
  for (size_t i = ordinalOffset; i < encoded.size(); ++i)
    ordinal = (ordinal << 8) | static_cast<unsigned char>(encoded[i]);
  auto key = original(encoded.substr(17, 64), ordinal);
  if (key.encoding() != encoded)
    logAndThrowError("Malformed original declaration identity");
  return key;
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
  return PortableDeclarationKey(
      tuple('D', {bundleHash, integer(declarationNumber)}));
}

PortableDeclarationKey PortableDeclarationKey::specialization(
    const PortableDeclarationKey& origin,
    const std::vector<PortableTypeKey>& arguments,
    const std::optional<std::vector<PortableTypeKey>>& variadicArguments) {
  return PortableDeclarationKey(tuple(
      'S', {require(origin), types(arguments),
            variadicArguments ? types(*variadicArguments) : tuple('V', {})}));
}

PortableDeclarationKey PortableDeclarationKey::inInstance(
    const PortableDeclarationKey& origin,
    const PortableDeclarationKey& instance) {
  return PortableDeclarationKey(
      tuple('I', {require(origin), require(instance)}));
}

PortableDeclarationKey PortableDeclarationKey::generated(
    const PortableDeclarationKey& origin, const std::string& role,
    uint64_t slot) {
  if (role.empty())
    logAndThrowError("Generated declaration role must not be empty");
  return PortableDeclarationKey(
      tuple('G', {require(origin), role, integer(slot)}));
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

}  // namespace sun
