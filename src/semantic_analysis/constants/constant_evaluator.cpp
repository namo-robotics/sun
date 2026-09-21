// constant_evaluator.cpp — Computes file-scope initializers at compile time
// (see constant_evaluator.h)

#include "semantic_analysis/constants/constant_evaluator.h"

#include <llvm/ADT/APSInt.h>

#include <algorithm>
#include <functional>
#include <unordered_map>

#include "semantic_analysis/type_analysis/type_checking.h"
#include "types/enum_type.h"
#include "types/type_utils.h"

using llvm::APFloat;
using llvm::APInt;
using sun::ast::ASTNodeType;
using sun::ast::ExprAST;
using sun::parsing::TokenKind;
using sun::support::Position;
using SunType = sun::types::Type;
using sun::types::TypePtr;

/** Evaluates constant expressions while a program is being analyzed. */
namespace sun::semantic_analysis::constants {

/** Helpers private to the evaluator. */
namespace {

/** The type of an analyzed expression, looking through a reference. */
TypePtr getValueType(const ExprAST& expr) {
  return sun::types::unwrapRef(expr.getResolvedType());
}

/** Builds an integer-held value of the given type. */
ConstantValue makeInteger(TypePtr type, APInt bits) {
  return ConstantValue{std::move(type), std::move(bits)};
}

/** Builds a bool value. */
ConstantValue makeBool(bool value) {
  return makeInteger(sun::types::Types::Bool(), APInt(1, value ? 1 : 0));
}

/**
 * Widens an integer to `width` bits the way generated code does: by the
 * signedness of the type the value came from.
 */
APInt widenInteger(const ConstantValue& value, unsigned width) {
  const APInt& bits = value.getInteger();
  if (bits.getBitWidth() >= width) return bits;
  return value.isUnsigned() ? bits.zext(width) : bits.sext(width);
}

/**
 * Reads a value as a truth value the way `and` and `or` do in generated code:
 * a bool as itself, an integer as "not zero", a float as "ordered and not
 * zero". Nothing for any other kind of value.
 */
std::optional<bool> readTruthValue(const ConstantValue& value) {
  if (value.isInteger()) return !value.getInteger().isZero();
  if (value.isFloat())
    return !value.getFloat().isNaN() && !value.getFloat().isZero();
  return std::nullopt;
}

/** A short description of what an expression does, for a startup reason. */
std::string describeUnsupported(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::CALL:
      if (auto type = getValueType(expr); type && type->isClass())
        return "constructs a '" + type->toDisplayString() + "' value";
      return "calls a function";
    case ASTNodeType::GENERIC_CALL:
      return "calls '" +
             static_cast<const sun::ast::GenericCallAST&>(expr)
                 .getFunctionName() +
             "'";
    case ASTNodeType::STRUCT_LITERAL:
      return "builds a class value";
    case ASTNodeType::LAMBDA:
      return "is a lambda";
    case ASTNodeType::UNSAFE_BLOCK:
      return "contains an unsafe block";
    case ASTNodeType::NULL_LITERAL:
      return "is a null pointer";
    case ASTNodeType::INDEX:
    case ASTNodeType::ARRAY_INDEX:
    case ASTNodeType::SLICE:
      return "takes a range of an array";
    default:
      return "uses an expression that is only evaluated at run time";
  }
}

}  // namespace

// -------------------------------------------------------------------
// Deciding globals
// -------------------------------------------------------------------

void ConstantEvaluator::evaluateGlobals(const sun::ast::BlockExprAST& block) {
  decideGlobals(block);
  checkStartupOrder(block);
}

void ConstantEvaluator::decideGlobals(const sun::ast::BlockExprAST& block) {
  for (const auto& node : block.getBody()) {
    switch (node->getType()) {
      case ASTNodeType::MODULE:
        decideGlobals(static_cast<const sun::ast::ModuleAST&>(*node).getBody());
        break;
      case ASTNodeType::MOON_SCOPE:
        decideGlobals(
            static_cast<const sun::ast::MoonScopeAST&>(*node).getBody());
        break;
      case ASTNodeType::VARIABLE_CREATION: {
        const auto& global =
            static_cast<const sun::ast::VariableCreationAST&>(*node);
        if (!global.getGlobalInit()) evaluateGlobalInitializer(global);
        break;
      }
      default:
        break;
    }
  }
}

void ConstantEvaluator::collectInitializationOrder(
    const sun::ast::BlockExprAST& block,
    std::vector<const sun::ast::VariableCreationAST*>& order) const {
  for (const auto& node : block.getBody()) {
    switch (node->getType()) {
      case ASTNodeType::MODULE:
        collectInitializationOrder(
            static_cast<const sun::ast::ModuleAST&>(*node).getBody(), order);
        break;
      case ASTNodeType::MOON_SCOPE:
        collectInitializationOrder(
            static_cast<const sun::ast::MoonScopeAST&>(*node).getBody(), order);
        break;
      case ASTNodeType::VARIABLE_CREATION:
        order.push_back(
            &static_cast<const sun::ast::VariableCreationAST&>(*node));
        break;
      default:
        break;
    }
  }
}

const sun::ast::VariableCreationAST* ConstantEvaluator::findGlobalNode(
    DeclarationId target) const {
  if (!target) return nullptr;
  const ExprAST* node = declarations_.get(target).astNode;
  if (!node || node->getType() != ASTNodeType::VARIABLE_CREATION)
    return nullptr;
  return static_cast<const sun::ast::VariableCreationAST*>(node);
}

