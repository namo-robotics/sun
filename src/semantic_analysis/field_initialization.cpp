// field_initialization.cpp — What is initialized where inside a constructor
//
// The walk carries one view of the object: where each field stands at this
// point in the body.
//
//   Uninitialized — no path reaching here has assigned it
//   Initialized   — every path reaching here has assigned it
//   Unknown       — some paths have and some have not
//
// A choice — the arms of an `if`, a ternary, a `match`, a `try` and its
// `catch` clauses — walks each alternative from the state before the choice
// and merges the ends: where the alternatives agree the answer stands, where
// they disagree it becomes Unknown. A loop is walked until its state settles,
// so a write in the body sees what a second pass would see as well as a first.
//
// Three things are errors: reading a field, or the object as a whole, before
// it is Initialized; writing a field that is Unknown, because the write can
// neither start the field's life nor drop what it replaces; and reaching the
// end of a path with a field still not Initialized.
//
// A constructor may hand work to its own methods, including giving fields
// their first values. The walk follows the call into the method's body with
// the state as it stands at the call, so reads in there are checked against
// what is actually assigned, and what the method assigns counts for the
// constructor. A method's write always replaces and drops, whoever calls it:
// before a field's first value the storage is all zero, the state an owning
// deinit must treat as nothing to release, so the drop is a no-op there.
// Lambdas are not walked at all — they run later, not here, and making one
// that touches `this` already requires the whole object.

#include "semantic_analysis/field_initialization.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "ast/ast_children.h"
#include "ast/control_flow.h"
#include "support/error.h"

namespace sun {

void prepareFieldInitializers(ClassDefinitionAST& classDef) {
  if (classDef.isPartial() || classDef.isPrecompiled()) return;
  size_t count = 0;
  for (const auto& field : classDef.getFields()) {
    if (field.initializer) ++count;
  }
  if (!count) return;

  if (!classDef.getConstructor()) {
    if (count != classDef.getFields().size()) {
      logAndThrowError(
          "Class '" + classDef.getName() +
              "' needs an explicit init to initialize fields without defaults",
          classDef.getLocation());
    }
    auto proto = std::make_unique<PrototypeAST>(
        "init", std::vector<std::pair<std::string, TypeAnnotation>>{});
    proto->setLocation(classDef.getLocation());
    auto body = std::make_unique<BlockExprAST>();
    body->setKind(BlockKind::Function);
    body->setLocation(classDef.getLocation());
    auto function =
        std::make_unique<FunctionAST>(std::move(proto), std::move(body));
    function->setVisibility(sun::Visibility::Public);
    function->setLocation(classDef.getLocation());
    function->setSynthesizedConstructor(true);
    function->inheritSourceFile(classDef.getSourceFileId());
    classDef.getMutableMethods().push_back({std::move(function), true});
  }

  for (auto& method : classDef.getMutableMethods()) {
    auto& function = *method.function;
    if (!method.isConstructor || !function.hasBody() ||
        function.getFieldInitializerCount())
      continue;
    std::vector<std::unique_ptr<ExprAST>> prefix;
    for (const auto& field : classDef.getFields()) {
      if (!field.initializer) continue;
      auto receiver = std::make_unique<ThisExprAST>();
      receiver->setLocation(field.location);
      auto assignment = std::make_unique<MemberAssignmentAST>(
          std::move(receiver), field.name, field.initializer->clone());
      assignment->setLocation(field.initializer->getLocation());
      assignment->inheritSourceFile(classDef.getSourceFileId());
      prefix.push_back(std::move(assignment));
    }
    const_cast<BlockExprAST&>(function.getBody())
        .prependExpressions(std::move(prefix));
    function.setFieldInitializerCount(count);
  }
}

namespace {

/**
 * Is this expression the constructor's own receiver?
 */
bool isThis(const ExprAST* expr) {
  return expr && expr->getType() == ASTNodeType::THIS;
}

/**
 * Does `this` appear anywhere inside this expression?
 */
bool usesThis(const ExprAST& expr) {
  if (expr.getType() == ASTNodeType::THIS) return true;
  bool found = false;
  forEachChild(expr, [&found](const ExprAST& child) {
    if (!found) found = usesThis(child);
  });
  return found;
}

/**
 * Where a field stands at a point in the walk.
 */
enum class FieldStatus { Uninitialized, Initialized, Unknown };

using FieldStates = std::map<std::string, FieldStatus>;

/**
 * Folds the state at the end of one alternative into the state that follows a
 * choice: where the two agree the answer stands, where they disagree nothing
 * is known any more.
 */
void mergeInto(FieldStates& target, const FieldStates& other) {
  for (auto& [name, status] : target) {
    auto found = other.find(name);
    if (found == other.end() || found->second != status) {
      status = FieldStatus::Unknown;
    }
  }
}

/**
 * The fields and methods of one class.
 */
class ClassInitInfo {
 public:
  ClassInitInfo(const ClassType& classType,
                const std::vector<ClassMethodDecl>& methods)
      : classType_(classType), methods_(methods) {
    for (const auto& field : classType.getFields()) {
      allFields_.insert(field.name);
      if (typeNeedsDrop(field.type)) owningFields_.insert(field.name);
    }
  }

