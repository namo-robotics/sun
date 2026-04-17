// semantic_analyzer.cpp — Shared utilities for semantic analysis
//
// The SemanticAnalyzer is split across multiple files in
// src/semantic_analysis/:
//   - scope_variables.cpp: Scope management, variable/function registration
//   - classes.cpp: Class support, generic class instantiation
//   - interfaces.cpp: Interface/enum support, validation
//   - type_conversion.cpp: Type annotation to Type conversion
//   - type_inference.cpp: Type inference (inferType)
//   - captures.cpp: Free variable collection, closure captures
//   - analysis.cpp: Main analysis (analyzeExpr, analyzeBlock, analyzeFunction)
//
// This file contains shared utilities used across multiple analysis files.

#include "semantic_analyzer.h"

#include <unordered_set>

// Helper: check if an identifier is reserved (starts with underscore)
// Reserved identifiers are for builtins only (e.g., _is<T>, _sizeof<T>)
// Exception: specific dunder methods are allowed for operator overloading
bool SemanticAnalyzer::isReservedIdentifier(const std::string& name) {
  if (name.empty() || name[0] != '_') return false;
  // Allowlist of dunder methods for operator overloading
  static const std::unordered_set<std::string> allowedDunders = {
      "__index__",     // obj[i] read
      "__setindex__",  // obj[i] = val write
      "__slice__",     // obj[a:b] slicing
  };
  if (allowedDunders.count(name)) return false;
  return true;
}