void ConstantEvaluator::checkStartupOrder(const sun::ast::BlockExprAST& block) {
  std::vector<const sun::ast::VariableCreationAST*> order;
  collectInitializationOrder(block, order);
  std::unordered_map<const sun::ast::VariableCreationAST*, size_t> position;
  for (size_t i = 0; i < order.size(); ++i) position[order[i]] = i;

  // A library's globals and C globals are ready before any of these run.
  auto initializedAtStartup = [](const sun::ast::VariableCreationAST& global) {
    const GlobalInitRecord* decision = global.getGlobalInit();
    return decision && decision->kind == GlobalInitKind::Startup &&
           !global.isPrecompiled() && !global.isCExtern();
  };

  for (size_t i = 0; i < order.size(); ++i) {
    const auto& reader = *order[i];
    if (!initializedAtStartup(reader) || !reader.getValue()) continue;

    // Only reads the initializer performs itself: a lambda's body runs
    // whenever the lambda is called, which may be much later.
    std::function<void(const ExprAST&)> visit = [&](const ExprAST& expr) {
      if (expr.getType() == ASTNodeType::LAMBDA) return;
      if (expr.getType() == ASTNodeType::VARIABLE_REFERENCE ||
          expr.getType() == ASTNodeType::MEMBER_ACCESS ||
          expr.getType() == ASTNodeType::QUALIFIED_NAME) {
        const auto* read = findGlobalNode(expr.getTargetDeclarationId());
        auto found = read ? position.find(read) : position.end();
        if (found != position.end() && found->second > i &&
            initializedAtStartup(*read)) {
          sun::support::logAndThrowError(
              "'" + reader.getName() + "' is initialized before '" +
                  read->getName() + "', which it reads; move '" +
                  read->getName() + "' above '" + reader.getName() + "'",
              expr.getLocation());
        }
      }
      const_cast<ExprAST&>(expr).forEachChildSlot(
          [&](std::unique_ptr<ExprAST>& child) {
            if (child) visit(*child);
          });
    };
    visit(*reader.getValue());
  }
}

const GlobalInitRecord& ConstantEvaluator::evaluateGlobalInitializer(
    const sun::ast::VariableCreationAST& global) {
  GlobalInitRecord record;
  record.isConst = global.isConst();
  // Deciding this variable may interrupt the evaluation of one that reads it
  std::optional<StartupReason> interrupted = std::move(blocker_);
  blocker_.reset();
  deciding_.push_back(&global);
  if (deciding_.size() == 1) steps_ = 0;

  std::optional<ConstantValue> value;
  if (global.isCExtern()) {
    recordBlocker("it is defined by C code", global.getLocation());
  } else if (global.isPrecompiled()) {
    recordBlocker(
        "it is defined in a precompiled library that gives it no "
        "compile-time value",
        global.getLocation());
  } else if (!global.getValue() || !global.getResolvedType()) {
    recordBlocker("it has no analyzed initializer", global.getLocation());
  } else {
    value = evaluateExpression(*global.getValue());
    if (value)
      value = convertToDeclaredType(
          std::move(*value), sun::types::unwrapRef(global.getResolvedType()),
          global.getValue()->getLocation());
  }

  if (value) {
    record.kind = GlobalInitKind::Image;
    record.value = std::move(value);
  } else {
    record.kind = GlobalInitKind::Startup;
    if (!blocker_)
      recordBlocker("its initializer cannot be evaluated at compile time",
                    global.getLocation());
    record.reason = std::move(blocker_);
  }
  blocker_ = std::move(interrupted);
  deciding_.pop_back();
  global.setGlobalInit(std::move(record));
  return *global.getGlobalInit();
}

void ConstantEvaluator::recordBlocker(std::string message,
                                      const Position& position) {
  if (!blocker_) blocker_ = StartupReason{std::move(message), position};
}

// -------------------------------------------------------------------
// Expressions
// -------------------------------------------------------------------

std::optional<ConstantValue> ConstantEvaluator::evaluateExpression(
    const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::NUMBER:
    case ASTNodeType::BOOL_LITERAL:
    case ASTNodeType::CHAR_LITERAL:
    case ASTNodeType::STRING_LITERAL:
      return evaluateLiteral(expr);
    case ASTNodeType::PAREN_EXPR:
      return evaluateExpression(
          *static_cast<const sun::ast::ParenExprAST&>(expr).getInner());
    case ASTNodeType::VARIABLE_REFERENCE: {
      const auto& reference =
          static_cast<const sun::ast::VariableReferenceAST&>(expr);
      // Inside a function being evaluated, its own variables come first
      if (!frames_.empty()) {
        auto local = frames_.back().find(reference.getTargetDeclarationId());
        if (local != frames_.back().end()) return local->second;
      }
      return evaluateGlobalRead(reference.getTargetDeclarationId(),
                                reference.getName(), expr.getLocation());
    }
    case ASTNodeType::MEMBER_ACCESS:
      return evaluateMemberAccess(
          static_cast<const sun::ast::MemberAccessAST&>(expr));
    case ASTNodeType::BINARY:
      return evaluateBinary(static_cast<const sun::ast::BinaryExprAST&>(expr));
    case ASTNodeType::UNARY:
      return evaluateUnary(static_cast<const sun::ast::UnaryExprAST&>(expr));
    case ASTNodeType::TERNARY:
      return evaluateTernary(
          static_cast<const sun::ast::TernaryExprAST&>(expr));
    case ASTNodeType::ARRAY_LITERAL:
      return evaluateArrayLiteral(
          static_cast<const sun::ast::ArrayLiteralAST&>(expr));
    case ASTNodeType::GENERIC_CALL: {
      const auto& call = static_cast<const sun::ast::GenericCallAST&>(expr);
      if (call.getFunctionName() == "_convert") return evaluateConvert(call);
      break;
    }
    case ASTNodeType::CALL:
      return evaluateCall(static_cast<const sun::ast::CallExprAST&>(expr));
    case ASTNodeType::INDEX: {
      const auto& index = static_cast<const sun::ast::IndexAST&>(expr);
      std::vector<const ExprAST*> indices;
      for (const auto& slice : index.getIndices()) {
        // A range takes part of the array, which is a view, not a value
        if (slice->isRange() || !slice->hasStart() || slice->hasEnd()) {
          indices.clear();
          break;
        }
        indices.push_back(slice->getStart());
      }
      if (indices.empty()) break;
      return evaluateIndex(*index.getTarget(), indices, expr.getLocation());
    }
    case ASTNodeType::ARRAY_INDEX: {
      const auto& index = static_cast<const sun::ast::ArrayIndexAST&>(expr);
      std::vector<const ExprAST*> indices;
      for (const auto& element : index.getIndices())
        indices.push_back(element.get());
      return evaluateIndex(*index.getArray(), indices, expr.getLocation());
    }
    default:
      break;
  }
  recordBlocker("its initializer " + describeUnsupported(expr),
                expr.getLocation());
  return std::nullopt;
}

