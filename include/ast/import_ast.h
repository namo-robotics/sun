// import_ast.h — ImportAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Import declaration: import "path/to/module.sun";
class ImportAST : public ExprAST {
  std::string path;  // The file path to import

 public:
  explicit ImportAST(std::string path) : path(std::move(path)) {}

  ASTNodeType getType() const override { return ASTNodeType::IMPORT; }
  std::string toString() const override { return "import \"" + path + "\""; }
  std::string dotLabel() const override { return "Import\n" + path; }

  std::unique_ptr<ExprAST> clone() const override;
  const std::string& getPath() const { return path; }
};
