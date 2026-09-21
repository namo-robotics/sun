// constant_evaluator.h — Computes file-scope initializers at compile time.
//
// Runs on a program that has already been analyzed, so every expression has
// its type and every name its declaration. For each file-scope variable it
// either computes the initializer's value, which then goes into the program
// image, or records the first thing that stood in the way, in which case the
// variable is initialized by the startup function instead.
//
// A computed value must equal what the same expression produces at run time,
// so the arithmetic here follows generated code operation for operation:
// which operand decides signedness, how mixed widths are widened, how floats
// compare. Anything generated code leaves undefined, such as dividing an
// integer by zero, is refused rather than given a value.

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "semantic_analysis/constants/global_init.h"
#include "semantic_analysis/declaration_table.h"

/** Evaluates constant expressions while a program is being analyzed. */
namespace sun::semantic_analysis::constants {

/** Evaluates the initializers of a program's file-scope variables. */
/**
 * Most statements and loop turns evaluated for one initializer. A function
 * that never finishes must not hang the compiler; past the limit the global
 * is initialized at startup.
 */
constexpr size_t kMaxEvaluationSteps = 1'000'000;

/** Most function calls in progress at once while evaluating an initializer. */
constexpr size_t kMaxCallDepth = 200;

class ConstantEvaluator {
  // Finds the node that declares a variable an initializer reads.
  const DeclarationTable& declarations_;
  // The first obstacle met while evaluating the current initializer.
  std::optional<StartupReason> blocker_;
  /** The parameters and local variables of one function being evaluated. */
  using Frame = std::unordered_map<DeclarationId, ConstantValue>;
  // One frame per function call in progress, innermost last.
  std::vector<Frame> frames_;
  // The value handed back by the `return` that is ending the current call.
  std::optional<ConstantValue> returned_;
  // Statements and loop turns evaluated for the initializer being decided. A
  // function that never finishes must not hang the compiler.
  size_t steps_ = 0;

  /** What running a statement asks the code around it to do next. */
  enum class Flow {
    Next,      // carry on with the following statement
    Return,    // leave the function; the value is in returned_
    Break,     // leave the innermost loop
    Continue,  // start the innermost loop's next turn
    Failed,    // could not be evaluated; the obstacle has been recorded
  };

  // The globals being decided, outermost first: deciding one may mean
  // deciding a global declared further down that it reads.
  std::vector<const sun::ast::VariableCreationAST*> deciding_;

 public:
  /**
   * Decisions are stored on the declaring nodes; `declarations` is used to
   * reach the node of a variable that another initializer reads, and must
   * outlive the evaluator.
   */
  explicit ConstantEvaluator(const DeclarationTable& declarations)
      : declarations_(declarations) {}

  /**
   * Decides every file-scope variable in the block, in source order, looking
   * inside modules and bundle scopes. A variable that already has a decision
   * is left alone. Then checks the order of the variables left to startup
   * (see checkStartupOrder).
   */
  void evaluateGlobals(const sun::ast::BlockExprAST& block);

  /**
   * Decides one file-scope variable and stores the decision on its node.
   * Returns the stored decision.
   */
  const GlobalInitRecord& evaluateGlobalInitializer(
      const sun::ast::VariableCreationAST& global);

 private:
  /** Decides the variables of one block and of the modules inside it. */
  void decideGlobals(const sun::ast::BlockExprAST& block);

  /**
   * Startup initializers run in source order, so one that directly reads a
   * variable initialized later would see it still zeroed. Reports that as an
   * error. A read hidden inside a called function is not detected.
   */
  void checkStartupOrder(const sun::ast::BlockExprAST& block);
  /** Lists the block's variables in the order startup initializes them. */
  void collectInitializationOrder(
      const sun::ast::BlockExprAST& block,
      std::vector<const sun::ast::VariableCreationAST*>& order) const;
  /** The file-scope variable a name or `module.name` refers to, or null. */
  const sun::ast::VariableCreationAST* findGlobalNode(
      DeclarationId target) const;

  /**
   * The value of an analyzed expression, or nothing when it cannot be
   * computed; the obstacle is then recorded with recordBlocker.
   */
  std::optional<ConstantValue> evaluateExpression(
      const sun::ast::ExprAST& expr);

