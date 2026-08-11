// interpolated_string_ast.h — Template string (lossless parse tree only)

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/expr_ast.h"

// Template string literal: `Hello ${name}!`. Preserved by the parser for a
// lossless parse tree; the lowering pass desugars it into sun.String append
// calls before semantic analysis, so it never reaches the borrow checker or
// codegen.
class InterpolatedStringAST : public ExprAST {
 public:
  // Ordered segment: either a literal run or a ${...} expression
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
  InterpolatedStringAST(std::string rawContent, std::vector<Segment> segments)
      : rawContent_(std::move(rawContent)), segments_(std::move(segments)) {}

  ASTNodeType getType() const override {
    return ASTNodeType::INTERPOLATED_STRING;
  }

  const std::string& getRawContent() const { return rawContent_; }
  const std::vector<Segment>& getSegments() const { return segments_; }
  std::vector<Segment>& getSegmentsMutable() { return segments_; }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    for (auto& segment : segments_) {
      if (!segment.isLiteral) fn(segment.expression);
    }
  }

  std::string toString() const override { return "`" + rawContent_ + "`"; }
  std::string dotLabel() const override { return "InterpolatedString"; }
};
