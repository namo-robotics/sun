#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"
#include "types/types.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * MoonScopeAST wraps the declarations of one bundle under its `$hash$`
 * scope: the stubs of an imported .moon, or — when a .moon is being built —
 * the sources being bundled. Both are analyzed under the same scope name, so
 * a bundle spells its own symbols exactly as its importers will.
 *
 * An imported scope also carries the module name and optional alias from
 * the manifest, and its stubs are precompiled (no bodies, no body checks).
 */
class MoonScopeAST : public ExprAST {
  std::string
      contentHash_;         // Content hash from moon metadata for deduplication
  std::string moduleName_;  // Original module name from the moon
  std::optional<std::string> alias_;  // Optional rename from manifest
  std::string moonPath_;  // Path to the moon file (for error messages)
  std::unique_ptr<BlockExprAST> body_;  // Module stubs from this moon
  bool ownBundle_ = false;              // The bundle being built, not an import

 public:
  /** A referenced declaration and the type kind required by this use. */
  struct DeclarationRequirement {
    sun::semantic_analysis::PortableDeclarationKey key;
    std::string displayName;
    // Empty accepts any nominal type; implements and constraints require
    // Interface.
    std::optional<sun::types::Type::Kind> expectedKind;
  };

  /** Exact declarations referenced by this bundle's metadata, with any kind
   * requirement imposed by their use. Import preparation validates these
   * after registering every bundle's records, including requirements from
   * generic declarations that are never instantiated.
   */
  std::vector<DeclarationRequirement> requiredDeclarations;
  // Registered by import preparation before syntax identities are assigned.
  std::vector<sun::semantic_analysis::ImportedDeclarationRecord>
      importedDeclarations;

  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  MoonScopeAST(std::string contentHash, std::string moduleName,
               std::optional<std::string> alias, std::string moonPath,
               std::unique_ptr<BlockExprAST> body)
      : contentHash_(std::move(contentHash)),
        moduleName_(std::move(moduleName)),
        alias_(std::move(alias)),
        moonPath_(std::move(moonPath)),
        body_(std::move(body)) {}

  /**
   * The scope of the bundle being built: `body` is the program's own source
   * declarations, fully analyzed and compiled, under `scopeName` ("$hash$").
   */
  static std::unique_ptr<MoonScopeAST> forOwnBundle(
      std::string scopeName, std::unique_ptr<BlockExprAST> body) {
    auto scope = std::make_unique<MoonScopeAST>(
        std::move(scopeName), "", std::nullopt, "", std::move(body));
    scope->ownBundle_ = true;
    return scope;
  }

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::MOON_SCOPE; }

  /**
   * True for the bundle being built; false for an imported bundle's stubs
   */
  bool isOwnBundle() const { return ownBundle_; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (body_) body_->forEachChildSlot(fn);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    if (ownBundle_) return "own_bundle_scope(" + contentHash_ + ")";
    return "moon_scope(" + getEffectiveName() + ")";
  }

  /**
   * Get the content hash for deduplication
   */
  const std::string& getContentHash() const { return contentHash_; }

  /**
   * Get the original module name from the moon
   */
  const std::string& getModuleName() const { return moduleName_; }

  /**
   * Get the alias (rename) if specified
   */
  const std::optional<std::string>& getAlias() const { return alias_; }

  /**
   * Get the effective name (alias if set, otherwise original name)
   */
  std::string getEffectiveName() const { return alias_.value_or(moduleName_); }

  /**
   * Get the path to the moon file
   */
  const std::string& getMoonPath() const { return moonPath_; }

  /**
   * Get the body containing module stubs
   */
  const BlockExprAST& getBody() const { return *body_; }
  /** Provides access to the expressions that make up the body. */
  BlockExprAST& getBody() { return *body_; }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override {
    if (ownBundle_) return "OwnBundleScope\n" + contentHash_;
    return "MoonScope\n" + getEffectiveName() +
           "\nhash: " + contentHash_.substr(0, 8);
  }
};

}  // namespace sun::ast
