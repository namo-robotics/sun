/** Implements storage and control-flow queries on analyzed expressions. */
#include "semantic_analysis/expression_analysis/expression_properties.h"

#include <set>

#include "ast/ast_children.h"
#include "ast/control_flow.h"
#include "codegen/intrinsics/intrinsics.h"
#include "codegen/support/type_checks.h"
#include "semantic_analysis/type_analysis/type_traits.h"
#include "support/error.h"
#include "types/type_utils.h"

/** Checks expression properties during semantic analysis. */
namespace sun::semantic_analysis {
using sun::support::logAndThrowError;
using sun::types::TypePtr;
using sun::types::Types;
using sun::types::unwrapRef;

using sun::ast::ASTNodeType;

/** Reports whether an expression denotes storage that can be borrowed. */
bool isBorrowableLvalue(const ExprAST& target) {
  ASTNodeType kind = target.getType();
  // A conditional picks one of two slots at runtime; it borrows if both
  // branches do.
  if (kind == ASTNodeType::TERNARY) {
    const auto& ternary = static_cast<const sun::ast::TernaryExprAST&>(target);
    return isBorrowableLvalue(*ternary.getThen()) &&
           isBorrowableLvalue(*ternary.getElse());
  }
  return kind == ASTNodeType::VARIABLE_REFERENCE ||
         kind == ASTNodeType::MEMBER_ACCESS || kind == ASTNodeType::INDEX;
}

/** Reports whether an expression always leaves the current control-flow path.
 */
bool alwaysExits(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::RETURN:
    case ASTNodeType::THROW:
      return true;
    case ASTNodeType::BLOCK: {
      // One exiting statement is enough: nothing after it runs
      const auto& block = static_cast<const sun::ast::BlockExprAST&>(expr);
      for (const auto& stmt : block.getBody()) {
        if (alwaysExits(*stmt)) return true;
      }
      return false;
    }
    case ASTNodeType::UNSAFE_BLOCK:
      return alwaysExits(
          static_cast<const sun::ast::UnsafeBlockAST&>(expr).getBody());
    case ASTNodeType::IF: {
      const auto& ifExpr = static_cast<const sun::ast::IfExprAST&>(expr);
      return ifExpr.getElse() != nullptr && alwaysExits(*ifExpr.getThen()) &&
             alwaysExits(*ifExpr.getElse());
    }
    case ASTNodeType::MATCH: {
      // Every arm must exit, and no discriminant value may slip past the
      // arms: an enum match is checked for exhaustiveness elsewhere, and any
      // other match needs a wildcard to promise the same.
      const auto& matchExpr = static_cast<const sun::ast::MatchExprAST&>(expr);
      if (matchExpr.getArms().empty()) return false;
      bool sawWildcard = false;
      for (const auto& arm : matchExpr.getArms()) {
        if (!alwaysExits(*arm.body)) return false;
        if (arm.isWildcard) sawWildcard = true;
      }
      TypePtr discType =
          sun::types::unwrapRef(matchExpr.getDiscriminant()->getResolvedType());
      return sawWildcard || (discType && discType->isEnum());
    }
    case ASTNodeType::TRY_CATCH: {
      // The body may stop part-way and land in a catch, so every clause has
      // to exit as well
      const auto& tryCatch =
          static_cast<const sun::ast::TryCatchExprAST&>(expr);
      if (!alwaysExits(tryCatch.getTryBlock())) return false;
      for (const auto& clause : tryCatch.getCatchClauses()) {
        if (!clause.body || !alwaysExits(*clause.body)) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

/** Reports whether an expression contains a plain or generic call. */
bool containsCall(const ExprAST& expr) {
  if (expr.getType() == ASTNodeType::CALL ||
      expr.getType() == ASTNodeType::GENERIC_CALL)
    return true;
  bool found = false;
  sun::ast::forEachChild(expr, [&](const ExprAST& child) {
    found = found || containsCall(child);
  });
  return found;
}

/** Finds the first `this` that belongs to the enclosing receiver. */
const ExprAST* findThisUse(const ExprAST& expr) {
  if (expr.getType() == ASTNodeType::CLASS_DEFINITION ||
      expr.getType() == ASTNodeType::INTERFACE_DEFINITION)
    return nullptr;
  if (expr.getType() == ASTNodeType::THIS) return &expr;
  const ExprAST* found = nullptr;
  sun::ast::forEachChild(expr, [&found](const ExprAST& child) {
    if (!found) found = findThisUse(child);
  });
  return found;
}

/** Returns the recorded type or reports that analysis skipped the node. */
TypePtr requireResolvedType(const ExprAST& expr) {
  if (auto type = expr.getResolvedType()) return type;
  logAndThrowError(
      "Internal error: expression type was not prepared for inference",
      expr.getLocation());
}

/** Takes the type of the first return, else of the trailing value. */
TypePtr preparedBlockType(const sun::ast::BlockExprAST& block) {
  for (const auto& statement : block.getBody()) {
    if (statement->isReturn()) return requireResolvedType(*statement);
  }
  if (!block.producesValue() || block.isEmpty()) return Types::Void();
  return requireResolvedType(*block.getBody().back());
}

/** Skips repeated enum variants and stops after the first wildcard. */
std::vector<const sun::ast::MatchArm*> reachableMatchArms(
    const sun::ast::MatchExprAST& match) {
  std::vector<const sun::ast::MatchArm*> reachable;
  std::set<int64_t> coveredTags;
  for (const auto& arm : match.getArms()) {
    if (arm.bindingType) {
      bool reachable = false;
      for (auto tag : arm.matchedVariantTags)
        reachable |= coveredTags.insert(tag).second;
      if (!reachable) continue;
    } else if (!arm.isWildcard && arm.pattern &&
               arm.pattern->getResolvedType() &&
               arm.pattern->getResolvedType()->isEnum() &&
               !coveredTags.insert(arm.resolvedVariantTag).second) {
      continue;
    }
    reachable.push_back(&arm);
    if (arm.isWildcard) break;
  }
  return reachable;
}

/** Takes the type of the first reachable arm that yields a value. */
TypePtr preparedMatchType(const sun::ast::MatchExprAST& match) {
  for (const auto* arm : reachableMatchArms(match)) {
    if (!sun::ast::exprDiverges(*arm->body))
      return unwrapRef(requireResolvedType(*arm->body));
  }
  return Types::Void();
}

/** Compares each value-producing reachable arm with the match's type. */
void checkOwnedMatchArmTypes(const sun::ast::MatchExprAST& match) {
  auto resultType = match.getResolvedType();
  if (!sun::types::typeMovesOnRead(resultType)) return;
  for (const auto* arm : reachableMatchArms(match)) {
    if (sun::ast::exprDiverges(*arm->body)) continue;
    auto armType = unwrapRef(arm->body->getResolvedType());
    if (!armType || !armType->equals(*resultType)) {
      logAndThrowError("Every reachable arm must produce the same owned type",
                       arm->body->getLocation());
    }
  }
}

/** Accepts a wildcard arm, or both boolean literals over a bool. */
void checkOwnedMatchCoverage(const sun::ast::MatchExprAST& match,
                             const TypePtr& discriminantType) {
  // Owned results need a value on every path; there is no empty resource
  // that code generation can safely invent for an unmatched input.
  if (!sun::types::typeMovesOnRead(match.getResolvedType())) return;
  bool hasTrue = false;
  bool hasFalse = false;
  for (const auto& arm : match.getArms()) {
    if (arm.isWildcard) return;
    // A validated typed binding covers a direct class discriminant. Generic
    // discriminants are checked again after specialization.
    if (arm.bindingType && !arm.matchedVariantTags.empty() &&
        discriminantType &&
        (discriminantType->isClass() || discriminantType->isTypeParameter()))
      return;
    if (arm.pattern && arm.pattern->getType() == ASTNodeType::BOOL_LITERAL) {
      if (static_cast<const sun::ast::BoolLiteralAST&>(*arm.pattern)
              .getValue()) {
        hasTrue = true;
      } else {
        hasFalse = true;
      }
    }
  }
  if (discriminantType && discriminantType->isBool() && hasTrue && hasFalse)
    return;
  logAndThrowError(
      "Match producing an owned value must cover every input; add a '_' arm",
      match.getLocation());
}

/** Matches the `_is` intrinsic applied to one variable reference. */
std::optional<IsGuard> matchIsGuard(const ExprAST& cond) {
  if (cond.getType() != ASTNodeType::GENERIC_CALL) return std::nullopt;
  const auto& call = static_cast<const sun::ast::GenericCallAST&>(cond);
  if (sun::codegen::intrinsics::getIntrinsic(call.getFunctionName()) !=
      sun::codegen::intrinsics::Intrinsic::Is)
    return std::nullopt;
  const auto& args = call.getArgs();
  if (args.size() != 1 || args[0]->getType() != ASTNodeType::VARIABLE_REFERENCE)
    return std::nullopt;
  const auto& typeArgs = call.getTypeArguments();
  if (typeArgs.empty()) return std::nullopt;
  const std::string& typeName = typeArgs[0]->baseName;
  if (type_analysis::isTypeTrait(typeName)) return std::nullopt;
  return IsGuard{
      static_cast<const sun::ast::VariableReferenceAST&>(*args[0]).getName(),
      typeName};
}

/** Walks the access chain of a moved value looking for storage it must not
 * leave. */
void rejectPartialMove(const ExprAST& source,
                       const sun::support::Position& loc) {
  // Only an owned compound value moves; scalars copy and borrows stay put
  if (!sun::types::typeMovesOnRead(source.getResolvedType())) return;

  // An array owns its elements the way a container does: an element is
  // reached by borrowing it, never by moving it out of the middle
  if (source.getType() == ASTNodeType::INDEX) {
    const auto& index = static_cast<const sun::ast::IndexAST&>(source);
    TypePtr targetType = unwrapRef(index.getTarget()->getResolvedType());
    if (targetType && targetType->isArray()) {
      logAndThrowError(
          "Cannot move an element out of an array; borrow it "
          "with 'ref' or 'const ref' instead",
          loc);
    }
  }

  if (source.getType() != ASTNodeType::MEMBER_ACCESS) return;
  const ExprAST* part = &source;
  while (part && part->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto& member = static_cast<const sun::ast::MemberAccessAST&>(*part);
    if (member.hasQualifiedName() || member.isBoundMethodRef()) break;
    const ExprAST* owner = member.getObject();
    if (!owner) break;
    if (owner->getType() == ASTNodeType::THIS ||
        (owner->getResolvedType() && owner->getResolvedType()->isReference())) {
      logAndThrowError(
          "Cannot move a field through a reference; replace the field "
          "instead",
          loc);
    }
    auto ownerType = unwrapRef(owner->getResolvedType());
    if (auto* cls = sun::codegen::support::tryGetType<sun::types::ClassType>(
            ownerType)) {
      if (cls->getMethod("deinit")) {
        logAndThrowError(
            "Cannot move a field out of a class with deinit; replace the "
            "field instead",
            loc);
      }
    }
    part = owner;
  }
  if (part && part->getType() == ASTNodeType::INDEX) {
    logAndThrowError(
        "Cannot move a field out of an indexed element; replace the field "
        "instead",
        loc);
  }
}

}  // namespace sun::semantic_analysis
