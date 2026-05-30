// scope_tree_generator.cpp — Generate interactive HTML visualization of scope
// tree

#include "debug/scope_tree_generator.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "sun_path.h"

std::string ScopeTreeGenerator::generateHtml(const SemanticScope& root) {
  std::string json = generateJson(root);
  std::string html = getHtmlTemplate();

  // Replace placeholder with actual JSON data
  const std::string placeholder = "/* SCOPE_JSON_DATA */";
  size_t pos = html.find(placeholder);
  if (pos != std::string::npos) {
    html.replace(pos, placeholder.length(), json);
  }

  return html;
}

std::string ScopeTreeGenerator::scopeTypeToString(ScopeType type) {
  switch (type) {
    case ScopeType::Global:
      return "Global";
    case ScopeType::Module:
      return "Module";
    case ScopeType::Import:
      return "Import";
    case ScopeType::Class:
      return "Class";
    case ScopeType::Interface:
      return "Interface";
    case ScopeType::Function:
      return "Function";
    case ScopeType::Block:
      return "Block";
    case ScopeType::TypeParams:
      return "TypeParams";
    default:
      return "Unknown";
  }
}

std::string ScopeTreeGenerator::escapeJson(const std::string& s) {
  std::string result;
  result.reserve(s.size() + 10);
  for (char c : s) {
    switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 32) {
          // Control character - escape as unicode
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

std::string ScopeTreeGenerator::generateJson(const SemanticScope& scope,
                                             int indent) {
  std::ostringstream out;
  std::string pad(indent, ' ');
  std::string pad2(indent + 2, ' ');
  std::string pad4(indent + 4, ' ');

  out << "{\n";

  // Scope type
  out << pad2 << "\"type\": \"" << scopeTypeToString(scope.getType()) << "\"";

  // Always output scopeName and scopeKey for full transparency
  if (!scope.scopeName.empty()) {
    out << ",\n"
        << pad2 << "\"scopeName\": \"" << escapeJson(scope.scopeName) << "\"";
  }
  if (!scope.scopeKey.empty()) {
    out << ",\n"
        << pad2 << "\"scopeKey\": \"" << escapeJson(scope.scopeKey) << "\"";
  }

  // For class/interface scopes, show baseName and mangledName
  if ((scope.getType() == ScopeType::Class || scope.getType() == ScopeType::Interface) &&
      !scope.classBaseName.empty()) {
    out << ",\n"
        << pad2 << "\"className\": \"" << escapeJson(scope.classBaseName)
        << "\"";
    if (!scope.classMangledName.empty()) {
      out << ",\n"
          << pad2 << "\"mangledName\": \"" << escapeJson(scope.classMangledName)
          << "\"";
    }
  }

  // For function scopes, also show the function signature
  if (scope.getType() == ScopeType::Function && !scope.functionSignature.empty()) {
    out << ",\n"
        << pad2 << "\"functionSignature\": \""
        << escapeJson(scope.functionSignature) << "\"";
  }

  // For function scopes, show the fully qualified mangled name
  if (scope.getType() == ScopeType::Function &&
      !scope.functionName.baseName.empty()) {
    out << ",\n"
        << pad2 << "\"functionMangled\": \""
        << escapeJson(scope.functionName.mangled()) << "\"";
  }

  // Function can throw
  if (scope.functionCanThrow) {
    out << ",\n" << pad2 << "\"canThrow\": true";
  }

  // Unsafe context
  if (scope.inUnsafeContext) {
    out << ",\n" << pad2 << "\"unsafe\": true";
  }

  // External flag
  if (scope.isExternal) {
    out << ",\n" << pad2 << "\"external\": true";
  }

  // Variables
  if (!scope.variables.empty()) {
    out << ",\n" << pad2 << "\"variables\": {";
    bool first = true;
    for (const auto& [name, info] : scope.variables) {
      if (!first) out << ",";
      out << "\n"
          << pad4 << "\"" << escapeJson(name) << "\": {"
          << "\"type\": \""
          << (info.type ? escapeJson(info.type->toString()) : "unknown") << "\""
          << ", \"isGlobal\": " << (info.isGlobal ? "true" : "false")
          << ", \"isParam\": " << (info.isFunctionParam ? "true" : "false")
          << "}";
      first = false;
    }
    out << "\n" << pad2 << "}";
  }

  // Namespaced variables
  if (!scope.namespacedVariables.empty()) {
    out << ",\n" << pad2 << "\"namespacedVariables\": {";
    bool first = true;
    for (const auto& [name, info] : scope.namespacedVariables) {
      if (!first) out << ",";
      out << "\n"
          << pad4 << "\"" << escapeJson(name) << "\": {"
          << "\"type\": \""
          << (info.type ? escapeJson(info.type->toString()) : "unknown") << "\""
          << "}";
      first = false;
    }
    out << "\n" << pad2 << "}";
  }

  // Note: Functions, classes, interfaces, enums are not listed here
  // as they appear as child scope nodes in the tree. The info is shown
  // on the scope node header itself.

  // Type parameters
  if (!scope.typeParameters.empty()) {
    out << ",\n" << pad2 << "\"typeParameters\": {";
    bool first = true;
    for (const auto& [name, type] : scope.typeParameters) {
      if (!first) out << ",";
      out << "\n"
          << pad4 << "\"" << escapeJson(name) << "\": \""
          << (type ? escapeJson(type->toString()) : "?") << "\"";
      first = false;
    }
    out << "\n" << pad2 << "}";
  }

  // Type aliases
  if (!scope.typeAliases.empty()) {
    out << ",\n" << pad2 << "\"typeAliases\": {";
    bool first = true;
    for (const auto& [name, type] : scope.typeAliases) {
      if (!first) out << ",";
      out << "\n"
          << pad4 << "\"" << escapeJson(name) << "\": \""
          << (type ? escapeJson(type->toString()) : "?") << "\"";
      first = false;
    }
    out << "\n" << pad2 << "}";
  }

  // Child module scopes
  if (!scope.childModules.empty()) {
    out << ",\n" << pad2 << "\"childModules\": {";
    bool first = true;
    for (const auto& [name, child] : scope.childModules) {
      if (!first) out << ",";
      out << "\n" << pad4 << "\"" << escapeJson(name) << "\": ";
      out << generateJson(*child, indent + 4);
      first = false;
    }
    out << "\n" << pad2 << "}";
  }

  // Non-module children (block/function scopes)
  if (!scope.children.empty()) {
    out << ",\n" << pad2 << "\"children\": [";
    bool first = true;
    for (const auto& child : scope.children) {
      if (!first) out << ",";
      out << "\n" << pad4;
      out << generateJson(*child, indent + 4);
      first = false;
    }
    out << "\n" << pad2 << "]";
  }

  out << "\n" << pad << "}";
  return out.str();
}

std::string ScopeTreeGenerator::getHtmlTemplate() {
  // Read template from external file for easier editing
  auto paths = sun::SunPath::getPaths();
  std::string templatePath;
  for (const auto& dir : paths) {
    auto candidate = dir / "src/debug/scope_tree_template.html";
    if (std::filesystem::exists(candidate)) {
      templatePath = candidate.string();
      break;
    }
  }

  if (templatePath.empty()) {
    return R"HTML(<!DOCTYPE html>
<html><body>
<h1>Error</h1>
<p>Could not find scope_tree_template.html in SUN_PATH directories.</p>
<p>Make sure SUN_PATH environment variable is set correctly.</p>
</body></html>)HTML";
  }

  std::ifstream file(templatePath);
  if (!file) {
    return R"HTML(<!DOCTYPE html>
<html><body>
<h1>Error</h1>
<p>Could not open scope_tree_template.html</p>
</body></html>)HTML";
  }

  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}
