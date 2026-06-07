// qualified_name_ast.cpp — QualifiedNameAST clone implementation

#include "ast/qualified_name_ast.h"

std::unique_ptr<ExprAST> QualifiedNameAST::clone() const {
  auto copy = std::make_unique<QualifiedNameAST>(parts);
  cloneBase(*copy);
  if (hasAnalysis()) {
    copy->setResolvedMangledName(
        static_cast<QualifiedNameExprAnalysis&>(*analysis_)
            .resolvedMangledName);
  }
  return copy;
}
