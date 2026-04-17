#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "moon.h"
#include "types.h"

namespace sun {

/// Reconstructs Sun type objects from serialized type signatures
/// Used to enable semantic analysis on precompiled imports without AST
class ModuleTypeResolver {
 public:
  /// Parse a type signature string into a Type
  /// Examples:
  ///   "i32" -> PrimitiveType(Int32)
  ///   "f64" -> PrimitiveType(Float64)
  ///   "(i32, i32) -> i32" -> FunctionType
  ///   "ptr<Point>" -> PointerType
  /// @param sig The type signature string
  /// @return Parsed type, or nullptr if invalid
  static TypePtr parseTypeSignature(const std::string& sig);

  /// Serialize a type to a signature string
  /// @param type The type to serialize
  /// @return Type signature string
  static std::string serializeType(const TypePtr& type);

  /// Build a symbol table from module metadata exports
  /// @param metadata Module metadata containing exports
  /// @return Map from symbol name to type
  static std::unordered_map<std::string, TypePtr> buildExportTable(
      const ModuleMetadata& metadata);

 private:
  /// Parse helper - advance position and return parsed type
  static TypePtr parseType(const std::string& sig, size_t& pos);

  /// Skip whitespace
  static void skipWhitespace(const std::string& sig, size_t& pos);

  /// Parse an identifier (type name, parameter name, etc.)
  static std::string parseIdentifier(const std::string& sig, size_t& pos);

  /// Check if we're at a specific character
  static bool match(const std::string& sig, size_t& pos, char c);

  /// Parse function type: (params) -> return
  static TypePtr parseFunctionType(const std::string& sig, size_t& pos);

  /// Parse generic type: Name<T1, T2>
  static TypePtr parseGenericType(const std::string& name,
                                  const std::string& sig, size_t& pos);
};

}  // namespace sun
