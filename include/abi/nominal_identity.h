#pragma once

#include <string>
#include <vector>

/** Describes nominal identity separately from exact implementation artifacts.
 */
namespace sun::abi {
/** Distinguishes nominal declaration kinds even when their names match. */
enum class NominalKind { Class, Interface, Enum };

/** Identifies a declaration across revisions of an explicitly named library.
 * Equality identifies a candidate correspondence, not ABI compatibility.
 * PortableDeclarationKey continues to identify exact code and linker symbols.
 */
class NominalIdentity {
 public:
  /** Creates an identity from a library namespace and an ordered declaration
   * path, including enclosing modules/types. Empty names and paths are
   * rejected. The caller supplies a persistent library identity, never a
   * version, source filename, artifact digest, or module name inferred from a
   * candidate bundle.
   */
  NominalIdentity(const std::string& library, NominalKind kind,
                  const std::vector<std::string>& path);

  /** Returns the canonical experimental identity, independent of code bytes. */
  const std::string& encoding() const { return encoded_; }

  /** Reports the same nominal origin without authorizing a replacement. */
  bool operator==(const NominalIdentity&) const = default;

 private:
  std::string encoded_;
};
}  // namespace sun::abi
