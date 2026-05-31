// scope_tree_generator.h — Generate interactive HTML visualization of scope
// tree

#pragma once

#include <sstream>
#include <string>

#include "semantic_scope.h"

/// Generates an interactive HTML visualization of the semantic scope tree.
/// Usage:
///   ScopeTreeGenerator gen;
///   std::string html = gen.generateHtml(rootScope);
class ScopeTreeGenerator {
 public:
  /// Generate HTML representation of the entire scope tree
  std::string generateHtml(const SemanticScope& root);

 private:
  std::stringstream out;

  /// Generate JSON representation of a scope (recursive)
  std::string generateJson(const SemanticScope& scope, int indent = 0);

  /// Convert ScopeType enum to string
  static std::string scopeTypeToString(ScopeType type);

  /// Escape special characters for JSON strings
  static std::string escapeJson(const std::string& s);

  /// Get the HTML template with embedded CSS and JavaScript
  static std::string getHtmlTemplate();
};
