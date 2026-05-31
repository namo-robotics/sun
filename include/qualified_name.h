// qualified_name.h - Qualified name with dual representation for Sun language
//
// A QualifiedName keeps module path and symbol name separate to provide
// both mangled names (for codegen) and display names (for error messages).
//
// For moon imports, the library content hash is encoded as a module scope
// in the module path (e.g., "$abc123$.sun.submodule").

#pragma once

#include <string>

namespace sun {

// A qualified name with both mangled and display representations
// Keeps module path and symbol name separate to avoid lossy conversion
struct QualifiedName {
  std::string
      scopeKey;  // "A.B" or "$hash$.A.B" (dot-separated) or "" for global
  // Function context for nested functions: "outer(i32)::middle(f64)"
  // Empty for top-level functions
  std::string functionContext;
  std::string baseName;  // "my_func" (the original identifier, may contain _)

  QualifiedName() = default;
  QualifiedName(std::string modPath, std::string name)
      : scopeKey(std::move(modPath)), baseName(std::move(name)) {}
  QualifiedName(std::string mod, std::string funcCtx, std::string name)
      : scopeKey(std::move(mod)),
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
    return QualifiedName(scopeKey, newCtx, baseName);
  }

  // Get mangled form for codegen/lookup: "$hash$_A_B_outer(i32)::my_func"
  // Simply replaces dots with underscores
  std::string mangled() const {
    std::string result;
    if (!scopeKey.empty()) {
      // Replace dots with underscores
      result = scopeKey;
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

  bool empty() const {
    return scopeKey.empty() && functionContext.empty() && baseName.empty();
  }

  bool operator==(const QualifiedName& other) const {
    return scopeKey == other.scopeKey &&
           functionContext == other.functionContext &&
           baseName == other.baseName;
  }

  bool operator!=(const QualifiedName& other) const {
    return !(*this == other);
  }

  bool operator<(const QualifiedName& other) const {
    if (scopeKey != other.scopeKey) return scopeKey < other.scopeKey;
    if (functionContext != other.functionContext)
      return functionContext < other.functionContext;
    return baseName < other.baseName;
  }
};

}  // namespace sun
