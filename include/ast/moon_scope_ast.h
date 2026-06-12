#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"

/// MoonScopeAST wraps all stubs from a single moon import.
/// It carries the content hash for deduplication and optional alias for
/// renaming.
class MoonScopeAST : public ExprAST {
  std::string
      contentHash_;         // Content hash from moon metadata for deduplication
  std::string moduleName_;  // Original module name from the moon
  std::optional<std::string> alias_;  // Optional rename from manifest
  std::string moonPath_;  // Path to the moon file (for error messages)
  std::unique_ptr<BlockExprAST> body_;  // Module stubs from this moon

 public:
  MoonScopeAST(std::string contentHash, std::string moduleName,
               std::optional<std::string> alias, std::string moonPath,
               std::unique_ptr<BlockExprAST> body)
      : contentHash_(std::move(contentHash)),
        moduleName_(std::move(moduleName)),
        alias_(std::move(alias)),
        moonPath_(std::move(moonPath)),
        body_(std::move(body)) {}

  ASTNodeType getType() const override { return ASTNodeType::MOON_SCOPE; }
  std::string toString() const override {
    return "moon_scope(" + getEffectiveName() + ")";
  }

  /// Get the content hash for deduplication
  const std::string& getContentHash() const { return contentHash_; }

  /// Get the original module name from the moon
  const std::string& getModuleName() const { return moduleName_; }

  /// Get the alias (rename) if specified
  const std::optional<std::string>& getAlias() const { return alias_; }

  /// Get the effective name (alias if set, otherwise original name)
  std::string getEffectiveName() const { return alias_.value_or(moduleName_); }

  /// Get the path to the moon file
  const std::string& getMoonPath() const { return moonPath_; }

  /// Get the body containing module stubs
  const BlockExprAST& getBody() const { return *body_; }
  BlockExprAST& getBody() { return *body_; }

  std::string dotLabel() const override {
    return "MoonScope\n" + getEffectiveName() +
           "\nhash: " + contentHash_.substr(0, 8);
  }
};
