// qualified_name.h - Qualified name with dual representation for Sun language
//
// A QualifiedName keeps scope path and symbol name separate to provide
// both mangled names (for codegen) and display names (for error messages).
//
// The scopePath is a vector of path segments (module names, class names, etc.).
// For moon imports, the library content hash is encoded as a scope segment
// (e.g., {"$abc123$", "sun", "submodule"}).

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sun {

// A qualified name with both mangled and display representations
// Keeps scope path and symbol name separate to avoid lossy conversion
struct QualifiedName {
  // Scope path segments, e.g., {"module", "submodule"} or empty for global
  std::vector<std::string> scopePath;
  std::string baseName;  // "my_func" (the original identifier, may contain _)

  QualifiedName() = default;
  QualifiedName(std::vector<std::string> path, std::string name)
      : scopePath(std::move(path)), baseName(std::move(name)) {}

  // Get mangled form for codegen/lookup: "$hash$_A_B_my_func"
  // Joins scope path with underscores
  std::string mangled() const {
    if (scopePath.empty()) {
      return baseName;
    }

    std::string result;
    for (const auto& segment : scopePath) {
      if (!result.empty()) result += "_";
      result += segment;
    }
    return result + "_" + baseName;
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

  bool empty() const { return scopePath.empty() && baseName.empty(); }

  bool operator==(const QualifiedName& other) const {
    return scopePath == other.scopePath && baseName == other.baseName;
  }

  bool operator!=(const QualifiedName& other) const { return !(*this == other); }

  bool operator<(const QualifiedName& other) const {
    if (scopePath != other.scopePath) return scopePath < other.scopePath;
    return baseName < other.baseName;
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
    return h;
  }
};
