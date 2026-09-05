// qualified_name.h - Qualified name with dual representation for Sun language
//
// A QualifiedName keeps scope path and symbol name separate to provide
// both mangled names (for codegen) and display names (for error messages).
//
// The scopePath is a vector of path segments (module names, class names, etc.).
// For moon imports, the library content hash is encoded as a scope segment
// (e.g., {"$abc123$", "std", "submodule"}).
//
// All name mangling logic is centralized here:
// - extractHashPrefix: get "$hash$_" prefix from a mangled name
// - canonicalTypeString: stable type string for mangling
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
  // Scope path segments, e.g., {"module", "submodule"} or empty for global.
  // May include enclosing class/function segments for nested items.
  std::vector<std::string> scopePath;
  std::string baseName;  // "my_func" (the original identifier, may contain _)
  std::string paramSuffix;  // "$i32$ref_String_" for overload disambiguation
  // The module that declared the item — the unit of visibility. Unlike
  // scopePath it never contains class/function segments; empty = root.
  std::vector<std::string> modulePath;

  QualifiedName() = default;
  QualifiedName(std::vector<std::string> path, std::string name)
      : scopePath(std::move(path)), baseName(std::move(name)) {}
  QualifiedName(std::vector<std::string> path, std::string name,
                std::vector<std::string> owner)
      : scopePath(std::move(path)),
        baseName(std::move(name)),
        modulePath(std::move(owner)) {}

  const std::vector<std::string>& owner() const { return modulePath; }

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

  /** Return the dotted lookup name, including the bundle scope. */
  std::string lookupName() const {
    return (scopePath.empty() ? baseName : scopePathString() + "." + baseName) +
           paramSuffix;
  }

  /** Return the defining bundle hash, or empty for an unbundled name. */
  std::string bundleHash() const {
    if (scopePath.empty()) return "";
    const auto& scope = scopePath.front();
    if (scope.size() < 3 || scope.front() != '$' || scope.back() != '$')
      return "";
    return scope.substr(1, scope.size() - 2);
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

  // The one way a type is spelled inside a symbol name: overload suffixes
  // and the type arguments of every specialization use it. A bundle and its
  // importers name every type identically (library hash included), so the
  // result is the same on both sides of a .moon boundary. Symbol-safe: any
  // punctuation in the spelling becomes '_'.
  //
  // - Classes / interfaces: their mangled name
  // - References: "ref_<inner>_" / "const_ref_<inner>_"
  // - Pointers: "raw_ptr_<inner>_" / "static_ptr_<inner>_"
  // - Arrays: "array_<inner>_"
  // - Primitives/other: use toString()
  static std::string canonicalTypeString(const TypePtr& type);

  // Build param type suffix string for overload disambiguation.
  // Format: "$paramType1$paramType2$..."
  static std::string buildParamSuffix(const std::vector<TypePtr>& paramTypes);

  // Build a suffix that keys a variadic generic-method specialization by its
  // actual variadic argument types, so calls with different arities/types
  // resolve to distinct specializations. Format: "$v$type1$type2$...". Empty
  // when there are no variadic args (leaving non-variadic names unchanged).
  // Must be computed identically by semantic analysis and codegen.
  static std::string buildVariadicArgSuffix(
      const std::vector<TypePtr>& variadicArgTypes);

  // Name one specialization of a generic function or method. The template's
  // scope and module are the specialization's too; only the base name grows,
  // by the type arguments and then the pack's argument types when the
  // template ends in one — "make_vec_i32", "create_Point$v$$i32$i32".
  //
  // The single place this name is built. Semantic analysis names each
  // specialization here and records the result on the call, and codegen calls
  // that name; the type arguments are the specialization's identity, so it
  // carries no overload suffix of its own.
  static QualifiedName specializationOf(
      const QualifiedName& templateName, const std::vector<TypePtr>& typeArgs,
      const std::vector<TypePtr>& packArgTypes = {});

  // The name of `member` declared inside this one — the enclosing name
  // becomes a scope segment, as a class does for its methods.
  QualifiedName memberNamed(const std::string& member) const {
    QualifiedName result = *this;
    result.scopePath.push_back(result.baseName);
    result.baseName = member;
    result.paramSuffix.clear();
    return result;
  }

  // Set param suffix from resolved param types.
  void setParamSuffix(const std::vector<TypePtr>& paramTypes) {
    paramSuffix = buildParamSuffix(paramTypes);
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
