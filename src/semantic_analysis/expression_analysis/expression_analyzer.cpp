/** Checks borrows, struct literals, and bound methods and resets analyzed
 * syntax. */
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
// Borrow targets
// -------------------------------------------------------------------

void ExpressionAnalyzer::rejectBorrowOfByValueCapture(const ExprAST& target,
                                                      const Position& loc) {
  if (target.getType() != ASTNodeType::VARIABLE_REFERENCE) return;
  const auto& varRef =
      static_cast<const sun::ast::VariableReferenceAST&>(target);
  VariableInfo* varInfo = ctx_.currentScope().lookupVariable(varRef.getName());
  if (!varInfo || varInfo->captureKind != sun::ast::CaptureKind::ByValue)
    return;
  const std::string& name = varRef.getName();
  logAndThrowError(
      "Cannot borrow '" + name +
          "': the lambda captures it by value, so the reference would alias "
          "the closure's private copy, not the original. Capture it with "
          "'[ref " +
          name + "]() => ...' to share the original, or '[const ref " + name +
          "]() => ...' to read it",
      loc);
}

void ExpressionAnalyzer::validateBorrowTarget(const ExprAST& target,
                                              const Position& loc) {
  if (!isBorrowableLvalue(target)) {
    logAndThrowError(
        "Reference target must be a variable, field, or array element", loc);
  }
  rejectBorrowOfByValueCapture(target, loc);
  if (target.getType() == ASTNodeType::TERNARY) {
    const auto& ternary = static_cast<const TernaryExprAST&>(target);
    validateBorrowTarget(*ternary.getThen(), loc);
    validateBorrowTarget(*ternary.getElse(), loc);
    return;
  }
  if (target.getType() == ASTNodeType::INDEX) {
    const auto& indexExpr = static_cast<const IndexAST&>(target);
    auto baseType =
        sun::types::unwrapRef(indexExpr.getTarget()->getResolvedType());
    if (baseType && baseType->isClass()) {
      logAndThrowError(
          "Cannot create a reference to a class __index__ element - it "
          "has no storage address",
          loc);
    }
    if (indexExpr.hasSlices()) {
      logAndThrowError("Cannot create a reference to a slice", loc);
    }
  }
  sema_.classes().checkPackedFieldNotBorrowed(target, loc);
}

void ExpressionAnalyzer::analyzeStructLiteral(
    sun::ast::StructLiteralAST& literal, const TypePtr& expectedType) {
  if (!expectedType || !expectedType->isClass()) {
    logAndThrowError(
        "A '{ field: value }' literal needs a known class type. Annotate the "
        "target, as in `var x: MyClass = { ... };`.",
        literal.getLocation());
    return;
  }

  auto* classType = static_cast<sun::types::ClassType*>(expectedType.get());

  // A class with its own init is constructed through it; allowing both would
  // give two ways to build one object with different invariants.
  if (const auto* init = classType->getMethod("init");
      init && !init->isSynthesizedConstructor) {
    logAndThrowError("Class '" + classType->getDisplayName() +
                         "' declares an 'init', so construct it with "
                         "'" +
                         classType->getDisplayName() +
                         "(...)' rather than a '{ field: value }' literal.",
                     literal.getLocation());
    return;
  }

  literal.resolvedFields().clear();
  std::set<std::string> seen;
  for (auto& field : literal.getMutableFields()) {
    const sun::types::ClassField* classField =
        ctx_.accessibleField(*classType, field.name, field.location);
    if (!classField) {
      logAndThrowError("Class '" + classType->getDisplayName() +
                           "' has no field '" + field.name + "'",
                       field.location);
      continue;
    }
    if (!seen.insert(field.name).second) {
      logAndThrowError(
          "Field '" + field.name + "' is initialized more than once",
          field.location);
      continue;
    }

    literal.resolvedFields().push_back(classField->declarationId);
    sema_.analyzeExpr(*field.value, classField->type);
    TypePtr valueType = field.value->getResolvedType();
    checkMoveSource(*field.value, field.location);
    if (valueType && classField->type &&
        !isAssignableTo(valueType, classField->type)) {
      if (!tryCoerceIntegerLiteral(field.value.get(), classField->type,
                                   false)) {
        logAndThrowError(
            "Cannot initialize field '" + field.name + "' of type '" +
                classField->type->toDisplayString() +
                "' with a value of type '" + valueType->toDisplayString() + "'",
            field.location);
      }
    }
  }

  // Every field must be named. A field left out would silently be zero, which
  // is exactly the class of bug this syntax exists to prevent.
  std::string missing;
  for (const auto& classField : classType->getFields()) {
    if (seen.count(classField.name)) continue;
    if (!missing.empty()) missing += ", ";
    missing += classField.name;
  }
  if (!missing.empty()) {
    logAndThrowError("Struct literal for '" + classType->getDisplayName() +
                         "' is missing field(s): " + missing,
                     literal.getLocation());
  }

  literal.setResolvedType(expectedType);
}