  const ClassType& classType() const { return classType_; }
  const std::set<std::string>& allFields() const { return allFields_; }
  bool isField(const std::string& name) const {
    return allFields_.count(name) != 0;
  }

  // True when the field holds something that has to be released, so writing
  // it again would have to drop what was there. A field holding a number
  // releases nothing, so it is never in doubt.
  bool fieldOwns(const std::string& name) const {
    return owningFields_.count(name) != 0;
  }

  /**
   * The body of one of the class's own methods, or null when the class does
   * not declare it or carries only its signature, as a precompiled bundle
   * does.
   */
  const FunctionAST* methodBody(const std::string& name) const {
    for (const auto& method : methods_) {
      if (method.function && method.function->getProto().getName() == name &&
          method.function->hasBody()) {
        return method.function.get();
      }
    }
    return nullptr;
  }

 private:
  const ClassType& classType_;
  const std::vector<ClassMethodDecl>& methods_;
  std::set<std::string> allFields_;
  std::set<std::string> owningFields_;
};

/**
 * Walks a constructor body in source order, tagging each write to a field and
 * reporting what the body may not do.
 */
class BodyWalk {
 public:
  explicit BodyWalk(ClassInitInfo& info) : info_(info) {
    for (const auto& name : info.allFields()) {
      states_[name] = FieldStatus::Uninitialized;
    }
  }

  /**
   * Walks one expression and everything under it.
   */
  void walk(const ExprAST& expr);

  /**
   * Reports the fields still without a value where a constructor body ends.
   * Nothing is reported when control cannot get there.
   */
  void requireEveryFieldAtEnd(const ExprAST& body, std::optional<Position> loc);

 private:
  ClassInitInfo& info_;
  // Where every field stands at the current point
  FieldStates states_;
  // Methods being walked further up the call chain, so a cycle is not
  // followed round again
  std::set<std::string> inProgress_;
  // True while the walk is inside the body of a method the constructor
  // called. Such a body also runs after construction, on whole objects, so
  // its writes must mean the same thing there as here.
  bool inMethodBody_ = false;

  std::string className() const { return info_.classType().getDisplayName(); }

  FieldStatus statusOf(const std::string& field) const {
    auto found = states_.find(field);
    return found == states_.end() ? FieldStatus::Unknown : found->second;
  }

  /**
   * The name of a field that does not certainly hold a value, or an empty
   * string once every field does.
   */
  std::string firstFieldWithoutValue() const;

  /**
   * Records that the body reaches a field's value here, which is an error
   * unless the field certainly has one.
   */
  void noteFieldRead(const std::string& field, const std::string& what,
                     std::optional<Position> loc);

  /**
   * Records that the body reaches the whole object here, which needs every
   * field to hold a value.
   */
  void noteObjectUse(const std::string& what, std::optional<Position> loc);

  /**
   * Tags one write to a field and moves the field to Initialized.
   */
  void walkFieldWrite(const MemberAssignmentAST& assign,
                      const std::string& field);

  /**
   * Handles `this.method(...)` by walking the method's own body from here.
   */
  void walkCallOnThis(const CallExprAST& call, const std::string& methodName);

  /**
   * Reports a write to a field that only some of the paths reaching it have
   * assigned, so whether it holds a value is not known.
   */
  void rejectUncertainAssignment(const std::string& field,
                                 std::optional<Position> loc);

  void walkChildren(const ExprAST& expr);

  /**
   * Walks the alternatives of a choice — the arms of an `if`, a ternary or a
   * `match`. Each starts from the state before the choice. A null entry
   * stands for a path that runs nothing, as an `if` without an `else` has.
   */
  void walkBranches(const std::vector<const ExprAST*>& branches);

