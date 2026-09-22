#include "semantic_analysis/passes/import_completion_pass.h"

#include "ast.h"
#include "semantic_analysis/semantic_analyzer.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

void ImportCompletionPass::run(
    const std::vector<sun::ast::MoonScopeAST*>& imports) const {
  for (auto* moon : imports) {
    SemanticContext::SourceFileGuard sourceFile(analyzer_.context(),
                                                moon->getSourceFileId());
    analyzer_.analyzeMoonScope(*moon);
  }
}

}  // namespace sun::semantic_analysis::passes
