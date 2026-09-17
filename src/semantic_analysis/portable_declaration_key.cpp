#include "semantic_analysis/portable_declaration_key.h"

#include <algorithm>
#include <set>

#include "llvm/Support/SHA256.h"
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
    const std::vector<PortableTypeKey>& variadicArguments) {
  return PortableDeclarationKey(tuple(
      'S', {require(origin), types(arguments), types(variadicArguments)}));
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
    bool isLambda, bool hasRefEnvironment) {
  if (hasRefEnvironment && !isLambda)
    logAndThrowError("Only lambda types can carry a reference environment");
  return PortableTypeKey(
      tuple('F', {result.encoding(), types(parameters), integer(canThrow),
                  integer(isLambda), integer(hasRefEnvironment)}));
}

PortableTypeKey PortableTypeKey::errorUnion(const PortableTypeKey& value) {
  return PortableTypeKey(tuple('U', {value.encoding()}));
}

}  // namespace sun
