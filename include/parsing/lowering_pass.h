// lowering_pass.h — Post-parse, pre-semantic AST lowering
//
// Desugars lossless parse-tree nodes into the core AST that semantic
// analysis, borrow checking, and codegen consume. Runs once per program in
// Driver::runPipeline, before moon-import stubs are injected.

#pragma once

#include <memory>

#include "ast.h"

class LoweringPass {
 public:
  // Lower the program in place (recursive, bottom-up)
  void run(BlockExprAST& program);

  // True if any string interpolation was desugared by this pass
  bool usedInterpolation() const { return usedInterpolation_; }

 private:
  // Recurse into the slot's children, then rewrite the slot itself if it
  // holds a node the core pipeline doesn't understand
  void lowerSlot(std::unique_ptr<ExprAST>& slot);

  bool usedInterpolation_ = false;
};
