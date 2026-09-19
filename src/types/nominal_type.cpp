/** Resolves the source declaration of a session-owned nominal type. */
#include "types/nominal_type.h"

#include "semantic_analysis/type_registry.h"

/** Implements shared type descriptions and queries. */
namespace sun::types {
sun::semantic_analysis::DeclarationId NominalType::sourceDeclaration(
    const DeclarationTable& table) const {
  if (!belongsTo(table))
    logAndThrowError("Nominal type belongs to another analysis session");
  const auto& record = table.get(getDeclarationId());
  return record.specialization ? record.specialization->source
                               : getDeclarationId();
}

}  // namespace sun::types
