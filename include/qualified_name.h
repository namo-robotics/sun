// qualified_name.h - Qualified name with dual representation for Sun language
//
// A QualifiedName keeps module path and symbol name separate to provide
// both mangled names (for codegen) and display names (for error messages).

#pragma once

#include <string>

namespace sun {

// A qualified name with both mangled and display representations
// Keeps module path and symbol name separate to avoid lossy conversion
struct QualifiedName {
  std::string modulePath;  // "A.B" (dot-separated) or "" for global scope
  std::string baseName;    // "my_func" (the original identifier, may contain _)

  QualifiedName() = default;
  QualifiedName(std::string mod, std::string name)
      : modulePath(std::move(mod)), baseName(std::move(name)) {}

  // Construct from a mangled name when we only have the mangled form
  // This is a fallback - prefer constructing with module path + base name
  static QualifiedName fromMangled(const std::string& mangledName) {
    return QualifiedName("", mangledName);
  }

  // Get mangled form for codegen/lookup: "A_B_my_func"
  std::string mangled() const {
    if (modulePath.empty()) return baseName;
    std::string path = modulePath;
    for (char& c : path) {
      if (c == '.') c = '_';
    }
    return path + "_" + baseName;
  }

  // Get display form for error messages: "A.B.my_func"
  std::string display() const {
    if (modulePath.empty()) return baseName;
    return modulePath + "." + baseName;
  }

  bool empty() const { return modulePath.empty() && baseName.empty(); }

  bool operator==(const QualifiedName& other) const {
    return modulePath == other.modulePath && baseName == other.baseName;
  }

  bool operator!=(const QualifiedName& other) const {
    return !(*this == other);
  }

  bool operator<(const QualifiedName& other) const {
    if (modulePath != other.modulePath) return modulePath < other.modulePath;
    return baseName < other.baseName;
  }
};

}  // namespace sun