  /** Evaluates a number, bool, char or string literal. */
  std::optional<ConstantValue> evaluateLiteral(const sun::ast::ExprAST& expr);
  /** Reads the compile-time value of the `const` a name refers to. */
  std::optional<ConstantValue> evaluateGlobalRead(
      DeclarationId target, const std::string& name,
      const sun::support::Position& position);
  /** Evaluates `m.X` on a module and `Color.Red` on a payload-free enum. */
  std::optional<ConstantValue> evaluateMemberAccess(
      const sun::ast::MemberAccessAST& access);
  /** Evaluates an arithmetic, comparison, bitwise or logical operator. */
  std::optional<ConstantValue> evaluateBinary(
      const sun::ast::BinaryExprAST& binary);
  /** Evaluates `and` and `or`, which skip the right side when decided. */
  std::optional<ConstantValue> evaluateLogical(
      const sun::ast::BinaryExprAST& binary);
  /** Evaluates negation, `not` and bitwise complement. */
  std::optional<ConstantValue> evaluateUnary(
      const sun::ast::UnaryExprAST& unary);
  /** Evaluates `cond ? a : b`, computing only the chosen side. */
  std::optional<ConstantValue> evaluateTernary(
      const sun::ast::TernaryExprAST& ternary);
  /** Evaluates an array literal, element by element. */
  std::optional<ConstantValue> evaluateArrayLiteral(
      const sun::ast::ArrayLiteralAST& literal);
  /** Evaluates the numeric conversion `_convert<T>(value)`. */
  std::optional<ConstantValue> evaluateConvert(
      const sun::ast::GenericCallAST& call);
  /** Reads one element of an array value: `a[i]` or `a[i, j]`. */
  std::optional<ConstantValue> evaluateIndex(
      const sun::ast::ExprAST& array,
      const std::vector<const sun::ast::ExprAST*>& indices,
      const sun::support::Position& position);

  /**
   * Evaluates a call to a plain function by running its analyzed body. The
   * function must be pure as far as the evaluator can see: it has a body in
   * this program, takes and returns values, cannot throw, and touches nothing
   * but its own variables and compile-time constants. Anything else is
   * refused, and the global is initialized at startup instead.
   */
  std::optional<ConstantValue> evaluateCall(const sun::ast::CallExprAST& call);
  /**
   * The function a call runs, when it is one the evaluator can run; otherwise
   * records why not and returns null.
   */
  const sun::ast::FunctionAST* findEvaluableCallee(
      const sun::ast::CallExprAST& call);
  /** Runs one statement of a function body in the innermost frame. */
  Flow runStatement(const sun::ast::ExprAST& statement);
  /** Runs the statements of a block in order until one changes the flow. */
  Flow runBlock(const sun::ast::BlockExprAST& block);
  /** Runs a `while` loop or a `for (init; condition; step)` loop. */
  Flow runLoop(const sun::ast::ExprAST* condition,
               const sun::ast::ExprAST* increment,
               const sun::ast::ExprAST& body,
               const sun::support::Position& position);
  /** Evaluates a condition to true or false, or nothing when it cannot. */
  std::optional<bool> evaluateCondition(const sun::ast::ExprAST& condition);
  /**
   * Stores a value in a local variable of the innermost frame, converted to
   * the variable's type. Refuses a name that is not such a variable, which is
   * how a write to a global is kept out.
   */
  bool assignLocal(DeclarationId target, const std::string& name,
                   ConstantValue value, const sun::support::Position& position);
  /**
   * Counts one unit of work and reports whether evaluation may continue. Past
   * the limit the initializer is left to startup.
   */
  bool countStep(const sun::support::Position& position);

  /**
   * Brings two operands to one width the way generated code does, then
   * applies the operator. `leftType` decides the signedness of the operation.
   */
  std::optional<ConstantValue> combineOperands(
      sun::parsing::TokenKind op, ConstantValue left, ConstantValue right,
      const sun::types::TypePtr& leftType,
      const sun::types::TypePtr& resultType,
      const sun::support::Position& position);

  /**
   * Applies an arithmetic, bitwise, shift or comparison operator to two
   * operands of one width, the way generated code does after widening them.
   * `unsignedOperation` is the signedness of the left operand's type.
   */
  std::optional<ConstantValue> applyBinaryOperator(
      sun::parsing::TokenKind op, const ConstantValue& left,
      const ConstantValue& right, bool unsignedOperation,
      const sun::types::TypePtr& resultType,
      const sun::support::Position& position);

  /**
   * Brings a value to the type it is stored as, the way generated code
   * converts an initializer: a narrower integer widens by its own signedness
   * and a wider one keeps its low bits, and a float is rounded to the other
   * float width. Any other difference in type is refused.
   */
  std::optional<ConstantValue> convertToDeclaredType(
      ConstantValue value, const sun::types::TypePtr& declaredType,
      const sun::support::Position& position);

  /**
   * Notes why the current initializer cannot be evaluated. Only the first
   * obstacle is kept, since that is the one a diagnostic should name.
   */
  void recordBlocker(std::string message,
                     const sun::support::Position& position);
};

}  // namespace sun::semantic_analysis::constants
