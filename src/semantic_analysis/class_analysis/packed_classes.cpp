/** Checks packed classes within the semantic session. */
#include "semantic_analysis/class_analysis/packed_layout.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

// `ref p.field` would hand out an address the borrower accesses at the field
// type's natural alignment, which a packed field does not satisfy.
void ClassAnalyzer::checkPackedFieldNotBorrowed(
    const sun::ast::ExprAST& target, const sun::support::Position& loc) const {
  if (target.getType() != sun::ast::ASTNodeType::MEMBER_ACCESS) return;
  std::string ownerName;
  if (!sun::semantic_analysis::isFieldAccess(target, &ownerName)) return;
  logAndThrowError(sun::semantic_analysis::borrowRejection(
                       "create a reference to a " +
                           sun::semantic_analysis::fieldPhrase(ownerName),
                       "Copy the field into a local instead."),
                   loc);
}

void ClassAnalyzer::checkPackedRefArguments(
    const std::vector<std::unique_ptr<sun::ast::ExprAST>>& args,
    const std::vector<sun::types::TypePtr>& paramTypes) const {
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    if (!paramTypes[i] || !paramTypes[i]->isReference()) continue;
    if (args[i]->getType() != sun::ast::ASTNodeType::MEMBER_ACCESS) continue;
    std::string ownerName;
    if (sun::semantic_analysis::isFieldAccess(*args[i], &ownerName)) {
      logAndThrowError(
          sun::semantic_analysis::borrowRejection(
              "pass a " + sun::semantic_analysis::fieldPhrase(ownerName) +
                  " to a ref parameter",
              "Pass a copy instead."),
          args[i]->getLocation());
    }
  }
}

void ClassAnalyzer::checkPackedFieldType(
    const sun::ast::ClassDefinitionAST& classDef,
    const sun::ast::ClassFieldDecl& field,
    const sun::types::TypePtr& fieldType) const {
  if (!classDef.isPacked()) return;
  std::string reason = sun::semantic_analysis::rejectFieldType(fieldType);
  if (reason.empty()) return;
  logAndThrowError("Field '" + field.name + "' in packed class '" +
                       classDef.getName() + "' " + reason,
                   field.location);
}

}  // namespace sun::semantic_analysis