void ExpressionAnalyzer::checkExternVariableAccessAllowed(
    const VariableInfo& info, const std::string& displayName,
    const Position& loc) const {
  if (!info.isCExtern || ctx_.isInUnsafeBlock()) return;
  logAndThrowError(
      "Accessing extern variable '" + displayName +
          "' requires an unsafe block: C-owned storage is outside the borrow "
          "checker's guarantees. Wrap the access in `unsafe { ... }`, or "
          "expose it through a safe Sun wrapper.",
      loc);
}

void ExpressionAnalyzer::clearResolvedTypes(ExprAST& expr) {
  expr.clearResolvedType();

  // Recursively clear based on expression type
  switch (expr.getType()) {
    case ASTNodeType::BLOCK: {
      auto& block = static_cast<BlockExprAST&>(expr);
      for (const auto& stmt : block.getBody()) {
        clearResolvedTypes(const_cast<ExprAST&>(*stmt));
      }
      break;
    }
    case ASTNodeType::BINARY: {
      auto& bin = static_cast<sun::ast::BinaryExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*bin.getLHS()));
      clearResolvedTypes(const_cast<ExprAST&>(*bin.getRHS()));
      break;
    }
    case ASTNodeType::UNARY: {
      auto& unary = static_cast<sun::ast::UnaryExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*unary.getOperand()));
      break;
    }
    case ASTNodeType::CALL: {
      auto& call = static_cast<sun::ast::CallExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*call.getCallee()));
      for (const auto& arg : call.getArgs()) {
        clearResolvedTypes(const_cast<ExprAST&>(*arg));
      }
      break;
    }
    case ASTNodeType::MEMBER_ACCESS: {
      auto& ma = static_cast<MemberAccessAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ma.getObject()));
      break;
    }
    case ASTNodeType::INDEX: {
      auto& idx = static_cast<IndexAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*idx.getTarget()));
      for (const auto& slice : idx.getIndices()) {
        if (slice->hasStart())
          clearResolvedTypes(const_cast<ExprAST&>(*slice->getStart()));
        if (slice->hasEnd())
          clearResolvedTypes(const_cast<ExprAST&>(*slice->getEnd()));
      }
      break;
    }
    case ASTNodeType::VARIABLE_CREATION: {
      auto& vc = static_cast<sun::ast::VariableCreationAST&>(expr);
      if (vc.getValue()) {
        clearResolvedTypes(const_cast<ExprAST&>(*vc.getValue()));
      }
      break;
    }
    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      auto& va = static_cast<sun::ast::VariableAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*va.getValue()));
      break;
    }
    case ASTNodeType::MEMBER_ASSIGNMENT: {
      auto& ma = static_cast<MemberAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ma.getObject()));
      clearResolvedTypes(const_cast<ExprAST&>(*ma.getValue()));
      break;
    }
    case ASTNodeType::INDEXED_ASSIGNMENT: {
      auto& ia = static_cast<sun::ast::IndexedAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ia.getTarget()));
      clearResolvedTypes(const_cast<ExprAST&>(*ia.getValue()));
      break;
    }
    case ASTNodeType::COMPOUND_ASSIGNMENT: {
      auto& ca = static_cast<sun::ast::CompoundAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ca.getTarget()));
      clearResolvedTypes(const_cast<ExprAST&>(*ca.getValue()));
      break;
    }
    case ASTNodeType::IF: {
      auto& ifExpr = static_cast<sun::ast::IfExprAST&>(expr);
      clearResolvedTypes(*ifExpr.getCond());
      clearResolvedTypes(*ifExpr.getThen());
      if (ifExpr.getElse()) {
        clearResolvedTypes(*ifExpr.getElse());
      }
      break;
    }
    case ASTNodeType::TERNARY: {
      auto& ternary = static_cast<TernaryExprAST&>(expr);
      clearResolvedTypes(*ternary.getCond());
      clearResolvedTypes(*ternary.getThen());
      clearResolvedTypes(*ternary.getElse());
      break;
    }
    case ASTNodeType::FOR_LOOP: {
      auto& loop = static_cast<sun::ast::ForExprAST&>(expr);
      if (loop.getInit())
        clearResolvedTypes(const_cast<ExprAST&>(*loop.getInit()));
      if (loop.getCondition())
        clearResolvedTypes(const_cast<ExprAST&>(*loop.getCondition()));
      if (loop.getIncrement())
        clearResolvedTypes(const_cast<ExprAST&>(*loop.getIncrement()));
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getBody()));
      break;
    }
    case ASTNodeType::FOR_IN_LOOP: {
      auto& loop = static_cast<sun::ast::ForInExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getIterable()));
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getBody()));
      break;
    }
    case ASTNodeType::WHILE_LOOP: {
      auto& loop = static_cast<sun::ast::WhileExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getCondition()));
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getBody()));
      break;
    }
    case ASTNodeType::RETURN: {
      auto& ret = static_cast<sun::ast::ReturnExprAST&>(expr);
      if (ret.hasValue()) {
        clearResolvedTypes(const_cast<ExprAST&>(*ret.getValue()));
      }
      break;
    }
    case ASTNodeType::REFERENCE_CREATION: {
      auto& ref = static_cast<sun::ast::ReferenceCreationAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ref.getTarget()));
      break;
    }
    case ASTNodeType::GENERIC_CALL: {
      auto& gc = static_cast<sun::ast::GenericCallAST&>(expr);
      for (const auto& arg : gc.getArgs()) {
        clearResolvedTypes(const_cast<ExprAST&>(*arg));
      }
      break;
    }
    case ASTNodeType::TRY_CATCH: {
      auto& tc = static_cast<sun::ast::TryCatchExprAST&>(expr);
      clearResolvedTypes(const_cast<BlockExprAST&>(tc.getTryBlock()));
      for (const auto& clause : tc.getCatchClauses()) {
        clearResolvedTypes(*clause.body);
      }
      break;
    }
    case ASTNodeType::UNSAFE_BLOCK: {
      auto& ub = static_cast<sun::ast::UnsafeBlockAST&>(expr);
      clearResolvedTypes(ub.getBody());
      break;
    }
    case ASTNodeType::THROW: {
      auto& th = static_cast<sun::ast::ThrowExprAST&>(expr);
      if (th.hasErrorExpr()) {
        clearResolvedTypes(const_cast<ExprAST&>(th.getErrorExpr()));
      }
      break;
    }
    case ASTNodeType::ARRAY_LITERAL: {
      auto& arr = static_cast<sun::ast::ArrayLiteralAST&>(expr);
      for (const auto& elem : arr.getElements()) {
        clearResolvedTypes(const_cast<ExprAST&>(*elem));
      }
      break;
    }
    case ASTNodeType::MATCH: {
      auto& match = static_cast<sun::ast::MatchExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*match.getDiscriminant()));
      for (auto& arm : match.getArmsMutable()) {
        if (arm.pattern) {
          clearResolvedTypes(*arm.pattern);
        }
        arm.resolvedVariantTag = -1;
        for (auto& binding : arm.bindings) {
          binding.resolvedType = nullptr;
        }
        clearResolvedTypes(*arm.body);
      }
      break;
    }
    // Terminal nodes (no children to recurse into)
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
    case ASTNodeType::CHAR_LITERAL:
    case ASTNodeType::BOOL_LITERAL:
    case ASTNodeType::NULL_LITERAL:
    case ASTNodeType::VARIABLE_REFERENCE:
    case ASTNodeType::THIS:
    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
      break;
    default:
      // For any other node types, just clear this node (may miss children)
      break;
  }
}

