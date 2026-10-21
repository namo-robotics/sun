#include "abi/nominal_identity.h"

#include <llvm/Support/SHA256.h>

#include <cstdint>

#include "support/error.h"

/** Describes nominal identity separately from exact implementation artifacts.
 */
namespace sun::abi {
/** Keeps canonical encoding details private to this implementation. */
namespace {
/** Frames each component so path segmentation and arbitrary names stay
 * distinct. */
void append(std::string& bytes, const std::string& value) {
  const uint64_t length = value.size();
  for (unsigned shift = 0; shift < 64; shift += 8)
    bytes.push_back(static_cast<char>(length >> shift));
  bytes += value;
}

/** Uses explicit kind names rather than compiler enum ordinals. */
std::string kindName(NominalKind kind) {
  switch (kind) {
    case NominalKind::Class:
      return "class";
    case NominalKind::Interface:
      return "interface";
    case NominalKind::Enum:
      return "enum";
  }
  sun::support::logAndThrowError("Unknown nominal declaration kind");
}
}  // namespace

NominalIdentity::NominalIdentity(const std::string& library, NominalKind kind,
                                 const std::vector<std::string>& path) {
  if (library.empty() || path.empty())
    sun::support::logAndThrowError(
        "Nominal identity requires a library and declaration path");
  std::string bytes;
  append(bytes, "sun.abi.nominal-identity.v1");
  append(bytes, library);
  append(bytes, kindName(kind));
  for (const auto& component : path) {
    if (component.empty())
      sun::support::logAndThrowError(
          "Empty nominal declaration path component");
    append(bytes, component);
  }
  llvm::SHA256 hash;
  hash.update(bytes);
  encoded_ = "sun.nominal.v1:";
  constexpr char hex[] = "0123456789abcdef";
  for (auto byte : hash.final()) {
    encoded_ += hex[byte >> 4];
    encoded_ += hex[byte & 15];
  }
}
}  // namespace sun::abi