std::optional<ConstantValue> ConstantEvaluator::evaluateLiteral(
    const ExprAST& expr) {
  TypePtr type = getValueType(expr);
  switch (expr.getType()) {
    case ASTNodeType::BOOL_LITERAL:
      return makeBool(
          static_cast<const sun::ast::BoolLiteralAST&>(expr).getValue());
    case ASTNodeType::CHAR_LITERAL: {
      // b'a' is a byte; 'a' is a char, stored in 32 bits.
      const auto& literal = static_cast<const sun::ast::CharLiteralAST&>(expr);
      if (!type)
        type = literal.isByte() ? sun::types::Types::UInt8()
                                : sun::types::Types::Char();
      return makeInteger(type,
                         APInt(literal.isByte() ? 8 : 32, literal.getValue()));
    }
    case ASTNodeType::STRING_LITERAL:
      return ConstantValue{
          type,
          static_cast<const sun::ast::StringLiteralAST&>(expr).getValue()};
    default:
      break;
  }

  const auto& number = static_cast<const sun::ast::NumberExprAST&>(expr);
  if (number.isFloat()) {
    // A float literal is 64-bit unless its context typed it as 32-bit, in
    // which case it is rounded to the nearest 32-bit value.
    APFloat value(number.getFloatVal());
    if (type && type->isFloat32()) {
      bool losesInfo = false;
      value.convert(APFloat::IEEEsingle(), APFloat::rmNearestTiesToEven,
                    &losesInfo);
      return ConstantValue{type, value};
    }
    return ConstantValue{sun::types::Types::Float64(), value};
  }

  // Analysis has range-checked the literal against its type, so the value is
  // the low bits of its 64-bit two's-complement pattern. Without a usable
  // type the literal is 32-bit when it fits and 64-bit otherwise.
  std::optional<unsigned> width;
  if (type) width = getIntegerBitWidth(*type);
  if (!width) {
    bool fits = sun::semantic_analysis::type_analysis::literalFitsInType(
        number.getMagnitude(), number.isNegative(), SunType::Kind::Int32);
    type = fits ? sun::types::Types::Int32() : sun::types::Types::Int64();
    width = fits ? 32 : 64;
  }
  if (type->isBool()) return makeBool(number.getIntegerBits() != 0);
  return makeInteger(type, APInt(64, number.getIntegerBits()).trunc(*width));
}

std::optional<ConstantValue> ConstantEvaluator::evaluateGlobalRead(
    DeclarationId target, const std::string& name, const Position& position) {
  // The decision lives on the node that declares the variable being read.
  const GlobalInitRecord* record = nullptr;
  if (const auto* global = findGlobalNode(target)) {
    record = global->getGlobalInit();
    // Globals may be read before the line that declares them, so the one
    // read here may not have had its turn yet. Analysis has already refused
    // a variable that depends on itself.
    const bool beingDecided = std::find(deciding_.begin(), deciding_.end(),
                                        global) != deciding_.end();
    if (!record && !beingDecided && global->getResolvedType())
      record = &evaluateGlobalInitializer(*global);
  }
  if (!record) {
    recordBlocker(
        "it reads '" + name + "', whose value is not known at compile time",
        position);
    return std::nullopt;
  }
  // A `var` may have been changed by the time the reader runs, so only a
  // `const` whose own value is in the image can be read here.
  if (!record->isConst) {
    recordBlocker("it reads '" + name + "', which is a 'var'", position);
    return std::nullopt;
  }
  if (record->kind != GlobalInitKind::Image || !record->value) {
    recordBlocker("it reads '" + name + "', which is initialized at startup",
                  position);
    return std::nullopt;
  }
  return record->value;
}

std::optional<ConstantValue> ConstantEvaluator::evaluateMemberAccess(
    const sun::ast::MemberAccessAST& access) {
  TypePtr objectType = access.getObject()->getResolvedType();
  if (objectType && objectType->isModule())
    return evaluateGlobalRead(access.getTargetDeclarationId(),
                              access.getMemberName(), access.getLocation());

  // A variant of a payload-free enum is the integer the enum is stored as.
  if (objectType && objectType->isEnum()) {
    const auto& enumType =
        static_cast<const sun::types::EnumType&>(*objectType);
    const auto* variant = enumType.getVariant(access.getMemberName());
    auto width = getIntegerBitWidth(enumType);
    if (variant && width && !variant->hasPayload())
      return makeInteger(
          objectType,
          APInt(64, static_cast<uint64_t>(variant->value)).trunc(*width));
  }
  recordBlocker("its initializer reads a member of a value",
                access.getLocation());
  return std::nullopt;
}

