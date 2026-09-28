/** Recognizes type tests used to narrow values in control flow. */
#include "codegen/intrinsics/intrinsics.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

using sun::types::TypePtr;

using sun::ast::ASTNodeType;
using sun::ast::ExprAST;
using sun::ast::MemberAccessAST;
using sun::ast::VariableReferenceAST;
using sun::support::logAndThrowError;
using sun::support::Position;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

using sun::types::unwrapRef;

// Helper: extract type guard pattern from condition
// If condition is `_is<T>(var)`, returns (varName, narrowedType)
// Works for concrete types, interfaces, and type traits
std::optional<std::pair<std::string, TypePtr>> BodyAnalyzer::extractTypeGuard(
    const ExprAST& cond) {
  // Must be a GenericCallAST with function name "_is"
  if (cond.getType() != ASTNodeType::GENERIC_CALL) return std::nullopt;

  const auto& genericCall = static_cast<const sun::ast::GenericCallAST&>(cond);
  if (sun::codegen::intrinsics::getIntrinsic(genericCall.getFunctionName()) !=
      sun::codegen::intrinsics::Intrinsic::Is) {
    return std::nullopt;
  }

  // Must have exactly one argument that is a variable reference
  const auto& args = genericCall.getArgs();
  if (args.size() != 1) return std::nullopt;
  if (args[0]->getType() != ASTNodeType::VARIABLE_REFERENCE)
    return std::nullopt;

  const auto& varRef = static_cast<const VariableReferenceAST&>(*args[0]);
  const std::string& varName = varRef.getName();

  // Get the type argument
  const auto& typeArgs = genericCall.getTypeArguments();
  const std::string& typeName = typeArgs[0]->baseName;

  // Skip type traits (_Integer, _Float, etc.) - they don't narrow to a concrete
  // type
  if (sun::semantic_analysis::type_analysis::isTypeTrait(typeName)) {
    return std::nullopt;
  }

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
