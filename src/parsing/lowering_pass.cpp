// lowering_pass.cpp — Post-parse, pre-semantic AST lowering

#include "parsing/lowering_pass.h"

#include "parsing/interpolated_string_parser.h"

using sun::ast::ASTNodeType;
using sun::ast::ExprAST;

/** Turns source text into syntax trees and provides source formatting. */
namespace sun::parsing {

void LoweringPass::run(sun::ast::BlockExprAST& program) {
  program.forEachChildSlot(
      [this](std::unique_ptr<ExprAST>& slot) { lowerSlot(slot); });
}

/**
 * Normalize an if/loop body block to the shape the core pipeline expects:
 * empty block -> synthetic literal, single-statement block -> the statement.
 * The parser keeps bodies as blocks for losslessness.
 */
static void normalizeBody(std::unique_ptr<ExprAST>& slot, bool isIfBody) {
  if (!slot || slot->getType() != ASTNodeType::BLOCK) return;
  auto& block = static_cast<sun::ast::BlockExprAST&>(*slot);
  const auto sourceFile = block.getSourceFileId();
  if (block.isEmpty()) {
    if (isIfBody) {
      slot = std::make_unique<sun::ast::BoolLiteralAST>(false);
    } else {
      slot = std::make_unique<sun::ast::NumberExprAST>(0.0);
    }
  } else if (block.getBody().size() == 1) {
    slot = std::move(block.mutableBody()[0]);
  }
  slot->inheritSourceFile(sourceFile);
}

void LoweringPass::lowerSlot(std::unique_ptr<ExprAST>& slot) {
  if (!slot || slot->isPrecompiled()) return;

  const auto sourceFile = slot->getSourceFileId();

  // Bottom-up: children first
  slot->forEachChildSlot(
      [this](std::unique_ptr<ExprAST>& child) { lowerSlot(child); });

  // Unwrap grouping parentheses: (expr) -> expr (iterative for ((expr)))
  while (slot && slot->getType() == ASTNodeType::PAREN_EXPR) {
    slot = static_cast<sun::ast::ParenExprAST&>(*slot).takeInner();
  }

  // Desugar template strings into std.String append calls
  if (slot && slot->getType() == ASTNodeType::INTERPOLATED_STRING) {
    usedInterpolation_ = true;
    slot = InterpolatedStringParser::desugar(
        static_cast<sun::ast::InterpolatedStringAST&>(*slot));
  }

  if (!slot) return;
  slot->inheritSourceFile(sourceFile);
  switch (slot->getType()) {
    case ASTNodeType::IF: {
      auto& n = static_cast<sun::ast::IfExprAST&>(*slot);
      normalizeBody(n.thenSlot(), /*isIfBody=*/true);
      normalizeBody(n.elseSlot(), /*isIfBody=*/true);  // else-if untouched
      break;
    }
    case ASTNodeType::WHILE_LOOP:
      normalizeBody(static_cast<sun::ast::WhileExprAST&>(*slot).bodySlot(),
                    false);
      break;
    case ASTNodeType::FOR_LOOP:
      normalizeBody(static_cast<sun::ast::ForExprAST&>(*slot).bodySlot(),
                    false);
      break;
    case ASTNodeType::FOR_IN_LOOP:
      normalizeBody(static_cast<sun::ast::ForInExprAST&>(*slot).bodySlot(),
                    false);
      break;
    default:
      break;
  }
}

}  // namespace sun::parsing
