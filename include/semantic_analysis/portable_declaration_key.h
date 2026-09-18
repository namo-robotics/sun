#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "semantic_analysis/declaration_id.h"

namespace sun {

class PortableDeclarationKey;
class DeclarationTable;
class Type;

/** Structural type identity for independently compiled generic instances. */
class PortableTypeKey {
  std::string encoded_;
  explicit PortableTypeKey(std::string encoded)
      : encoded_(std::move(encoded)) {}

 public:
  /** Encode a concrete semantic type at an artifact boundary. */
  static PortableTypeKey fromType(const Type& type,
                                  const DeclarationTable& table);
  /** Identify a primitive by its stable language spelling. */
  static PortableTypeKey primitive(const std::string& name);
  /** Identify a nominal type without recursively expanding its fields. */
  static PortableTypeKey nominal(const PortableDeclarationKey& declaration);
  /** Identify a mutable or immutable reference to another type. */
  static PortableTypeKey reference(const PortableTypeKey& value,
                                   bool isMutable);
  /** Identify a raw or static pointer to another type. */
  static PortableTypeKey pointer(const PortableTypeKey& value, bool isStatic);
  /** Identify an array by its element type and every dimension. */
  static PortableTypeKey array(const PortableTypeKey& element,
                               const std::vector<uint64_t>& dimensions);
  /** Identify a function signature, including its error and closure behavior.
   */
  static PortableTypeKey function(
      const PortableTypeKey& result,
      const std::vector<PortableTypeKey>& parameters, bool canThrow,
      bool isLambda = false, bool hasRefEnvironment = false,
      bool requiresUnsafe = false);
  /** Identify a value paired with an error alternative. */
  static PortableTypeKey errorUnion(const PortableTypeKey& value);
  /** Return the canonical structural encoding. */
  const std::string& encoding() const { return encoded_; }
  bool operator==(const PortableTypeKey& other) const {
    return encoded_ == other.encoded_;
  }
};

/** Identifies a declaration independently of any compiler session. */
class PortableDeclarationKey {
  std::string encoded_;
  explicit PortableDeclarationKey(std::string encoded)
      : encoded_(std::move(encoded)) {}

 public:
  /** Construct an unset key for declarations not yet assigned an artifact. */
  PortableDeclarationKey() = default;
  /** Derive a key from assigned source keys and concrete specialization inputs.
   */
  static PortableDeclarationKey fromDeclaration(DeclarationId id,
                                                const DeclarationTable& table);
  /** Identify an original declaration in a content-addressed artifact. */
  static PortableDeclarationKey original(const std::string& bundleHash,
                                         uint64_t declarationNumber);
  /** Identify an instance by its template and concrete argument types. */
  static PortableDeclarationKey specialization(
      const PortableDeclarationKey& origin,
      const std::vector<PortableTypeKey>& arguments,
      const std::optional<std::vector<PortableTypeKey>>& variadicArguments =
          std::nullopt);
  /** Identify a declaration cloned within one specific enclosing instance. */
  static PortableDeclarationKey inInstance(
      const PortableDeclarationKey& origin,
      const PortableDeclarationKey& instance);
  /** Identify a generated declaration by a stable role within its origin. */
  static PortableDeclarationKey generated(const PortableDeclarationKey& origin,
                                          const std::string& role,
                                          uint64_t slot);
  /** Return canonical bytes interpreted under the enclosing ABI version. */
  const std::string& encoding() const { return encoded_; }
  /** Report whether the portable identity has been established. */
  bool empty() const { return encoded_.empty(); }
  /** Derive a linker symbol for one explicitly named emission role. */
  std::string symbol(const std::string& role) const;
  bool operator==(const PortableDeclarationKey& other) const {
    return encoded_ == other.encoded_;
  }
  bool operator<(const PortableDeclarationKey& other) const {
    return encoded_ < other.encoded_;
  }
};

}  // namespace sun
