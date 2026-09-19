#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sun::semantic_analysis {

/** Describe a source name and its scope independently of declaration identity.
 */
struct QualifiedName {
  // Scope path segments, e.g., {"module", "submodule"} or empty for global.
  // May include enclosing class/function segments for nested items.
  std::vector<std::string> scopePath;
  std::string baseName;  // "my_func" (the original identifier, may contain _)
  QualifiedName() = default;
  QualifiedName(std::vector<std::string> path, std::string name)
      : scopePath(std::move(path)), baseName(std::move(name)) {}
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

  bool operator!=(const QualifiedName& other) const {
    return !(*this == other);
  }

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

  /** Return the dotted lookup name, including the bundle scope. */
  std::string lookupName() const {
    return scopePath.empty() ? baseName : scopePathString() + "." + baseName;
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

  // The name of `member` declared inside this one — the enclosing name
  // becomes a scope segment, as a class does for its methods.
  QualifiedName memberNamed(const std::string& member) const {
    QualifiedName result = *this;
    result.scopePath.push_back(result.baseName);
    result.baseName = member;
    return result;
  }
};

}  // namespace sun::semantic_analysis

// Hash specialization for std::unordered_map/set support
template <>
struct std::hash<sun::semantic_analysis::QualifiedName> {
  size_t operator()(
      const sun::semantic_analysis::QualifiedName& qn) const noexcept {
    size_t h = std::hash<std::string>{}(qn.baseName);
    for (const auto& seg : qn.scopePath) {
      h ^= std::hash<std::string>{}(seg) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
  }
};
