#include "semantic_analysis/passes/global_registration_pass.h"

#include "semantic_analysis/declaration_table.h"
#include "semantic_analysis/globals.h"

/** Provides the ordered passes for semantic analysis. */
namespace sun::semantic_analysis::passes {

void GlobalRegistrationPass::run(const sun::ast::BlockExprAST& block) const {
  forEachGlobalDeclaration(
      block, [&](const sun::ast::VariableCreationAST& global) {
        // A library's globals and C globals have no initializer to analyze
        if (global.isPrecompiled() || global.isCExtern() ||
            !global.hasQualifiedName() || !global.getDeclarationId())
          return;
        declarations_.registerGlobal(global.getQualifiedName(),
                                     global.getDeclarationId());
      });
}

}  // namespace sun::semantic_analysis::passes
