// ast_dot_generator.h — Generate Graphviz DOT representation of AST

#pragma once

#include <sstream>
#include <string>

#include "ast.h"

/// Generates a Graphviz DOT graph from an AST tree.
/// Usage:
///   AstDotGenerator gen;
///   std::string dot = gen.generate(astRoot);
class AstDotGenerator {
 public:
  /// Generate DOT representation of the entire AST
  std::string generate(const ExprAST* root);

 private:
  std::stringstream out;
  int nodeCounter = 0;

  /// Visit a node, assign it an ID, emit its label, and return its ID
  int visitNode(const ExprAST* node);

  /// Get a human-readable label for a node
  std::string getNodeLabel(const ExprAST* node);

  /// Escape special characters for DOT labels
  static std::string escapeLabel(const std::string& s);

  /// Visit children of a node and emit edges
  void visitChildren(const ExprAST* node, int parentId);
};
