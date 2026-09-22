// interpolated_string_ast.h — Template string (lossless parse tree only)

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Template string literal: `Hello ${name}!`. Preserved by the parser for a
 * lossless parse tree; the lowering pass desugars it into std.String append
 * calls before semantic analysis, so it never reaches the borrow checker or
 * codegen.
 */
class InterpolatedStringAST : public ExprAST {
 public:
  /**
   * Ordered segment: either a literal run or a ${...} expression
   */
  struct Segment {
    bool isLiteral = true;
    std::string rawText;     // Exact source slice (escapes unprocessed; for
                             // expression segments: the text inside ${...})
    std::string cookedText;  // Literal segments only: escape-processed text
    std::unique_ptr<ExprAST> expression;  // Expression segments only
    int sourceOffset = 0;  // Segment start, absolute byte offset in the file
  };

 private:
  std::string rawContent_;  // Inner text, backticks stripped, unprocessed
  std::vector<Segment> segments_;

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  InterpolatedStringAST(std::string rawContent, std::vector<Segment> segments)
      : rawContent_(std::move(rawContent)), segments_(std::move(segments)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override {
    return ASTNodeType::INTERPOLATED_STRING;
  }

  /** Returns the unprocessed source contents. */
  const std::string& getRawContent() const { return rawContent_; }
  /** Returns the interpolated string segments. */
  const std::vector<Segment>& getSegments() const { return segments_; }
  /** Returns the modifiable interpolated string segments. */
  std::vector<Segment>& getSegmentsMutable() { return segments_; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    for (auto& segment : segments_) {
      if (!segment.isLiteral) fn(segment.expression);
    }
  }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return "`" + rawContent_ + "`"; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "InterpolatedString"; }
};

}  // namespace sun::ast
