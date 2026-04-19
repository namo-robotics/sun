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
  // Function context for nested functions: "outer(i32)::middle(f64)"
  // Empty for top-level functions
  std::string functionContext;
  std::string baseName;  // "my_func" (the original identifier, may contain _)

  QualifiedName() = default;
  QualifiedName(std::string mod, std::string name)
      : modulePath(std::move(mod)), baseName(std::move(name)) {}
  QualifiedName(std::string mod, std::string funcCtx, std::string name)
      : modulePath(std::move(mod)),
        functionContext(std::move(funcCtx)),
        baseName(std::move(name)) {}

  // Construct from a mangled name when we only have the mangled form
  // This is a fallback - prefer constructing with module path + base name
  static QualifiedName fromMangled(const std::string& mangledName) {
    return QualifiedName("", mangledName);
  }

  // Create a new QualifiedName with the given function context added
  QualifiedName withFunctionContext(const std::string& funcCtx) const {
    if (funcCtx.empty()) return *this;
    std::string newCtx = functionContext;
    if (!newCtx.empty()) newCtx += "::";
    newCtx += funcCtx;
    return QualifiedName(modulePath, newCtx, baseName);
  }

  // Get mangled form for codegen/lookup: "A_B_outer(i32)::my_func"
  std::string mangled() const {
    std::string result;
    if (!modulePath.empty()) {
      result = modulePath;
      for (char& c : result) {
        if (c == '.') c = '_';
      }
      result += "_";
    }
    if (!functionContext.empty()) {
      result += functionContext + "::";
    }
    result += baseName;
    return result;
  }

  // Get display form for error messages: "A.B.my_func"
  // Note: functionContext is not shown in display form (internal detail)
  std::string display() const {
    if (modulePath.empty()) return baseName;
    return modulePath + "." + baseName;
  }

  bool empty() const {
    return modulePath.empty() && functionContext.empty() && baseName.empty();
  }

  bool operator==(const QualifiedName& other) const {
    return modulePath == other.modulePath &&
           functionContext == other.functionContext &&
           baseName == other.baseName;
  }

  bool operator!=(const QualifiedName& other) const {
    return !(*this == other);
  }

  bool operator<(const QualifiedName& other) const {
    if (modulePath != other.modulePath) return modulePath < other.modulePath;
    if (functionContext != other.functionContext)
      return functionContext < other.functionContext;
    return baseName < other.baseName;
  }
};

}  // namespace sun
