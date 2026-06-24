// qualified_name.h - Qualified name with dual representation for Sun language
//
// A QualifiedName keeps scope path and symbol name separate to provide
// both mangled names (for codegen) and display names (for error messages).
//
// The scopePath is a vector of path segments (module names, class names, etc.).
// For moon imports, the library content hash is encoded as a scope segment
// (e.g., {"$abc123$", "sun", "submodule"}).
//
// All name mangling logic is centralized here:
// - extractHashPrefix: get "$hash$_" prefix from a mangled name
// - canonicalTypeString: stable type string for mangling (strips hash prefixes)
// - buildParamSuffix: "$type1$type2$..." for overload disambiguation

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sun {
class Type;  // Forward declaration
using TypePtr = std::shared_ptr<Type>;

// A qualified name with both mangled and display representations
// Keeps scope path and symbol name separate to avoid lossy conversion
struct QualifiedName {
  // Scope path segments, e.g., {"module", "submodule"} or empty for global
  std::vector<std::string> scopePath;
  std::string baseName;  // "my_func" (the original identifier, may contain _)
  std::string paramSuffix;  // "$i32$ref_String_" for overload disambiguation

  QualifiedName() = default;
  QualifiedName(std::vector<std::string> path, std::string name)
      : scopePath(std::move(path)), baseName(std::move(name)) {}

  // Get mangled form for codegen/lookup: "$hash$_A_B_my_func$i32$ref_String_"
  // Joins scope path with underscores, appends paramSuffix for overloads
  std::string mangled() const {
    std::string result;
    if (scopePath.empty()) {
      result = baseName;
    } else {
      for (const auto& segment : scopePath) {
        if (!result.empty()) result += "_";
        result += segment;
      }
      result += "_" + baseName;
    }
    result += paramSuffix;
    return result;
  }

  // Get display form for error messages: "A.B.my_func"
  // Note: Library hash scopes (starting with $) are filtered out for cleaner
  // display
  std::string display() const {
    if (scopePath.empty()) return baseName;

    // Filter out library hash scopes from display (they start with $)
    std::string displayPath;
    for (const auto& segment : scopePath) {
      // Skip hash segments (start with $)
      if (!segment.empty() && segment[0] != '$') {
        if (!displayPath.empty()) displayPath += ".";
        displayPath += segment;
      }
    }
    if (displayPath.empty()) return baseName;
    return displayPath + "." + baseName;
  }

  bool empty() const {
    return scopePath.empty() && baseName.empty() && paramSuffix.empty();
  }

  bool operator==(const QualifiedName& other) const {
    return scopePath == other.scopePath && baseName == other.baseName &&
           paramSuffix == other.paramSuffix;
  }

  bool operator!=(const QualifiedName& other) const {
    return !(*this == other);
  }

  bool operator<(const QualifiedName& other) const {
    if (scopePath != other.scopePath) return scopePath < other.scopePath;
    if (baseName != other.baseName) return baseName < other.baseName;
    return paramSuffix < other.paramSuffix;
  }

  // Get scope path as dot-separated string (for compatibility/display)
  std::string scopePathString() const {
    std::string result;
    for (const auto& segment : scopePath) {
      if (!result.empty()) result += ".";
      result += segment;
    }
    return result;
  }

  // Static helper to join a scope path vector into dot-separated string
  static std::string joinPath(const std::vector<std::string>& path) {
    std::string result;
    for (const auto& segment : path) {
      if (!result.empty()) result += ".";
      result += segment;
    }
    return result;
  }

  // =========================================================================
  // Centralized name mangling utilities
  // =========================================================================

  // Extract "$hash$_" prefix from a mangled name.
  // Returns empty string if no hash prefix found.
  // E.g., "$abc123$_sun_Foo" -> "$abc123$_"
  static std::string extractHashPrefix(const std::string& name) {
    if (name.size() > 2 && name[0] == '$') {
      size_t secondDollar = name.find('$', 1);
      if (secondDollar != std::string::npos && secondDollar + 1 < name.size() &&
          name[secondDollar + 1] == '_') {
        return name.substr(0, secondDollar + 2);  // includes "$hash$_"
      }
    }
    return "";
  }

  // Get the hash prefix from this qualified name's scope path.
  // Looks for first segment starting with '$'.
  std::string getHashPrefix() const {
    for (const auto& segment : scopePath) {
      if (!segment.empty() && segment[0] == '$') {
        return segment + "_";
      }
    }
    return "";
  }

  // Get canonical type string for mangling.
  // Strips hash prefix from class names when it matches the given prefix,
  // ensuring that types within the same library are mangled without hashes.
  // This produces stable names for overload disambiguation.
  //
  // - Classes: use mangledName, strip matching hash prefix
  // - References: "ref_<inner>_"
  // - Pointers: "raw_ptr_<inner>_" / "static_ptr_<inner>_"
  // - Arrays: "array_<inner>_"
  // - Primitives/other: use toString()
  static std::string canonicalTypeString(const TypePtr& type,
                                         const std::string& hashPrefix);

  // Build param type suffix string for overload disambiguation.
  // Format: "$paramType1$paramType2$..."
  // Uses canonicalTypeString with hash prefix stripping.
  static std::string buildParamSuffix(const std::vector<TypePtr>& paramTypes,
                                      const std::string& hashPrefix = "");

  // Build a suffix that keys a variadic generic-method specialization by its
  // actual variadic argument types, so calls with different arities/types
  // resolve to distinct specializations. Format: "$v$type1$type2$...". Empty
  // when there are no variadic args (leaving non-variadic names unchanged).
  // Must be computed identically by semantic analysis and codegen.
  static std::string buildVariadicArgSuffix(
      const std::vector<TypePtr>& variadicArgTypes,
      const std::string& hashPrefix = "");

  // Set param suffix from resolved param types.
  // Automatically derives hash prefix from this name's scope path.
  void setParamSuffix(const std::vector<TypePtr>& paramTypes) {
    paramSuffix = buildParamSuffix(paramTypes, getHashPrefix());
  }
};

}  // namespace sun

// Hash specialization for std::unordered_map/set support
template <>
struct std::hash<sun::QualifiedName> {
  size_t operator()(const sun::QualifiedName& qn) const noexcept {
    size_t h = std::hash<std::string>{}(qn.baseName);
    for (const auto& seg : qn.scopePath) {
      h ^= std::hash<std::string>{}(seg) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    h ^= std::hash<std::string>{}(qn.paramSuffix) + 0x9e3779b9 + (h << 6) +
         (h >> 2);
    return h;
  }
};