std::optional<ConstantValue> ConstantEvaluator::evaluateUnary(
    const sun::ast::UnaryExprAST& unary) {
  auto operand = evaluateExpression(*unary.getOperand());
  if (!operand) return std::nullopt;
  TypePtr type = getValueType(unary);
  if (!type) type = operand->type;

  switch (unary.getOp().kind) {
    case TokenKind::MINUS:
      if (operand->isFloat()) {
        APFloat negated = operand->getFloat();
        negated.changeSign();
        return ConstantValue{type, negated};
      }
      // Two's-complement negation, which wraps for the most negative value.
      if (operand->isInteger())
        return makeInteger(type, -operand->getInteger());
      break;
    case TokenKind::NOT:
    case TokenKind::TILDE:
      if (operand->isInteger())
        return makeInteger(type, ~operand->getInteger());
      break;
    default:
      break;
  }
  recordBlocker(
      "its initializer uses an operator the compiler cannot "
      "evaluate",
      unary.getLocation());
  return std::nullopt;
}

std::optional<ConstantValue> ConstantEvaluator::evaluateLogical(
    const sun::ast::BinaryExprAST& binary) {
  const bool isAnd = binary.getOp().kind == TokenKind::AND;
  auto left = evaluateExpression(*binary.getLHS());
  if (!left) return std::nullopt;
  auto leftTruth = readTruthValue(*left);
  // `and` is decided by a false left side and `or` by a true one; the right
  // side is then never evaluated, so nothing in it can stand in the way.
  if (leftTruth && *leftTruth != isAnd) return makeBool(!isAnd);

  auto right = evaluateExpression(*binary.getRHS());
  if (!right) return std::nullopt;
  auto rightTruth = readTruthValue(*right);
  if (!leftTruth || !rightTruth) {
    recordBlocker(
        "its initializer applies a logical operator to a value "
        "that is not a truth value",
        binary.getLocation());
    return std::nullopt;
  }
  return makeBool(*rightTruth);
}

std::optional<ConstantValue> ConstantEvaluator::evaluateBinary(
    const sun::ast::BinaryExprAST& binary) {
  const TokenKind op = binary.getOp().kind;
  if (op == TokenKind::AND || op == TokenKind::OR)
    return evaluateLogical(binary);

  auto left = evaluateExpression(*binary.getLHS());
  if (!left) return std::nullopt;
  auto right = evaluateExpression(*binary.getRHS());
  if (!right) return std::nullopt;

  return combineOperands(op, std::move(*left), std::move(*right),
                         getValueType(*binary.getLHS()), getValueType(binary),
                         binary.getLocation());
}

std::optional<ConstantValue> ConstantEvaluator::combineOperands(
    TokenKind op, ConstantValue left, ConstantValue right,
    const TypePtr& leftType, const TypePtr& resultType,
    const Position& position) {
  // Generated code brings both operands to one width before operating: the
  // narrower integer is widened by its own signedness, and a 32-bit float
  // becomes 64-bit. Integers and floats are never mixed.
  if (left.isInteger() && right.isInteger()) {
    unsigned width = std::max(left.getInteger().getBitWidth(),
                              right.getInteger().getBitWidth());
    APInt leftBits = widenInteger(left, width);
    APInt rightBits = widenInteger(right, width);
    left.data = leftBits;
    right.data = rightBits;
  } else if (left.isFloat() && right.isFloat()) {
    const bool leftIsSingle =
        &left.getFloat().getSemantics() == &APFloat::IEEEsingle();
    const bool rightIsSingle =
        &right.getFloat().getSemantics() == &APFloat::IEEEsingle();
    if (leftIsSingle != rightIsSingle) {
      ConstantValue& narrower = leftIsSingle ? left : right;
      APFloat widened = narrower.getFloat();
      bool losesInfo = false;
      widened.convert(APFloat::IEEEdouble(), APFloat::rmNearestTiesToEven,
                      &losesInfo);
      narrower.data = widened;
    }
  } else {
    recordBlocker(
        "its initializer applies an operator to values the "
        "compiler cannot combine at compile time",
        position);
    return std::nullopt;
  }

  // Signedness of division, remainder, right shift and ordering follows the
  // left operand's type, as it does in generated code.
  bool unsignedOperation = leftType && leftType->isUnsigned();
  return applyBinaryOperator(op, left, right, unsignedOperation, resultType,
                             position);
}