void ExpressionAnalyzer::maybeResolveBoundMethodRef(
    MemberAccessAST& memberAccess, TypePtr expectedType) {
  TypePtr objectType = unwrapRef(memberAccess.getObject()->getResolvedType());
  if (!objectType) return;

  // Unwrap raw_ptr<Class> / static_ptr<Class> (mirrors member resolution)
  if (objectType->isRawPointer()) {
    TypePtr pointee = static_cast<sun::types::RawPointerType*>(objectType.get())
                          ->getPointeeType();
    if (pointee && pointee->isClass()) objectType = pointee;
  } else if (objectType->isStaticPointer()) {
    TypePtr pointee =
        static_cast<sun::types::StaticPointerType*>(objectType.get())
            ->getPointeeType();
    if (pointee && pointee->isClass()) objectType = pointee;
  }

  const std::string& memberName = memberAccess.getMemberName();

  // Interface methods as values are not supported (would need a vtable
  // load at bind time). Only diagnose when a lambda is expected so
  // interface method calls stay untouched.
  if (objectType->isInterface() && expectedType && expectedType->isLambda()) {
    auto* ifaceType = static_cast<sun::types::InterfaceType*>(objectType.get());
    if (ifaceType->getMethod(memberName)) {
      logAndThrowError("Referencing interface method '" + memberName +
                           "' as a value is not supported",
                       memberAccess.getLocation());
    }
    return;
  }

  if (!objectType->isClass()) return;
  const auto* classType =
      static_cast<const sun::types::ClassType*>(objectType.get());
  if (classType->getField(memberName)) return;

  std::vector<const ClassMethod*> overloads;
  for (const auto& m : classType->getMethods()) {
    if (m.name == memberName) overloads.push_back(&m);
  }
  if (overloads.empty())
    return;  // not a method (member resolution already checked this)

  const ClassMethod* chosen = nullptr;
  if (overloads.size() == 1) {
    chosen = overloads[0];
  } else if (expectedType && expectedType->isLambda()) {
    // Pick the overload matching the expected lambda signature. A
    // non-throwing method may bind where a throwing lambda is expected.
    const auto* expected = static_cast<const LambdaType*>(expectedType.get());
    std::vector<const ClassMethod*> matches;
    for (const auto* m : overloads) {
      LambdaType candidate(m->returnType, m->paramTypes, m->canThrow);
      if (candidate.equalsIgnoringThrow(*expected) &&
          (expected->canThrow() || !m->canThrow)) {
        matches.push_back(m);
      }
    }
    if (matches.size() == 1) chosen = matches[0];
  }

  if (!chosen) {
    logAndThrowError("Cannot reference overloaded method '" + memberName +
                         "' as a value; add a type annotation or call it with "
                         "arguments",
                     memberAccess.getLocation());
    return;
  }

  if (chosen->isGeneric()) {
    logAndThrowError(
        "Cannot use generic method '" + memberName + "' as a value",
        memberAccess.getLocation());
    return;
  }

  // The bound method will run on this receiver later, so the receiver must
  // allow it now
  checkMethodReceiver(*memberAccess.getObject(), memberName, chosen->isConst,
                      chosen->isConstructor, memberAccess.getLocation());

  auto boundType = Types::Lambda(chosen->returnType, chosen->paramTypes,
                                 chosen->canThrow, chosen->isUnsafe);
  // A bound method holds its receiver by reference, so the value is bound
  // to the frame the receiver lives in - the same escape rules as a lambda
  // with a `[ref ...]` capture list apply to it.
  static_cast<LambdaType*>(boundType.get())->setHasRefCaptures(true);
  memberAccess.setResolvedType(std::move(boundType));
  memberAccess.setTargetDeclarationId(chosen->declarationId);
  memberAccess.setIsBoundMethodRef(true);
}

}  // namespace sun::semantic_analysis
