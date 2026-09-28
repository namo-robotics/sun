/** Provides focused checking services for one semantic analysis session. */
#pragma once
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "semantic_analysis/semantic_context.h"
/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
class SemanticAnalyzer;
/** Checks class definitions, partial classes, and packed layout restrictions.
 */
class ClassAnalyzer {
 public:
  /** Borrows the session context and the cooperating analysis services. */
  ClassAnalyzer(SemanticContext &context, SemanticAnalyzer &analyzer)
      : ctx_(context), sema_(analyzer) {}
  /** Checks a class definition, its members, and its method bodies. */
  void analyzeClassDefinition(sun::ast::ClassDefinitionAST &classDef);

  /**
   * Analyze a partial class definition. Partial classes add methods to an
   * existing primary class. If the primary has been analyzed, merges now;
   * otherwise stashes for later merging.
   */
  void analyzePartialClass(sun::ast::ClassDefinitionAST &classDef,
                           sun::ast::ExprAST &expr);

  /** A packed field has no guaranteed alignment, so it cannot be borrowed. */
  void checkPackedFieldNotBorrowed(const sun::ast::ExprAST &target,
                                   const sun::support::Position &loc) const;

  /** The same rule for an argument passed to a `ref T` parameter. */
  void checkPackedRefArguments(
      const std::vector<std::unique_ptr<sun::ast::ExprAST>> &args,
      const std::vector<sun::types::TypePtr> &paramTypes) const;

  /** Reject a field type a packed class cannot lay out. */
  void checkPackedFieldType(const sun::ast::ClassDefinitionAST &classDef,
                            const sun::ast::ClassFieldDecl &field,
                            const sun::types::TypePtr &fieldType) const;

 private:
  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;
};
}  // namespace sun::semantic_analysis
