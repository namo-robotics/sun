#pragma once

/** Defines shared type descriptions used by portable identities. */
namespace sun::types {
class Type;
}

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
class ExprAST;
}

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {}

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

class DeclarationId;
class DeclarationTable;

/** Structural type identity for independently compiled generic instances. */
class PortableTypeKey {
  std::string encoded_;
  /** Stores an encoded type identity that can cross compilation sessions. */
  explicit PortableTypeKey(std::string encoded)
      : encoded_(std::move(encoded)) {}

 public:
  /** Encode a concrete semantic type at an artifact boundary. */
  static PortableTypeKey fromType(const sun::types::Type& type,
                                  const DeclarationTable& table);
  /** Identify a primitive by its stable language spelling. */
  static PortableTypeKey primitive(const std::string& name);
  /** Identify a nominal type without recursively expanding its fields. */
  static PortableTypeKey nominal(const DeclarationId& declaration);
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
  /** Compares the stored values for equality. */
  bool operator==(const PortableTypeKey& other) const {
    return encoded_ == other.encoded_;
  }
};

}  // namespace sun::semantic_analysis
