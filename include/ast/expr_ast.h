// expr_ast.h — Base class for all AST expression nodes

#pragma once

#include <iostream>
#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/ast_common.h"
#include "lexer.h"

class ExprAST {
 protected:
  mutable std::unique_ptr<ExprAnalysis> analysis_;  // Analysis metadata
  Position location_;                               // Original source location
  bool precompiled_ = false;  // True if from precompiled library
  bool skipCodegen_ = false;  // Set by semantic analyzer for diamond duplicates
  std::string symbolPrefix_;  // Hash prefix for moon symbol isolation

  // Virtual method to ensure analysis is allocated with the correct type
  // Derived classes with specialized analysis types should override this
  virtual void ensureAnalysis() const {
    if (!analysis_) {
      analysis_ = std::make_unique<ExprAnalysis>();
    }
  }

  // Accessor for analysis data - calls ensureAnalysis to allocate correct type
  ExprAnalysis& analysis() const {
    ensureAnalysis();
    return *analysis_;
  }

  // Copy base class fields to a cloned node. Called by derived clone() methods.
  void cloneBase(ExprAST& dest) const {
    dest.location_ = location_;
    dest.precompiled_ = precompiled_;
    dest.skipCodegen_ = skipCodegen_;
    dest.symbolPrefix_ = symbolPrefix_;
    if (analysis_) {
      dest.setResolvedType(getResolvedType());
      dest.setMoved(isMoved());
    }
  }

 public:
  virtual ~ExprAST() = default;
  virtual ASTNodeType getType() const = 0;

  // Debug representation of this AST node
  virtual std::string toString() const = 0;

  // Label for DOT graph visualization (includes type name)
  virtual std::string dotLabel() const = 0;

  // Print to stderr (easier to call from debugger than toString())
  void dump() const { std::cerr << toString() << "\n"; }

  // Clone this AST node (deep copy) via protobuf serialization roundtrip.
  // This approach is less error-prone than manual cloning as it automatically
  // handles all fields through the proto schema.
  std::unique_ptr<ExprAST> clone() const;

  // Analysis data access
  bool hasAnalysis() const { return analysis_ != nullptr; }
  void clearAnalysis() const { analysis_.reset(); }

  // Type annotation set by semantic analyzer (delegates to analysis)
  void setResolvedType(sun::TypePtr type) const {
    analysis().resolvedType = std::move(type);
  }
  sun::TypePtr getResolvedType() const {
    return analysis_ ? analysis_->resolvedType : nullptr;
  }
  bool hasResolvedType() const {
    return analysis_ && analysis_->resolvedType != nullptr;
  }
  void clearResolvedType() const {
    if (analysis_) analysis_->resolvedType = nullptr;
  }

  // Source location tracking
  void setLocation(Position loc) { location_ = loc; }
  void setLocation(int line, int column) {
    location_.line = line;
    location_.column = column;
  }
  const Position& getLocation() const { return location_; }
  int getLine() const { return location_.line; }
  int getColumn() const { return location_.column; }

  // Convenience type checks
  bool isFunction() const { return getType() == ASTNodeType::FUNCTION; }
  bool isLambda() const { return getType() == ASTNodeType::LAMBDA; }
  bool isBlock() const { return getType() == ASTNodeType::BLOCK; }
  bool isReturn() const { return getType() == ASTNodeType::RETURN; }

  /// Returns true if this expression is a temporary (rvalue) that will be
  /// destroyed at the end of the statement. Temporaries include:
  /// - Constructor/function calls that return class values (CALL, GENERIC_CALL)
  /// - Literals that produce class values
  /// Named variables (VARIABLE_REFERENCE) and member accesses are NOT
  /// temporaries.
  bool isTemporary() const {
    ASTNodeType t = getType();
    // CALL and GENERIC_CALL produce temporaries when returning class values
    // The resolved type should be checked by the caller to confirm it's a class
    return t == ASTNodeType::CALL || t == ASTNodeType::GENERIC_CALL;
  }

  /// Returns true if this expression is an lvalue (can be assigned to,
  /// has a stable address). Includes variables, member access, and indexing.
  bool isLvalue() const {
    ASTNodeType t = getType();
    return t == ASTNodeType::VARIABLE_REFERENCE ||
           t == ASTNodeType::MEMBER_ACCESS || t == ASTNodeType::INDEX;
  }

  // Precompiled flag (for definitions loaded from .moon files)
  bool isPrecompiled() const { return precompiled_; }
  void setPrecompiled(bool value) { precompiled_ = value; }

  // Skip codegen flag (set by semantic analyzer for diamond import duplicates)
  bool shouldSkipCodegen() const { return skipCodegen_; }
  void setSkipCodegen(bool value) { skipCodegen_ = value; }

  // Symbol prefix for moon library isolation (content hash)
  const std::string& getSymbolPrefix() const { return symbolPrefix_; }
  void setSymbolPrefix(const std::string& prefix) { symbolPrefix_ = prefix; }

  // Moved flag: set by borrow checker when ownership is transferred
  // (returned, assigned to variable/field, passed by value). Moved expressions
  // should not have deinit called - the destination owns the data.
  bool isMoved() const { return analysis_ && analysis_->moved; }
  void setMoved(bool value) const { analysis().moved = value; }

 protected:
  ExprAST() = default;
  explicit ExprAST(Position loc) : location_(loc) {}
};