std::optional<ConstantValue> ConstantEvaluator::applyBinaryOperator(
    TokenKind op, const ConstantValue& left, const ConstantValue& right,
    bool unsignedOperation, const TypePtr& resultType,
    const Position& position) {
  auto refuse = [&](const std::string& what) -> std::optional<ConstantValue> {
    recordBlocker("its initializer " + what, position);
    return std::nullopt;
  };

  // ---- Floats ----
  if (left.isFloat()) {
    const APFloat& a = left.getFloat();
    const APFloat& b = right.getFloat();
    const APFloat::cmpResult order = a.compare(b);
    const bool unordered = order == APFloat::cmpUnordered;
    switch (op) {
      // Ordering is true when either side is not a number, because generated
      // code uses the unordered comparisons; equality is not.
      case TokenKind::LESS:
        return makeBool(unordered || order == APFloat::cmpLessThan);
      case TokenKind::LESS_EQUAL:
        return makeBool(unordered || order == APFloat::cmpLessThan ||
                        order == APFloat::cmpEqual);
      case TokenKind::GREATER:
        return makeBool(unordered || order == APFloat::cmpGreaterThan);
      case TokenKind::GREATER_EQUAL:
        return makeBool(unordered || order == APFloat::cmpGreaterThan ||
                        order == APFloat::cmpEqual);
      case TokenKind::EQUAL_EQUAL:
        return makeBool(order == APFloat::cmpEqual);
      case TokenKind::NOT_EQUAL:
        return makeBool(!unordered && order != APFloat::cmpEqual);
      default:
        break;
    }
    APFloat result = a;
    switch (op) {
      case TokenKind::PLUS:
        result.add(b, APFloat::rmNearestTiesToEven);
        break;
      case TokenKind::MINUS:
        result.subtract(b, APFloat::rmNearestTiesToEven);
        break;
      case TokenKind::STAR:
        result.multiply(b, APFloat::rmNearestTiesToEven);
        break;
      case TokenKind::SLASH:
        result.divide(b, APFloat::rmNearestTiesToEven);
        break;
      default:
        return refuse("applies an integer operator to a float");
    }
    TypePtr type = &result.getSemantics() == &APFloat::IEEEsingle()
                       ? sun::types::Types::Float32()
                       : sun::types::Types::Float64();
    return ConstantValue{type, result};
  }

  // ---- Integers ----
  const APInt& a = left.getInteger();
  const APInt& b = right.getInteger();
  switch (op) {
    case TokenKind::LESS:
      return makeBool(unsignedOperation ? a.ult(b) : a.slt(b));
    case TokenKind::LESS_EQUAL:
      return makeBool(unsignedOperation ? a.ule(b) : a.sle(b));
    case TokenKind::GREATER:
      return makeBool(unsignedOperation ? a.ugt(b) : a.sgt(b));
    case TokenKind::GREATER_EQUAL:
      return makeBool(unsignedOperation ? a.uge(b) : a.sge(b));
    case TokenKind::EQUAL_EQUAL:
      return makeBool(a == b);
    case TokenKind::NOT_EQUAL:
      return makeBool(a != b);
    default:
      break;
  }

  APInt result(a.getBitWidth(), 0);
  switch (op) {
    // Addition, subtraction and multiplication wrap around.
    case TokenKind::PLUS:
      result = a + b;
      break;
    case TokenKind::MINUS:
      result = a - b;
      break;
    case TokenKind::STAR:
      result = a * b;
      break;
    case TokenKind::SLASH:
    case TokenKind::PERCENT: {
      // Generated code gives these no defined result, so neither do we.
      if (b.isZero()) return refuse("divides an integer by zero");
      if (!unsignedOperation && a.isMinSignedValue() && b.isAllOnes())
        return refuse("divides the most negative integer by -1");
      if (op == TokenKind::SLASH)
        result = unsignedOperation ? a.udiv(b) : a.sdiv(b);
      else
        result = unsignedOperation ? a.urem(b) : a.srem(b);
      break;
    }
    case TokenKind::AMPERSAND:
      result = a & b;
      break;
    case TokenKind::PIPE:
      result = a | b;
      break;
    case TokenKind::CARET:
      result = a ^ b;
      break;
    case TokenKind::LEFT_SHIFT:
    case TokenKind::RIGHT_SHIFT: {
      // Shifting by the width or more has no defined result at run time.
      if (b.uge(a.getBitWidth()))
        return refuse("shifts an integer by its width or more");
      unsigned amount = static_cast<unsigned>(b.getZExtValue());
      if (op == TokenKind::LEFT_SHIFT)
        result = a.shl(amount);
      else
        result = unsignedOperation ? a.lshr(amount) : a.ashr(amount);
      break;
    }
    default:
      return refuse("uses an operator the compiler cannot evaluate");
  }

  // The result is typed the way analysis typed the expression. If that type
  // is not held in the width generated code computes in, decline rather than
  // guess which of the two is right.
  TypePtr type = resultType ? resultType : left.type;
  auto width = type ? getIntegerBitWidth(*type) : std::nullopt;
  if (!width || *width != result.getBitWidth())
    return refuse("produces a value whose width the compiler cannot confirm");
  return makeInteger(type, result);
}

std::optional<ConstantValue> ConstantEvaluator::evaluateTernary(
    const sun::ast::TernaryExprAST& ternary) {
  auto condition = evaluateExpression(*ternary.getCond());
  if (!condition) return std::nullopt;
  auto truth = readTruthValue(*condition);
  if (!truth) {
    recordBlocker(
        "its initializer chooses on a value that is not a truth "
        "value",
        ternary.getLocation());
    return std::nullopt;
  }
  auto chosen =
      evaluateExpression(*truth ? *ternary.getThen() : *ternary.getElse());
  if (!chosen) return std::nullopt;
  // Both sides share the expression's type; widen the chosen one to it.
  TypePtr type = getValueType(ternary);
  return type ? convertToDeclaredType(std::move(*chosen), type,
                                      ternary.getLocation())
              : chosen;
}

