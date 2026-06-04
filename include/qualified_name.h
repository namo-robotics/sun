// qualified_name.h - Qualified name with dual representation for Sun language
//
// A QualifiedName keeps scope key and symbol name separate to provide
// both mangled names (for codegen) and display names (for error messages).
//
// The scopeKey contains the module path using "." separators.
// For moon imports, the library content hash is encoded as a module scope
// in the module path (e.g., "$abc123$.sun.submodule").

#pragma once

#include <string>

namespace sun {

// A qualified name with both mangled and display representations
// Keeps scope key and symbol name separate to avoid lossy conversion
struct QualifiedName {
  // Module path using "." separators, or empty for global scope.
  std::string scopeKey;
  std::string baseName;  // "my_func" (the original identifier, may contain _)

  QualifiedName() = default;
  QualifiedName(std::string key, std::string name)
      : scopeKey(std::move(key)), baseName(std::move(name)) {}

  // Construct from a mangled name when we only have the mangled form
  // This is a fallback - prefer constructing with scope key + base name
  static QualifiedName fromMangled(const std::string& mangledName) {
    return QualifiedName("", mangledName);
  }

  // Get mangled form for codegen/lookup: "$hash$_A_B_my_func"
  // Replaces dots with underscores in module path
  std::string mangled() const {
    if (scopeKey.empty()) {
      return baseName;
    }

    // Mangle module path: replace dots with underscores
    std::string result = scopeKey;
    for (char& c : result) {
      if (c == '.') c = '_';
    }
    return result + "_" + baseName;
  }

  // Get display form for error messages: "A.B.my_func"
  // Note: Library hash scopes (starting with $) are filtered out for cleaner
  // display
  std::string display() const {
    if (scopeKey.empty()) return baseName;

    // Filter out library hash scopes from display (they start with $)
    std::string displayPath;
    size_t pos = 0;
    while (pos < scopeKey.size()) {
      size_t dot = scopeKey.find('.', pos);
      std::string segment = (dot == std::string::npos)
                                ? scopeKey.substr(pos)
                                : scopeKey.substr(pos, dot - pos);
      // Skip hash segments (start with $)
      if (!segment.empty() && segment[0] != '$') {
        if (!displayPath.empty()) displayPath += ".";
        displayPath += segment;
      }
      pos = (dot == std::string::npos) ? scopeKey.size() : dot + 1;
    }
    if (displayPath.empty()) return baseName;
    return displayPath + "." + baseName;
  }

  bool empty() const { return scopeKey.empty() && baseName.empty(); }

  bool operator==(const QualifiedName& other) const {
    return scopeKey == other.scopeKey && baseName == other.baseName;
  }

  bool operator!=(const QualifiedName& other) const {
    return !(*this == other);
  }

  bool operator<(const QualifiedName& other) const {
    if (scopeKey != other.scopeKey) return scopeKey < other.scopeKey;
    return baseName < other.baseName;
  }
};

}  // namespace sun
