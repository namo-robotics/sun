// packed_classes.cpp — The rules a packed class has to obey
//
// A packed class lays its fields out with no padding, so a field is not at
// its type's natural alignment. Each check below rejects one way that
// guarantee could leak out: by borrowing such a field, by passing it to a
// `ref` parameter, or by declaring a field type the layout cannot hold.
// See include/semantic_analysis/packed_layout.h for what "packed" means.

#include "semantic_analysis/packed_layout.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

// `ref p.field` would hand out an address the borrower accesses at the field
// type's natural alignment, which a packed field does not satisfy.
void SemanticAnalyzer::checkPackedFieldNotBorrowed(const ExprAST& target,
                                                   const Position& loc) const {
  if (target.getType() != ASTNodeType::MEMBER_ACCESS) return;
  std::string ownerName;
  if (!sun::packed::isFieldAccess(target, &ownerName)) return;
  logAndThrowError(
      sun::packed::borrowRejection(
          "create a reference to a " + sun::packed::fieldPhrase(ownerName),
          "Copy the field into a local instead."),
      loc);
}

// A ref parameter takes the argument's address, so it has the same problem.
void SemanticAnalyzer::checkPackedRefArguments(
    const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<sun::TypePtr>& paramTypes) const {
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    if (!paramTypes[i] || !paramTypes[i]->isReference()) continue;
    if (args[i]->getType() != ASTNodeType::MEMBER_ACCESS) continue;
    std::string ownerName;
    if (sun::packed::isFieldAccess(*args[i], &ownerName)) {
      logAndThrowError(sun::packed::borrowRejection(
                           "pass a " + sun::packed::fieldPhrase(ownerName) +
                               " to a ref parameter",
                           "Pass a copy instead."),
                       args[i]->getLocation());
    }
  }
}

// Not every field type can live in a padding-free layout.
void SemanticAnalyzer::checkPackedFieldType(
    const ClassDefinitionAST& classDef, const ClassFieldDecl& field,
    const sun::TypePtr& fieldType) const {
  if (!classDef.isPacked()) return;
  std::string reason = sun::packed::rejectFieldType(fieldType);
  if (reason.empty()) return;
  logAndThrowError("Field '" + field.name + "' in packed class '" +
                       classDef.getName() + "' " + reason,
                   field.location);
}

// -------------------------------------------------------------------
// Generic class support