std::optional<ConstantValue> ConstantEvaluator::evaluateArrayLiteral(
    const sun::ast::ArrayLiteralAST& literal) {
  TypePtr type = getValueType(literal);
  if (!type || !type->isArray()) {
    recordBlocker("its initializer is an array the compiler has no type for",
                  literal.getLocation());
    return std::nullopt;
  }
  const auto& arrayType = static_cast<const sun::types::ArrayType&>(*type);
  // An element is itself an array when the literal is nested; otherwise it
  // is widened to the array's element type, as each element is at run time.
  TypePtr elementType = arrayType.getDimensions().size() > 1
                            ? nullptr
                            : arrayType.getElementType();

  ConstantValue::Elements elements;
  elements.reserve(literal.getElements().size());
  for (const auto& element : literal.getElements()) {
    auto value = evaluateExpression(*element);
    if (value && elementType)
      value = convertToDeclaredType(std::move(*value), elementType,
                                    element->getLocation());
    if (!value) return std::nullopt;
    elements.push_back(std::move(*value));
  }
  return ConstantValue{type, std::move(elements)};
}

std::optional<ConstantValue> ConstantEvaluator::evaluateConvert(
    const sun::ast::GenericCallAST& call) {
  auto refuse = [&](const std::string& what) -> std::optional<ConstantValue> {
    recordBlocker("its initializer " + what, call.getLocation());
    return std::nullopt;
  };
  if (call.getArgs().size() != 1 || call.getResolvedTypeArgs().size() != 1)
    return refuse("calls '_convert' in a form the compiler cannot evaluate");

  auto source = evaluateExpression(*call.getArgs()[0]);
  if (!source) return std::nullopt;
  TypePtr target = sun::types::unwrapRef(call.getResolvedTypeArgs()[0]);
  if (!target) return refuse("converts to an unknown type");

  // An enum converts as the integer it is stored as. Only the integer types
  // sign-extend; a bool or a char is never negative and zero-extends.
  TypePtr sourceType = source->type;
  if (sourceType && sourceType->isEnum())
    sourceType = static_cast<const sun::types::EnumType&>(*sourceType)
                     .getUnderlyingType();
  const bool sourceSigned =
      sourceType && sourceType->isIntegral() && !sourceType->isUnsigned();
  const bool targetSigned = !target->isUnsigned();
  auto targetWidth = getIntegerBitWidth(*target);

  if (source->isInteger() && targetWidth) {
    const APInt& bits = source->getInteger();
    APInt converted = sourceSigned ? bits.sextOrTrunc(*targetWidth)
                                   : bits.zextOrTrunc(*targetWidth);
    return makeInteger(target, converted);
  }
  if (source->isInteger() && target->isFloatingPoint()) {
    APFloat converted(target->isFloat32() ? APFloat::IEEEsingle()
                                          : APFloat::IEEEdouble());
    converted.convertFromAPInt(source->getInteger(), sourceSigned,
                               APFloat::rmNearestTiesToEven);
    return ConstantValue{target, converted};
  }
  if (source->isFloat() && targetWidth) {
    // Rounds toward zero. A value the target cannot hold has no defined
    // result at run time.
    llvm::APSInt converted(*targetWidth, /*isUnsigned=*/!targetSigned);
    bool isExact = false;
    if (source->getFloat().convertToInteger(converted, APFloat::rmTowardZero,
                                            &isExact) &
        APFloat::opInvalidOp)
      return refuse("converts a float that the integer type cannot hold");
    return makeInteger(target, converted);
  }
  if (source->isFloat() && target->isFloatingPoint()) {
    APFloat converted = source->getFloat();
    bool losesInfo = false;
    converted.convert(
        target->isFloat32() ? APFloat::IEEEsingle() : APFloat::IEEEdouble(),
        APFloat::rmNearestTiesToEven, &losesInfo);
    return ConstantValue{target, converted};
  }
  return refuse("uses a conversion the compiler cannot evaluate");
}

std::optional<ConstantValue> ConstantEvaluator::evaluateIndex(
    const ExprAST& array, const std::vector<const ExprAST*>& indices,
    const Position& position) {
  auto current = evaluateExpression(array);
  if (!current) return std::nullopt;
  for (const ExprAST* indexExpr : indices) {
    auto index = evaluateExpression(*indexExpr);
    if (!index) return std::nullopt;
    if (!current->isArray() || !index->isInteger()) {
      recordBlocker("its initializer indexes something other than an array",
                    position);
      return std::nullopt;
    }
    // Indexing is not checked at run time, so an index outside the array has
    // no defined result to reproduce.
    const APInt& bits = index->getInteger();
    const auto& elements = current->getElements();
    const bool negative = !index->isUnsigned() && bits.isNegative();
    if (negative || bits.getActiveBits() > 64 ||
        bits.getZExtValue() >= elements.size()) {
      recordBlocker("its initializer indexes outside an array",
                    indexExpr->getLocation());
      return std::nullopt;
    }
    ConstantValue element = elements[bits.getZExtValue()];
    current = std::move(element);
  }
  return current;
}

// -------------------------------------------------------------------
// Function calls
// -------------------------------------------------------------------

bool ConstantEvaluator::countStep(const Position& position) {
  if (++steps_ <= kMaxEvaluationSteps) return true;
  // Said once per initializer: a function that may never finish is worth
  // knowing about even though the program still compiles.
  if (steps_ == kMaxEvaluationSteps + 1 && !deciding_.empty())
    sun::support::logWarning(
        "'" + deciding_.front()->getName() +
            "' is initialized at startup: evaluating its initializer at "
            "compile time was given up after too many steps",
        position);
  recordBlocker(
      "its initializer did not finish within the number of steps "
      "the compiler evaluates at compile time",
      position);
  return false;
}

