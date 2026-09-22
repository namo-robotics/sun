#pragma once

#include <vector>

#include "semantic_analysis/semantic_context.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

/** Visit each borrowed import body in its bundle scope and source file.
 * Restore the caller's scope and source file even if the visitor throws.
 */
template <typename Visitor>
void forEachImportedBody(const std::vector<sun::ast::MoonScopeAST*>& imports,
                         SemanticContext& context, Visitor&& visitor) {
  for (auto* moon : imports) {
    SemanticContext::SourceFileGuard sourceFile(context,
                                                moon->getSourceFileId());
    SemanticContext::ScopeSwitchGuard scope(context, context.scope());
    if (!moon->getContentHash().empty())
      context.enterModuleScope(moon->getContentHash());
    visitor(const_cast<sun::ast::BlockExprAST&>(moon->getBody()));
  }
}

}  // namespace sun::semantic_analysis::passes
