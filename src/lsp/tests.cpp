// tests.cpp — Listing the test functions declared in one document.

#include "lsp/tests.h"

#include "ast/function_ast.h"
#include "ast/module_ast.h"
#include "lsp/declarations.h"
#include "lsp/name_ranges.h"

namespace sun::lsp {

namespace {

// Walk item-level statements, recursing through module bodies with the
// dotted path so ids come out exactly as the test runner names them.
void collect(const BlockExprAST& block, std::vector<std::string>& modulePath,
             const std::string& documentPath, const std::string& source,
             std::vector<TestItem>& out) {
  for (const auto& stmt : block.getBody()) {
    if (!stmt) continue;
    if (stmt->getType() == ASTNodeType::MODULE) {
      const auto& module = static_cast<const ModuleAST&>(*stmt);
      modulePath.push_back(module.getName());
      collect(module.getBody(), modulePath, documentPath, source, out);
      modulePath.pop_back();
      continue;
    }
    if (stmt->getType() != ASTNodeType::FUNCTION) continue;
    const auto& function = static_cast<const FunctionAST&>(*stmt);
    if (!function.isTest()) continue;

    const Position& span = function.getLocation();
    if (!span.filePath || normalizePath(*span.filePath) != documentPath) {
      continue;
    }

    const std::string& name = function.getProto().getName();
    TestItem item;
    for (const auto& segment : modulePath) item.id += segment + ".";
    item.id += name;
    item.label = name;
    item.location =
        makeSymbolLocation(documentPath, nameRange(span, name, source), source);
    out.push_back(std::move(item));
  }
}

}  // namespace

std::vector<TestItem> collectTests(const BlockExprAST& ast,
                                   const std::string& documentPath,
                                   const std::string& source) {
  std::vector<TestItem> items;
  std::vector<std::string> modulePath;
  collect(ast, modulePath, normalizePath(documentPath), source, items);
  return items;
}

namespace {

// Workspace-wide walk: like collect, but keeps every test with its file
// instead of filtering to one document.
void collectSpans(const BlockExprAST& block,
                  std::vector<std::string>& modulePath,
                  std::vector<TestSpan>& out) {
  for (const auto& stmt : block.getBody()) {
    if (!stmt) continue;
    if (stmt->getType() == ASTNodeType::MODULE) {
      const auto& module = static_cast<const ModuleAST&>(*stmt);
      modulePath.push_back(module.getName());
      collectSpans(module.getBody(), modulePath, out);
      modulePath.pop_back();
      continue;
    }
    if (stmt->getType() != ASTNodeType::FUNCTION) continue;
    const auto& function = static_cast<const FunctionAST&>(*stmt);
    if (!function.isTest()) continue;

    const Position& span = function.getLocation();
    if (!span.filePath) continue;

    TestSpan item;
    for (const auto& segment : modulePath) item.id += segment + ".";
    item.id += function.getProto().getName();
    item.label = function.getProto().getName();
    item.span = span;
    item.filePath = normalizePath(*span.filePath);
    out.push_back(std::move(item));
  }
}

}  // namespace

std::vector<TestSpan> collectTestSpans(const BlockExprAST& ast) {
  std::vector<TestSpan> items;
  std::vector<std::string> modulePath;
  collectSpans(ast, modulePath, items);
  return items;
}

}  // namespace sun::lsp
