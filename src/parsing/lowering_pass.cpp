// lowering_pass.cpp — Post-parse, pre-semantic AST lowering

#include "parsing/lowering_pass.h"

#include "parsing/interpolated_string_parser.h"

void LoweringPass::run(BlockExprAST& program) {
  program.forEachChildSlot(
      [this](std::unique_ptr<ExprAST>& slot) { lowerSlot(slot); });
}

// Normalize an if/loop body block to the shape the core pipeline expects:
// empty block -> synthetic literal, single-statement block -> the statement.
// The parser keeps bodies as blocks for losslessness.
static void normalizeBody(std::unique_ptr<ExprAST>& slot, bool isIfBody) {
  if (!slot || slot->getType() != ASTNodeType::BLOCK) return;
  auto& block = static_cast<BlockExprAST&>(*slot);
  if (block.isEmpty()) {
    if (isIfBody) {
      slot = std::make_unique<BoolLiteralAST>(false);
    } else {
      slot = std::make_unique<NumberExprAST>(0.0);
    }
  } else if (block.getBody().size() == 1) {
    slot = std::move(block.mutableBody()[0]);
  }
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

  // Desugar template strings into std.String append calls
  if (slot && slot->getType() == ASTNodeType::INTERPOLATED_STRING) {
    usedInterpolation_ = true;
    slot = InterpolatedStringParser::desugar(
        static_cast<InterpolatedStringAST&>(*slot));
  }

  if (!slot) return;
  switch (slot->getType()) {
    case ASTNodeType::IF: {
      auto& n = static_cast<IfExprAST&>(*slot);
      normalizeBody(n.thenSlot(), /*isIfBody=*/true);
      normalizeBody(n.elseSlot(), /*isIfBody=*/true);  // else-if untouched
      break;
    }
    case ASTNodeType::WHILE_LOOP:
      normalizeBody(static_cast<WhileExprAST&>(*slot).bodySlot(), false);
      break;
    case ASTNodeType::FOR_LOOP:
      normalizeBody(static_cast<ForExprAST&>(*slot).bodySlot(), false);
      break;
    case ASTNodeType::FOR_IN_LOOP:
      normalizeBody(static_cast<ForInExprAST&>(*slot).bodySlot(), false);
      break;
    default:
      break;
  }
}
