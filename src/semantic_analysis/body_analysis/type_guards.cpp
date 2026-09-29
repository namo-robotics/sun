/** Recognizes type tests used to narrow values in control flow. */
#include "semantic_analysis/expression_analysis/expression_properties.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

using sun::types::TypePtr;

using sun::ast::ExprAST;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

// Resolves the type named by an `_is<T>(x)` guard so the then-branch can
// treat x as a T. Works for interfaces, classes and primitives.
std::optional<std::pair<std::string, TypePtr>> BodyAnalyzer::extractTypeGuard(
    const ExprAST& cond) {
  auto guard = matchIsGuard(cond);
  if (!guard) return std::nullopt;
  const std::string& varName = guard->variable;
  const std::string& typeName = guard->typeName;

  // Check if it's an interface
  auto interfaceType = ctx_.lookupInterface(typeName);
  if (interfaceType) {
    return std::make_pair(varName, interfaceType);
  }

  // Check if it's a class
  auto classType = ctx_.lookupClass(typeName);
  if (classType) {
    return std::make_pair(varName, classType);
  }

  // Check if it's a primitive type
  TypePtr primType = sun::types::Types::fromString(typeName);
  if (primType) {
    return std::make_pair(varName, primType);
  }

  return std::nullopt;
}

}  // namespace sun::semantic_analysis
