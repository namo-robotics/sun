// tests.h — Listing the test functions declared in one document, for the
// editor's test explorer. The analyzed tree may span a whole manifest; only
// tests located in the requested document are returned.

#pragma once

#include <string>
#include <vector>

#include "ast/block_expr_ast.h"
#include "lsp/symbol_location.h"

namespace sun::lsp {

// One test_function: its dotted runner name (module path plus function
// name, the exact string --test-filter matches) and where its name is
// written in the document.
struct TestItem {
  std::string id;
  std::string label;
  SymbolLocation location;
};

// Every test_function declared in the document at documentPath, walking
// module bodies the way the test runner does so ids match the runner's
// dotted names. `source` is the document's text, used to narrow each
// declaration span to the name token.
std::vector<TestItem> collectTests(const BlockExprAST& ast,
                                   const std::string& documentPath,
                                   const std::string& source);

// One test_function anywhere in an analyzed program: its dotted name, the
// declaration span, and the file holding it. The caller narrows spans to
// name tokens with each file's own text (workspace-wide discovery has no
// single document to borrow text from).
struct TestSpan {
  std::string id;
  std::string label;
  Position span;
  std::string filePath;  // normalized
};

// Every test_function in the program, whichever file declares it.
std::vector<TestSpan> collectTestSpans(const BlockExprAST& ast);

}  // namespace sun::lsp