const sun::ast::FunctionAST* ConstantEvaluator::findEvaluableCallee(
    const sun::ast::CallExprAST& call) {
  auto refuse = [&](const std::string& what) -> const sun::ast::FunctionAST* {
    recordBlocker("its initializer " + what, call.getLocation());
    return nullptr;
  };
  if (auto type = getValueType(call); type && type->isClass())
    return refuse("constructs a '" + type->toDisplayString() + "' value");

  DeclarationId target = call.getTargetDeclarationId();
  const ExprAST* node = target ? declarations_.get(target).astNode : nullptr;
  if (!node || node->getType() != ASTNodeType::FUNCTION ||
      declarations_.get(target).kind != DeclarationKind::Function)
    return refuse("calls something other than a plain function");

  const auto& function = static_cast<const sun::ast::FunctionAST&>(*node);
  const auto& proto = function.getProto();
  const std::string name = "'" + proto.getName() + "'";
  if (function.isExtern())
    return refuse("calls " + name + ", which is defined outside Sun");
  if (function.isPrecompiled())
    return refuse("calls " + name +
                  ", which is defined in a precompiled library");
  if (proto.isGeneric() || proto.isTemplate() || proto.hasVariadicParam())
    return refuse("calls " + name + ", which is generic");
  if (proto.canThrow()) return refuse("calls " + name + ", which can throw");
  // Analysis gives a function its type once its body has been checked
  if (!function.getResolvedType())
    return refuse("calls " + name +
                  ", which has not been analyzed at this point; declare " +
                  name + " above the first use of the value");
  for (const auto& paramType : proto.getResolvedParamTypes())
    if (!paramType || paramType->isReference())
      return refuse("calls " + name + ", which takes a reference");
  if (call.getArgs().size() != proto.getResolvedParamTypes().size() ||
      proto.declarationIdentity().parameters.size() != call.getArgs().size())
    return refuse("calls " + name + " in a form the compiler cannot evaluate");
  return &function;
}

std::optional<ConstantValue> ConstantEvaluator::evaluateCall(
    const sun::ast::CallExprAST& call) {
  const sun::ast::FunctionAST* function = findEvaluableCallee(call);
  if (!function) return std::nullopt;
  const auto& proto = function->getProto();

  if (frames_.size() >= kMaxCallDepth) {
    recordBlocker(
        "its initializer calls functions nested more deeply than "
        "the compiler evaluates at compile time",
        call.getLocation());
    return std::nullopt;
  }

  // Arguments are evaluated in the caller, then become the callee's
  // parameters, converted as a call converts them.
  Frame frame;
  const auto& paramTypes = proto.getResolvedParamTypes();
  const auto& paramIds = proto.declarationIdentity().parameters;
  for (size_t i = 0; i < call.getArgs().size(); ++i) {
    auto argument = evaluateExpression(*call.getArgs()[i]);
    if (argument)
      argument = convertToDeclaredType(std::move(*argument), paramTypes[i],
                                       call.getArgs()[i]->getLocation());
    if (!argument) return std::nullopt;
    frame.emplace(paramIds[i], std::move(*argument));
  }

  frames_.push_back(std::move(frame));
  returned_.reset();
  Flow flow = runBlock(function->getBody());
  frames_.pop_back();

  std::optional<ConstantValue> result = std::move(returned_);
  returned_.reset();
  if (flow == Flow::Failed) return std::nullopt;
  if (flow != Flow::Return || !result) {
    recordBlocker("its initializer calls '" + proto.getName() +
                      "', which returns no value",
                  call.getLocation());
    return std::nullopt;
  }
  return convertToDeclaredType(std::move(*result),
                               sun::types::unwrapRef(getValueType(call)),
                               call.getLocation());
}

std::optional<bool> ConstantEvaluator::evaluateCondition(
    const ExprAST& condition) {
  auto value = evaluateExpression(condition);
  if (!value) return std::nullopt;
  auto truth = readTruthValue(*value);
  if (!truth)
    recordBlocker("its initializer tests a value that is not true or false",
                  condition.getLocation());
  return truth;
}

bool ConstantEvaluator::assignLocal(DeclarationId target,
                                    const std::string& name,
                                    ConstantValue value,
                                    const Position& position) {
  auto local =
      frames_.empty() ? Frame::iterator{} : frames_.back().find(target);
  if (frames_.empty() || local == frames_.back().end()) {
    recordBlocker("its initializer calls a function that writes to '" + name +
                      "', which is not one of its own variables",
                  position);
    return false;
  }
  auto converted =
      convertToDeclaredType(std::move(value), local->second.type, position);
  if (!converted) return false;
  local->second = std::move(*converted);
  return true;
}

ConstantEvaluator::Flow ConstantEvaluator::runBlock(
    const sun::ast::BlockExprAST& block) {
  for (const auto& statement : block.getBody()) {
    Flow flow = runStatement(*statement);
    if (flow != Flow::Next) return flow;
  }
  return Flow::Next;
}

ConstantEvaluator::Flow ConstantEvaluator::runLoop(const ExprAST* condition,
                                                   const ExprAST* increment,
                                                   const ExprAST& body,
                                                   const Position& position) {
  while (true) {
    if (!countStep(position)) return Flow::Failed;
    if (condition) {
      auto proceed = evaluateCondition(*condition);
      if (!proceed) return Flow::Failed;
      if (!*proceed) return Flow::Next;
    }
    Flow flow = runStatement(body);
    if (flow == Flow::Return || flow == Flow::Failed) return flow;
    if (flow == Flow::Break) return Flow::Next;
    if (increment && runStatement(*increment) == Flow::Failed)
      return Flow::Failed;
  }
}

