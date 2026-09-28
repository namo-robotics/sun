/** Checks writes to module variables and their value conversions. */
#include <algorithm>
#include <cassert>
#include <set>

#include "codegen/abi/c_abi_types.h"
#include "semantic_analysis/class_analysis/field_initialization.h"
#include "semantic_analysis/expression_analysis/expression_properties.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_analysis/type_rules.h"
#include "semantic_analysis/visibility.h"
#include "support/config.h"
#include "support/error.h"

using sun::types::ClassMethod;
using sun::types::LambdaType;
using sun::types::TypePtr;
using sun::types::Types;

using sun::ast::ASTNodeType;
using sun::ast::BlockExprAST;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::ast::IndexAST;
using sun::ast::MemberAccessAST;
using sun::ast::MemberAssignmentAST;
using sun::ast::PrototypeAST;
using sun::ast::TernaryExprAST;
using sun::support::logAndThrowError;
using sun::support::Position;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::types::Type;

using sun::semantic_analysis::isBorrowableLvalue;
using sun::semantic_analysis::isReservedIdentifier;
using sun::semantic_analysis::methodVisibility;
using sun::semantic_analysis::type_analysis::isAssignableTo;
using sun::semantic_analysis::type_analysis::tryCoerceIntegerLiteral;
using sun::types::unwrapRef;

// -------------------------------------------------------------------
// Module variable assignments
// -------------------------------------------------------------------

void BodyAnalyzer::analyzeModuleGlobalAssignment(
    MemberAssignmentAST& assign, const sun::types::Type& objectType) {
  const auto& moduleType =
      static_cast<const sun::types::ModuleType&>(objectType);
  const std::string& modPath = moduleType.getModulePath();
  const std::string& memberName = assign.getMemberName();

  SymbolMatch match = ctx_.findSymbolInModule(modPath, memberName);
  if (!match) {
    logAndThrowError("Unknown member '" + memberName + "' in module '" +
                         sun::semantic_analysis::displayModulePath(modPath) +
                         "'",
                     assign.getLocation());
  }
  if (match.kind != SymbolKind::Variable || !match.variableInfo) {
    logAndThrowError(
        "Cannot assign to '" + match.display() + "': it is not a variable",
        assign.getLocation());
  }

  const VariableInfo& target = *match.variableInfo;
  // display() names the declaring module without any library-hash scope
  std::string full = target.qualifiedName.display();
  sema_.expressions().checkExternVariableAccessAllowed(target, full,
                                                       assign.getLocation());
  if (target.isConst) {
    logAndThrowError("Cannot assign to constant '" + full +
                         "'; declare it with 'var' if it must change",
                     assign.getLocation());
  }
  if (sun::types::isConstRef(target.type)) {
    logAndThrowError("Cannot assign through const reference '" + full + "'",
                     assign.getLocation());
  }

  // The declaration's own qualified name is the symbol codegen emitted the
  // global under, so that is what the write is pointed at
  assign.setQualifiedName(target.qualifiedName);
  assign.setTargetDeclarationId(target.declarationId);

  TypePtr expectedType = unwrapRef(target.type);
  sema_.analyzeExpr(const_cast<ExprAST&>(*assign.getValue()), expectedType);
  sema_.expressions().checkMoveSource(*assign.getValue(), assign.getLocation());

  TypePtr rhsType = assign.getValue()->getResolvedType();
  if (rhsType && expectedType && !isAssignableTo(rhsType, expectedType)) {
    if (!tryCoerceIntegerLiteral(const_cast<ExprAST*>(assign.getValue()),
                                 expectedType, false)) {
      logAndThrowError("Cannot assign value of type '" +
                           rhsType->toDisplayString() + "' to '" + full +
                           "' of type '" + expectedType->toDisplayString() +
                           "'",
                       assign.getLocation());
    }
  }
}

}  // namespace sun::semantic_analysis
