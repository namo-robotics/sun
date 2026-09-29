#pragma once

#include <functional>
#include <string>
#include <vector>

#include "semantic_analysis/module_path.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Describe a source name and its scope independently of declaration identity.
 */
struct QualifiedName {
  // Scope path segments, e.g., {"module", "submodule"} or empty for global.
  // May include enclosing class/function segments for nested items.
  std::vector<std::string> scopePath;
  std::string baseName;  // "my_func" (the original identifier, may contain _)
  /** Creates a declaration name from its enclosing path and local spelling. */
  QualifiedName() = default;
  /** Creates a declaration name from its enclosing path and local spelling. */
  QualifiedName(std::vector<std::string> path, std::string name)
      : scopePath(std::move(path)), baseName(std::move(name)) {}
  /**
   * Display form for error messages, "A.B.my_func", with library hash
   * segments left out.
   */
  std::string display() const {
    std::string displayPath = displayModulePath(scopePath);
    if (displayPath.empty()) return baseName;
    return displayPath + "." + baseName;
  }

  /** Reports whether this object contains no entries. */
  bool empty() const { return scopePath.empty() && baseName.empty(); }

  /** Compares the stored values for equality. */
  bool operator==(const QualifiedName& other) const {
    return scopePath == other.scopePath && baseName == other.baseName;
  }

  /** Reports whether the stored values differ. */
  bool operator!=(const QualifiedName& other) const {
    return !(*this == other);
  }

  /** Orders values for use in sorted containers. */
  bool operator<(const QualifiedName& other) const {
    if (scopePath != other.scopePath) return scopePath < other.scopePath;
    return baseName < other.baseName;
  }

  /**
   * Get scope path as dot-separated string (for compatibility/display)
   */
  std::string scopePathString() const { return joinPath(scopePath); }

  /** Return the dotted lookup name, including the bundle scope. */
  std::string lookupName() const {
    return scopePath.empty() ? baseName : scopePathString() + "." + baseName;
  }

  /** Return the defining bundle hash, or empty for an unbundled name. */
  std::string bundleHash() const {
    return scopePath.empty() ? "" : libraryHashOf(scopePath.front());
  }

  /**
   * Static helper to join a scope path vector into dot-separated string
   */
  static std::string joinPath(const std::vector<std::string>& path) {
    std::string result;
    for (const auto& segment : path) {
      if (!result.empty()) result += ".";
      result += segment;
    }
    return result;
  }

  /**
   * The name of `member` declared inside this one — the enclosing name
   * becomes a scope segment, as a class does for its methods.
   */
  QualifiedName memberNamed(const std::string& member) const {
    QualifiedName result = *this;
    result.scopePath.push_back(result.baseName);
    result.baseName = member;
    return result;
  }
};

}  // namespace sun::semantic_analysis

/**
 * Hash specialization for std::unordered_map/set support
 */
template <>
struct std::hash<sun::semantic_analysis::QualifiedName> {
  /** Computes a hash for the supplied value for use in unordered containers. */
  size_t operator()(
      const sun::semantic_analysis::QualifiedName& qn) const noexcept {
    size_t h = std::hash<std::string>{}(qn.baseName);
    for (const auto& seg : qn.scopePath) {
      h ^= std::hash<std::string>{}(seg) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
  }
};