  /**
   * Walks a `try` and its `catch` clauses. A throw can leave the try block
   * part-way through, so a clause finds whatever it had assigned by then.
   */
  void walkTryCatch(const TryCatchExprAST& tryCatch);

  /**
   * Walks a loop: `once` runs before the first test, `body` and `then` may
   * run any number of times, including none.
   */
  void walkLoop(const std::vector<const ExprAST*>& once, const ExprAST* body,
                const ExprAST* then);

  /**
   * Gives up on every field written anywhere inside, for a `try` block that a
   * `catch` clause may pick up part-way through.
   */
  void forgetEveryWrittenField(const ExprAST& expr);
};

std::string BodyWalk::firstFieldWithoutValue() const {
  for (const auto& field : info_.classType().getFields()) {
    if (statusOf(field.name) != FieldStatus::Initialized) return field.name;
  }
  return "";
}

void BodyWalk::noteFieldRead(const std::string& field, const std::string& what,
                             std::optional<Position> loc) {
  if (statusOf(field) == FieldStatus::Initialized) return;
  logAndThrowError("Field '" + field + "' of '" + className() + "' is " + what +
                       " before it is assigned. A constructor assigns a field "
                       "before reading it back",
                   loc);
}

void BodyWalk::noteObjectUse(const std::string& what,
                             std::optional<Position> loc) {
  std::string missing = firstFieldWithoutValue();
  if (missing.empty()) return;
  logAndThrowError("Cannot " + what + " in the constructor of '" + className() +
                       "' while field '" + missing +
                       "' has no value yet. A constructor assigns every field, "
                       "on its own or through its own methods, before the "
                       "object may be read from or passed on",
                   loc);
}

void BodyWalk::rejectUncertainAssignment(const std::string& field,
                                         std::optional<Position> loc) {
  logAndThrowError(
      "Constructor of '" + className() + "' cannot tell whether field '" +
          field +
          "' already holds a value here: some paths reaching this write "
          "assigned it and some did not, so the write can neither start its "
          "life nor drop what it replaces. Assign the field on every path "
          "before this write, or on none of them: give each branch its own "
          "assignment, or settle the value in a local and assign it once",
      loc);
}

void BodyWalk::walkFieldWrite(const MemberAssignmentAST& assign,
                              const std::string& field) {
  // The value is produced before the store, so it is walked first
  walk(*assign.getValue());

  FieldStatus status = statusOf(field);

  if (!info_.fieldOwns(field)) {
    // Nothing to release either way, so the tag does not matter
    assign.setFieldWriteKind(FieldWriteKind::ReplacesValue);
  } else if (inMethodBody_) {
    // A method's write means one thing for every caller: it replaces what
    // the field holds and drops it. During construction, before the field's
    // first value, the storage is all zero — the state an owning deinit must
    // treat as nothing to release — so the same write is safe to come first,
    // and it discharges the constructor's obligation.
    assign.setFieldWriteKind(FieldWriteKind::ReplacesValue);
  } else if (status == FieldStatus::Unknown) {
    rejectUncertainAssignment(field, assign.getLocation());
  } else if (status == FieldStatus::Uninitialized) {
    assign.setFieldWriteKind(FieldWriteKind::StartsLife);
  } else {
    assign.setFieldWriteKind(FieldWriteKind::ReplacesValue);
  }

  states_[field] = FieldStatus::Initialized;
}

void BodyWalk::walkCallOnThis(const CallExprAST& call,
                              const std::string& methodName) {
  for (const auto& arg : call.getArgs()) {
    if (arg) walk(*arg);
  }

  // Once the object is whole this is an ordinary call on an ordinary method:
  // nothing left to check, and its writes replace values that are there
  if (firstFieldWithoutValue().empty()) return;

  // Not whole yet: follow the call and walk the method's body with the state
  // as it stands here. A body that is not here (a precompiled bundle carries
  // signatures only), or one already being walked further up the chain,
  // cannot be followed — it could reach anything, so it needs the whole
  // object.
  const FunctionAST* body = info_.methodBody(methodName);
  if (!body || !inProgress_.insert(methodName).second) {
    noteObjectUse("call method '" + methodName + "'", call.getLocation());
    return;
  }

  bool wasInMethod = inMethodBody_;
  inMethodBody_ = true;
  walk(body->getBody());
  inMethodBody_ = wasInMethod;
  inProgress_.erase(methodName);
}

void BodyWalk::requireEveryFieldAtEnd(const ExprAST& body,
                                      std::optional<Position> loc) {
  if (exprDiverges(body)) return;
  std::string missing = firstFieldWithoutValue();
  if (missing.empty()) return;
  logAndThrowError(
      "Constructor of '" + className() + "' can finish with field '" + missing +
          "' unassigned. Assign every field before the constructor ends: one "
          "left out would silently be zero",
      loc);
}

void BodyWalk::walkChildren(const ExprAST& expr) {
  forEachChild(expr, [this](const ExprAST& child) { walk(child); });
}

void BodyWalk::walkBranches(const std::vector<const ExprAST*>& branches) {
  FieldStates before = states_;
  std::optional<FieldStates> after;

  for (const ExprAST* branch : branches) {
    states_ = before;
    if (branch) {
      walk(*branch);
      // A branch that returns or throws never reaches the code after the
      // choice, so where it left the fields does not count
      if (exprDiverges(*branch)) continue;
    }
    if (after) {
      mergeInto(*after, states_);
    } else {
      after = states_;
    }
  }

  states_ = after ? std::move(*after) : std::move(before);
}

void BodyWalk::walkTryCatch(const TryCatchExprAST& tryCatch) {
  FieldStates before = states_;

  states_ = before;
  walk(tryCatch.getTryBlock());
  std::optional<FieldStates> after;
  if (!exprDiverges(tryCatch.getTryBlock())) after = states_;

  // A throw can leave the try block part-way through, so a clause picks up
  // with whatever the block had assigned by then
  states_ = before;
  forgetEveryWrittenField(tryCatch.getTryBlock());
  FieldStates atCatch = std::move(states_);

  for (const auto& clause : tryCatch.getCatchClauses()) {
    states_ = atCatch;
    if (clause.body) {
      walk(*clause.body);
      if (exprDiverges(*clause.body)) continue;
    }
    if (after) {
      mergeInto(*after, states_);
    } else {
      after = states_;
    }
  }

  states_ = after ? std::move(*after) : std::move(before);
}

void BodyWalk::walkLoop(const std::vector<const ExprAST*>& once,
                        const ExprAST* body, const ExprAST* then) {
  for (const ExprAST* expr : once) {
    if (expr) walk(*expr);
  }
  if (!body) return;

  // The body may run any number of times, including none, so it is walked
  // until the state at the top of the loop stops moving: the pass after the
  // first is what tells a write in the body that an earlier pass may already
  // have made it. Merging only ever gives up certainty, so this settles at
  // once for a field the loop cannot change and after one more round for the
  // rest.
  FieldStates atLoopTop = states_;
  for (int round = 0; round < 4; ++round) {
    states_ = atLoopTop;
    walk(*body);
    if (then) walk(*then);

    FieldStates settled = atLoopTop;
    mergeInto(settled, states_);
    if (settled == atLoopTop) break;
    atLoopTop = std::move(settled);
  }
  states_ = std::move(atLoopTop);
}

void BodyWalk::forgetEveryWrittenField(const ExprAST& expr) {
  if (expr.getType() == ASTNodeType::MEMBER_ASSIGNMENT) {
    const auto& assign = static_cast<const MemberAssignmentAST&>(expr);
    if (isThis(assign.getObject()) && info_.isField(assign.getMemberName())) {
      FieldStatus& status = states_[assign.getMemberName()];
      // A write can only ever give a field a value, so one that certainly has
      // one still does
      if (status == FieldStatus::Uninitialized) status = FieldStatus::Unknown;
    }
  }
  forEachChild(
      expr, [this](const ExprAST& child) { forgetEveryWrittenField(child); });
}

void BodyWalk::walk(const ExprAST& expr) {
  // Once every field holds a value the constructor is in its second phase,
  // and the rest of the body behaves like any method body: every read is
  // fine, and every write replaces and drops — the meaning a write carries
  // unless this walk says otherwise. Nothing can un-assign a field, so there
  // is nothing left to check or tag.
  if (firstFieldWithoutValue().empty()) return;

  switch (expr.getType()) {
    case ASTNodeType::MEMBER_ASSIGNMENT: {
      const auto& assign = static_cast<const MemberAssignmentAST&>(expr);
      const std::string& field = assign.getMemberName();
      if (isThis(assign.getObject()) && info_.isField(field)) {
        walkFieldWrite(assign, field);
        return;
      }
      break;
    }
    case ASTNodeType::MEMBER_ACCESS: {
      const auto& access = static_cast<const MemberAccessAST&>(expr);
      if (isThis(access.getObject())) {
        // A method in value position outlives the body it is taken in and
        // can reach every field, so it needs the whole object
        if (access.isBoundMethodRef()) {
          noteObjectUse(
              "take a reference to method '" + access.getMemberName() + "'",
              expr.getLocation());
        } else {
          noteFieldRead(access.getMemberName(), "read", expr.getLocation());
        }
        return;
      }
      break;
    }
    case ASTNodeType::CALL: {
      const auto& call = static_cast<const CallExprAST&>(expr);
      const ExprAST* callee = call.getCallee();
      if (callee && callee->getType() == ASTNodeType::MEMBER_ACCESS &&
          isThis(static_cast<const MemberAccessAST&>(*callee).getObject())) {
        walkCallOnThis(
            call, static_cast<const MemberAccessAST&>(*callee).getMemberName());
        return;
      }
      break;
    }
    case ASTNodeType::THIS:
      noteObjectUse("use 'this'", expr.getLocation());
      return;
    case ASTNodeType::RETURN: {
      const auto& ret = static_cast<const ReturnExprAST&>(expr);
      if (ret.getValue()) walk(*ret.getValue());
      // Only the constructor's own returns owe the whole object
      if (inMethodBody_) return;
      std::string missing = firstFieldWithoutValue();
      if (!missing.empty()) {
        logAndThrowError("Constructor of '" + className() +
                             "' returns with field '" + missing +
                             "' unassigned. Assign every field on every path "
                             "out of the constructor: one left out would "
                             "silently be zero",
                         expr.getLocation());
      }
      return;
    }
    case ASTNodeType::IF: {
      const auto& ifExpr = static_cast<const IfExprAST&>(expr);
      if (ifExpr.getCond()) walk(*ifExpr.getCond());
      walkBranches({ifExpr.getThen(), ifExpr.getElse()});
      return;
    }
    case ASTNodeType::TERNARY: {
      const auto& ternary = static_cast<const TernaryExprAST&>(expr);
      if (ternary.getCond()) walk(*ternary.getCond());
      walkBranches({ternary.getThen(), ternary.getElse()});
      return;
    }
    case ASTNodeType::MATCH: {
      const auto& match = static_cast<const MatchExprAST&>(expr);
      if (match.getDiscriminant()) walk(*match.getDiscriminant());
      std::vector<const ExprAST*> arms;
      for (const auto& arm : match.getArms()) arms.push_back(arm.body.get());
      walkBranches(arms);
      return;
    }
    case ASTNodeType::TRY_CATCH: {
      walkTryCatch(static_cast<const TryCatchExprAST&>(expr));
      return;
    }
    case ASTNodeType::WHILE_LOOP: {
      const auto& loop = static_cast<const WhileExprAST&>(expr);
      walkLoop({loop.getCondition()}, loop.getBody(), nullptr);
      return;
    }
    case ASTNodeType::FOR_LOOP: {
      const auto& loop = static_cast<const ForExprAST&>(expr);
      walkLoop({loop.getInit(), loop.getCondition()}, loop.getBody(),
               loop.getIncrement());
      return;
    }
    case ASTNodeType::FOR_IN_LOOP: {
      const auto& loop = static_cast<const ForInExprAST&>(expr);
      walkLoop({loop.getIterable()}, loop.getBody(), nullptr);
      return;
    }
    case ASTNodeType::LAMBDA:
    case ASTNodeType::FUNCTION: {
      // A lambda runs later, not here, so its body is not part of this walk.
      // Making one that touches the object is itself a use of `this`, so it
      // needs the object whole — and then every write in it replaces a value,
      // which is what a write means unless this walk says otherwise.
      if (usesThis(expr)) {
        noteObjectUse("capture 'this' in a lambda or nested function",
                      expr.getLocation());
      }
      return;
    }
    default:
      break;
  }
  walkChildren(expr);
}

}  // namespace

void checkFieldInitialization(const FunctionAST& constructor,
                              const ClassType& classType,
                              const std::vector<ClassMethodDecl>& methods) {
  if (!constructor.hasBody()) return;
  if (constructor.isSynthesizedConstructor() &&
      constructor.getFieldInitializerCount() != classType.getFields().size()) {
    logAndThrowError(
        "An explicit init is required for inherited fields without defaults",
        constructor.getLocation());
  }
  ClassInitInfo info(classType, methods);
  BodyWalk walk(info);
  walk.walk(constructor.getBody());
  walk.requireEveryFieldAtEnd(constructor.getBody(), constructor.getLocation());
}

}  // namespace sun
