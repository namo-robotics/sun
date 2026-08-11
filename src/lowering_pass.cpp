// lowering_pass.cpp — Post-parse, pre-semantic AST lowering

#include "lowering_pass.h"

#include "interpolated_string_parser.h"

void LoweringPass::run(BlockExprAST& program) {
  program.forEachChildSlot(
      [this](std::unique_ptr<ExprAST>& slot) { lowerSlot(slot); });
}

void LoweringPass::lowerSlot(std::unique_ptr<ExprAST>& slot) {
  if (!slot || slot->isPrecompiled()) return;

  // Bottom-up: children first
  slot->forEachChildSlot(
      [this](std::unique_ptr<ExprAST>& child) { lowerSlot(child); });

  // Unwrap grouping parentheses: (expr) -> expr (iterative for ((expr)))
  while (slot && slot->getType() == ASTNodeType::PAREN_EXPR) {
    slot = static_cast<ParenExprAST&>(*slot).takeInner();
  }

  // Desugar template strings into sun.String append calls
  if (slot && slot->getType() == ASTNodeType::INTERPOLATED_STRING) {
    usedInterpolation_ = true;
    slot = InterpolatedStringParser::desugar(
        static_cast<InterpolatedStringAST&>(*slot));
  }
}