ConstantEvaluator::Flow ConstantEvaluator::runStatement(
    const ExprAST& statement) {
  if (!countStep(statement.getLocation())) return Flow::Failed;

  switch (statement.getType()) {
    case ASTNodeType::BLOCK:
      return runBlock(static_cast<const sun::ast::BlockExprAST&>(statement));

    case ASTNodeType::VARIABLE_CREATION: {
      const auto& local =
          static_cast<const sun::ast::VariableCreationAST&>(statement);
      if (!local.getValue() || !local.getResolvedType() ||
          local.getResolvedType()->isReference()) {
        recordBlocker(
            "its initializer calls a function with a variable the "
            "compiler cannot evaluate",
            statement.getLocation());
        return Flow::Failed;
      }
      auto value = evaluateExpression(*local.getValue());
      if (value)
        value =
            convertToDeclaredType(std::move(*value), local.getResolvedType(),
                                  local.getValue()->getLocation());
      if (!value) return Flow::Failed;
      frames_.back()[local.getDeclarationId()] = std::move(*value);
      return Flow::Next;
    }

    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      const auto& assignment =
          static_cast<const sun::ast::VariableAssignmentAST&>(statement);
      auto value = evaluateExpression(*assignment.getValue());
      if (!value) return Flow::Failed;
      return assignLocal(assignment.getTargetDeclarationId(),
                         assignment.getName(), std::move(*value),
                         statement.getLocation())
                 ? Flow::Next
                 : Flow::Failed;
    }

    case ASTNodeType::COMPOUND_ASSIGNMENT: {
      const auto& compound =
          static_cast<const sun::ast::CompoundAssignmentAST&>(statement);
      auto op = sun::parsing::compoundToBinaryOp(compound.getOp().kind);
      if (!op ||
          compound.getTarget()->getType() != ASTNodeType::VARIABLE_REFERENCE) {
        recordBlocker(
            "its initializer calls a function that updates "
            "something other than a variable of its own",
            statement.getLocation());
        return Flow::Failed;
      }
      const auto& target = static_cast<const sun::ast::VariableReferenceAST&>(
          *compound.getTarget());
      auto current = evaluateExpression(target);
      if (!current) return Flow::Failed;
      auto operand = evaluateExpression(*compound.getValue());
      if (!operand) return Flow::Failed;
      TypePtr variableType = current->type;
      auto combined =
          combineOperands(*op, std::move(*current), std::move(*operand),
                          variableType, variableType, statement.getLocation());
      if (!combined) return Flow::Failed;
      return assignLocal(target.getTargetDeclarationId(), target.getName(),
                         std::move(*combined), statement.getLocation())
                 ? Flow::Next
                 : Flow::Failed;
    }

    case ASTNodeType::RETURN: {
      const auto& returnExpr =
          static_cast<const sun::ast::ReturnExprAST&>(statement);
      returned_.reset();
      if (returnExpr.hasValue()) {
        returned_ = evaluateExpression(*returnExpr.getValue());
        if (!returned_) return Flow::Failed;
      }
      return Flow::Return;
    }

    case ASTNodeType::IF: {
      const auto& branch = static_cast<const sun::ast::IfExprAST&>(statement);
      auto taken = evaluateCondition(*branch.getCond());
      if (!taken) return Flow::Failed;
      if (*taken) return runStatement(*branch.getThen());
      return branch.getElse() ? runStatement(*branch.getElse()) : Flow::Next;
    }

    case ASTNodeType::WHILE_LOOP: {
      const auto& loop = static_cast<const sun::ast::WhileExprAST&>(statement);
      return runLoop(loop.getCondition(), nullptr, *loop.getBody(),
                     statement.getLocation());
    }

    case ASTNodeType::FOR_LOOP: {
      const auto& loop = static_cast<const sun::ast::ForExprAST&>(statement);
      if (loop.getInit() && runStatement(*loop.getInit()) == Flow::Failed)
        return Flow::Failed;
      return runLoop(loop.getCondition(), loop.getIncrement(), *loop.getBody(),
                     statement.getLocation());
    }

    case ASTNodeType::BREAK_STMT:
      return Flow::Break;
    case ASTNodeType::CONTINUE_STMT:
      return Flow::Continue;

    default:
      // Any other statement is an expression whose value is dropped
      return evaluateExpression(statement) ? Flow::Next : Flow::Failed;
  }
}

std::optional<ConstantValue> ConstantEvaluator::convertToDeclaredType(
    ConstantValue value, const TypePtr& declaredType,
    const Position& position) {
  if (!declaredType) return value;

  if (value.isArray()) {
    // Elements were already brought to the element type; the array as a whole
    // takes the declared type, which analysis checked it fits.
    value.type = declaredType;
    return value;
  }
  if (value.isString()) {
    value.type = declaredType;
    return value;
  }

  if (value.isInteger()) {
    // A narrower integer widens by its own signedness; a wider one keeps its
    // low bits.
    if (auto width = getIntegerBitWidth(*declaredType)) {
      const APInt& bits = value.getInteger();
      APInt converted = bits.getBitWidth() > *width
                            ? bits.trunc(*width)
                            : widenInteger(value, *width);
      return makeInteger(declaredType, converted);
    }
  } else if (value.isFloat() && declaredType->isFloatingPoint()) {
    // Between the two float widths, rounding to the nearest value.
    APFloat converted = value.getFloat();
    bool losesInfo = false;
    converted.convert(declaredType->isFloat32() ? APFloat::IEEEsingle()
                                                : APFloat::IEEEdouble(),
                      APFloat::rmNearestTiesToEven, &losesInfo);
    return ConstantValue{declaredType, converted};
  }
  recordBlocker(
      "its initializer needs a conversion the compiler does not "
      "perform at compile time",
      position);
  return std::nullopt;
}

}  // namespace sun::semantic_analysis::constants
