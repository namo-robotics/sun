// import_scope_ast.h — ImportScopeAST class

#pragma once

#include <memory>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"

// Expanded import scope: contains the AST of an imported file.
// Nested ImportScopeASTs represent that file's own imports (transitive deps).
class ImportScopeAST : public ExprAST {
  std::string sourceFile;
  std::string contentHash;  // e.g. "$ab12cd34$" for .moon, empty for .sun
  std::unique_ptr<BlockExprAST> body;

 public:
  ImportScopeAST(std::string sourceFile, std::unique_ptr<BlockExprAST> body,
                 std::string contentHash = "")
      : sourceFile(std::move(sourceFile)),
        contentHash(std::move(contentHash)),
        body(std::move(body)) {}

  ASTNodeType getType() const override { return ASTNodeType::IMPORT_SCOPE; }
  std::string toString() const override {
    return "import_scope(\"" + sourceFile + "\") " + body->toString();
  }

  const std::string& getSourceFile() const { return sourceFile; }
  const std::string& getContentHash() const { return contentHash; }
  const BlockExprAST& getBody() const { return *body; }
  std::string dotLabel() const override { return "import\n" + sourceFile; }
};
