// analysis_utils.cpp — Places and constness, plus `_is<T>` type guards
//
// `immutableBaseOf` is the one predicate every write, borrow, `ref` argument,
// receiver and move site consults; the checks below it are its callers.
// The stateless type rules live in type_rules.cpp.

#include "codegen/intrinsics/intrinsics.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

using sun::unwrapRef;

// Helper: extract type guard pattern from condition
// If condition is `_is<T>(var)`, returns (varName, narrowedType)
// Works for concrete types, interfaces, and type traits
std::optional<std::pair<std::string, sun::TypePtr>>
SemanticAnalyzer::extractTypeGuard(const ExprAST& cond) {
  // Must be a GenericCallAST with function name "_is"
  if (cond.getType() != ASTNodeType::GENERIC_CALL) return std::nullopt;

  const auto& genericCall = static_cast<const GenericCallAST&>(cond);
  if (sun::getIntrinsic(genericCall.getFunctionName()) != sun::Intrinsic::Is) {
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
  if (sun::isTypeTrait(typeName)) {
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
  sun::TypePtr primType = sun::Types::fromString(typeName);
  if (primType) {
    return std::make_pair(varName, primType);
  }

  return std::nullopt;
}

// -------------------------------------------------------------------
// Constness
// -------------------------------------------------------------------

std::string SemanticAnalyzer::immutableBaseOf(const ExprAST& place) {
  switch (place.getType()) {
    case ASTNodeType::PAREN_EXPR:
      return immutableBaseOf(
          *static_cast<const ParenExprAST&>(place).getInner());

    case ASTNodeType::MEMBER_ACCESS: {
      const auto& access = static_cast<const MemberAccessAST&>(place);
      sun::TypePtr objectType = access.getObject()->getResolvedType();
      // mod.name names the module's own variable, so its own constness
      // decides — a module has no mutability of its own to inherit
      if (objectType && objectType->isModule()) {
        const auto& mod = static_cast<const sun::ModuleType&>(*objectType);
        SymbolMatch match = ctx_.findSymbolInModule(mod.getModulePath(),
                                                    access.getMemberName());
        if (match.kind != SymbolKind::Variable || !match.variableInfo) {
          return "";
        }
        // display() names the declaring module without any library-hash scope
        std::string full = match.variableInfo->qualifiedName.display();
        if (match.variableInfo->isConst) return "constant '" + full + "'";
        if (sun::isConstRef(match.variableInfo->type)) {
          return "const reference '" + full + "'";
        }
        return "";
      }
      // Through a mutable borrow the referent may be changed
      if (sun::isMutableRef(objectType)) return "";
      return immutableBaseOf(*access.getObject());
    }

    case ASTNodeType::INDEX: {
      const auto& index = static_cast<const IndexAST&>(place);
      if (sun::isMutableRef(index.getTarget()->getResolvedType())) return "";
      return immutableBaseOf(*index.getTarget());
    }

    case ASTNodeType::TERNARY: {
      const auto& ternary = static_cast<const TernaryExprAST&>(place);
      std::string why = immutableBaseOf(*ternary.getThen());
      return why.empty() ? immutableBaseOf(*ternary.getElse()) : why;
    }

    case ASTNodeType::THIS: {
      VariableInfo* info = ctx_.lookupVariable("this");
      if (info && info->isConst) return "'this' inside a const method";
      return "";
    }

    case ASTNodeType::VARIABLE_REFERENCE: {
      const auto& ref = static_cast<const VariableReferenceAST&>(place);
      VariableInfo* info = ctx_.lookupVariable(ref.getName());
      if (!info) return "";
      if (info->isConst) return "constant '" + ref.getName() + "'";
      if (sun::isConstRef(info->type))
        return "const reference '" + ref.getName() + "'";
      return "";
    }

    default:
      // A call result or other temporary: only a const borrow is frozen
      if (sun::isConstRef(place.getResolvedType())) {
        return "a const reference";
      }
      return "";
  }
}

void SemanticAnalyzer::requireMutablePlace(const ExprAST& place,
                                           const std::string& action,
                                           const Position& loc) {
  std::string why = immutableBaseOf(place);
  if (!why.empty()) {
    logAndThrowError("Cannot " + action + " " + why, loc);
  }
}

void SemanticAnalyzer::checkMoveSource(const ExprAST& value,
                                       const Position& loc) {
  const ExprAST* source = &value;
  while (source->getType() == ASTNodeType::PAREN_EXPR) {
    source = static_cast<const ParenExprAST*>(source)->getInner();
  }
  sun::TypePtr type = source->getResolvedType();
  // Only an owned compound value moves; scalars copy and borrows stay put
  if (!sun::typeMovesOnRead(type)) return;

  // An array owns its elements the way a container does: an element is
  // reached by borrowing it, never by moving it out of the middle
  if (source->getType() == ASTNodeType::INDEX) {
    const auto& index = static_cast<const IndexAST&>(*source);
    sun::TypePtr targetType = unwrapRef(index.getTarget()->getResolvedType());
    if (targetType && targetType->isArray()) {
      logAndThrowError(
          "Cannot move an element out of an array; borrow it "
          "with 'ref' or 'const ref' instead",
          loc);
    }
  }

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
    VariableInfo* info = ctx_.lookupVariable(ref.getName());
    if (info && info->isConst && info->isGlobal) {
      logAndThrowError("Cannot move constant global '" + ref.getName() +
                           "'; borrow it with 'const ref' or copy it with "
                           "clone()",
                       loc);
    }
  }
}

void SemanticAnalyzer::checkArgumentPlaces(
    const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<sun::TypePtr>& paramTypes, const std::string& callee,
    const Position& loc) {
  for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
    if (!args[i] || !paramTypes[i]) continue;
    sun::TypePtr argType = args[i]->getResolvedType();
    // Any reference parameter borrows its argument, const or not
    if (paramTypes[i]->isReference()) {
      rejectBorrowOfByValueCapture(*args[i], loc);
    }
    if (sun::isMutableRef(paramTypes[i])) {
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

bool SemanticAnalyzer::checkMethodReceiver(const ExprAST& receiver,
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
