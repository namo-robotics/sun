#include "semantic_analysis/passes/import_record_registration_pass.h"

#include "ast.h"
#include "semantic_analysis/declaration_table.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

void ImportRecordRegistrationPass::run(
    const std::vector<sun::ast::MoonScopeAST*>& imports) const {
  for (const auto* moon : imports)
    declarations_.importRecords(moon->importedDeclarations);
}

}  // namespace sun::semantic_analysis::passes
