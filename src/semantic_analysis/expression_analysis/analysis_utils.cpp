/** Checks analysis utils within the semantic session. */
#include "codegen/intrinsics/intrinsics.h"
#include "semantic_analysis/expression_analysis/expression_properties.h"
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
std::string ExpressionAnalyzer::immutableBaseOf(const ExprAST& place) {
  switch (place.getType()) {
    case ASTNodeType::PAREN_EXPR:
      return immutableBaseOf(
          *static_cast<const sun::ast::ParenExprAST&>(place).getInner());

    case ASTNodeType::MEMBER_ACCESS: {
      const auto& access = static_cast<const MemberAccessAST&>(place);
      TypePtr objectType = access.getObject()->getResolvedType();
      // mod.name names the module's own variable, so its own constness
      // decides — a module has no mutability of its own to inherit
      if (objectType && objectType->isModule()) {
        const auto& mod =
            static_cast<const sun::types::ModuleType&>(*objectType);
        SymbolMatch match = ctx_.findSymbolInModule(mod.getModulePath(),
                                                    access.getMemberName());
        if (match.kind != SymbolKind::Variable || !match.variableInfo) {
          return "";
        }
        // display() names the declaring module without any library-hash scope
        std::string full = match.variableInfo->qualifiedName.display();
        if (match.variableInfo->isConst) return "constant '" + full + "'";
        if (sun::types::isConstRef(match.variableInfo->type)) {
          return "const reference '" + full + "'";
        }
        return "";
      }
      // Through a mutable borrow the referent may be changed
      if (sun::types::isMutableRef(objectType)) return "";
      return immutableBaseOf(*access.getObject());
    }

    case ASTNodeType::INDEX: {
      const auto& index = static_cast<const sun::ast::IndexAST&>(place);
      if (sun::types::isMutableRef(index.getTarget()->getResolvedType()))
        return "";
      return immutableBaseOf(*index.getTarget());
    }

    case ASTNodeType::TERNARY: {
      const auto& ternary = static_cast<const sun::ast::TernaryExprAST&>(place);
      std::string why = immutableBaseOf(*ternary.getThen());
      return why.empty() ? immutableBaseOf(*ternary.getElse()) : why;
    }

    case ASTNodeType::THIS: {
      VariableInfo* info = ctx_.currentScope().lookupVariable("this");
      if (info && info->isConst) return "'this' inside a const method";
      return "";
    }

    case ASTNodeType::VARIABLE_REFERENCE: {
      const auto& ref = static_cast<const VariableReferenceAST&>(place);
      VariableInfo* info = ctx_.currentScope().lookupVariable(ref.getName());
      if (!info) return "";
      if (info->isConst) return "constant '" + ref.getName() + "'";
      if (sun::types::isConstRef(info->type))
        return "const reference '" + ref.getName() + "'";
      return "";
    }

    default:
      // A call result or other temporary: only a const borrow is frozen
      if (sun::types::isConstRef(place.getResolvedType())) {
        return "a const reference";
      }
      return "";
  }
}

void ExpressionAnalyzer::requireMutablePlace(const ExprAST& place,
                                             const std::string& action,
                                             const Position& loc) {
  std::string why = immutableBaseOf(place);
  if (!why.empty()) {
    logAndThrowError("Cannot " + action + " " + why, loc);
  }
}

void ExpressionAnalyzer::checkMoveSource(const ExprAST& value,
                                         const Position& loc) {
  const ExprAST* source = &value;
  while (source->getType() == ASTNodeType::PAREN_EXPR) {
    source = static_cast<const sun::ast::ParenExprAST*>(source)->getInner();
  }
  // Only an owned compound value moves; scalars copy and borrows stay put
  if (!sun::types::typeMovesOnRead(source->getResolvedType())) return;
  rejectPartialMove(*source, loc);

  if (source->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto& access = static_cast<const MemberAccessAST&>(*source);
    std::string why = immutableBaseOf(*access.getObject());
    if (!why.empty()) {
      logAndThrowError("Cannot move field '" + access.getMemberName() +
                           "' out of " + why +
                           "; borrow it with 'const ref' or copy it with "
                           "clone()",
                       loc);
    }
  } else if (source->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& ref = static_cast<const VariableReferenceAST&>(*source);
    VariableInfo* info = ctx_.currentScope().lookupVariable(ref.getName());
    if (info && info->isConst && info->isGlobal) {
      logAndThrowError("Cannot move constant global '" + ref.getName() +
                           "'; borrow it with 'const ref' or copy it with "
                           "clone()",
                       loc);
    }
  }
}

void ExpressionAnalyzer::checkArgumentPlaces(
    const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<TypePtr>& paramTypes, const std::string& callee,
    const Position& loc) {
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    if (!args[i] || !paramTypes[i]) continue;
    TypePtr argType = args[i]->getResolvedType();
    // Any reference parameter borrows its argument, const or not
    if (paramTypes[i]->isReference()) {
      rejectBorrowOfByValueCapture(*args[i], loc);
    }
    if (sun::types::isMutableRef(paramTypes[i])) {
      // A reference argument is checked by assignability (const ref never
      // becomes ref); a place argument is borrowed here
      if (argType && argType->isReference()) continue;
      requireMutablePlace(*args[i],
                          "pass as 'ref' argument " + std::to_string(i + 1) +
                              " of '" + callee + "'",
                          loc);
    } else if (!paramTypes[i]->isReference()) {
      checkMoveSource(*args[i], loc);
    }
  }
}

void ExpressionAnalyzer::checkUnsafeCall(bool requiresUnsafe,
                                         const std::string& name,
                                         const Position& loc) const {
  if (!requiresUnsafe || ctx_.isInUnsafeBlock()) return;
  logAndThrowError("Calling unsafe method '" + name +
                       "' requires an unsafe block or expression. Use `unsafe "
                       "...` or `unsafe { ... }`.",
                   loc);
}

bool ExpressionAnalyzer::checkMethodReceiver(const ExprAST& receiver,
                                             const std::string& name,
                                             bool methodIsConst,
                                             bool isConstructor,
                                             const Position& loc) {
  std::string why = immutableBaseOf(receiver);
  if (why.empty()) return false;
  if (!methodIsConst && !isConstructor) {
    logAndThrowError("Cannot call non-const method '" + name + "' on " + why +
                         "; declare it 'const method' if it does not "
                         "change the object",
                     loc);
  }
  return true;
}

}  // namespace sun::semantic_analysis
