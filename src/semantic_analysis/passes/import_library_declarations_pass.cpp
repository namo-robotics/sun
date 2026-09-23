#include "semantic_analysis/passes/import_library_declarations_pass.h"

#include "ast.h"
#include "semantic_analysis/declaration_table.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

void ImportLibraryDeclarationsPass::run(
    const std::vector<sun::ast::MoonScopeAST*>& imports) const {
  for (const auto* moon : imports)
    declarations_.importLibraryDeclarationRecords(moon->importedDeclarations);
}

}  // namespace sun::semantic_analysis::passes
