// field_initialization.cpp — What is initialized where inside a constructor
//
// The walk carries two views of the fields at each point in a body:
//
//   - the fields assigned on EVERY path that reaches here. Reading a field,
//     or calling a method that reads one, is allowed only once this covers
//     the field in question. Branches intersect, and a loop body contributes
//     nothing, since it may run no times at all.
//   - the fields assigned on SOME path that reaches here. A write to one of
//     these may have to drop what is there. Branches union, and a loop body
//     is seeded with everything it writes, since it may run again.
//
// Between them the two answer what a write does to the value that was there:
// in neither set, the write starts the field's life and drops nothing; in
// both, it replaces a value and drops it; in one but not the other, the walk
// cannot tell, and codegen decides at run time by looking at the storage.
//
// A constructor may hand the work to its own methods. Each such method is
// walked the same way to learn what it assigns and what it needs assigned
// before it runs; the call then discharges the constructor's obligation for
// the fields the method always assigns.

#include "semantic_analysis/field_initialization.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "ast/ast_children.h"
#include "ast/control_flow.h"
#include "support/error.h"

namespace sun {
namespace {

/**
 * Is this expression the constructor's own receiver?
 */
bool isThis(const ExprAST* expr) {
  return expr && expr->getType() == ASTNodeType::THIS;
}

/**
 * Keeps only the names that are in both sets.
 */
void intersectInto(std::set<std::string>& target,
                   const std::set<std::string>& other) {
  for (auto it = target.begin(); it != target.end();) {
    it = other.count(*it) ? std::next(it) : target.erase(it);
  }
}

/**
 * What one method of the class does to the fields, seen from a caller that
 * does not know which of them hold values yet.
 */
struct MethodSummary {
  // Assigned on every path through the method
  std::set<std::string> assigns;
  // Assigned on some path through the method
  std::set<std::string> mayAssign;
  // Must already hold a value when the method is called
  std::set<std::string> needs;
};

class BodyWalk;

/**
 * The fields and methods of one class, and the summaries worked out for the
 * methods a constructor hands work to.
 */
class ClassInitInfo {
 public:
  ClassInitInfo(const ClassType& classType,
                const std::vector<ClassMethodDecl>& methods)
      : classType_(classType), methods_(methods) {
    for (const auto& field : classType.getFields())
      allFields_.insert(field.name);
  }

  const ClassType& classType() const { return classType_; }
  const std::set<std::string>& allFields() const { return allFields_; }
  bool isField(const std::string& name) const {
    return allFields_.count(name) != 0;
  }

  /**
   * What calling this method of the class does to the fields. A method the
   * class does not declare here, or one that calls itself round again, is
   * taken to need everything and to promise nothing.
   */
  const MethodSummary& summaryOf(const std::string& methodName);

 private:
  const ClassType& classType_;
  const std::vector<ClassMethodDecl>& methods_;
  std::set<std::string> allFields_;
  std::map<std::string, MethodSummary> summaries_;
  std::set<std::string> inProgress_;

  /**
   * A summary that assumes the worst: the method needs every field to hold a
   * value already, and may write to any of them.
   */
  MethodSummary unknownSummary() const { return {{}, allFields_, allFields_}; }
};

/**
 * Walks one body in source order, tagging each write to a field and either
 * reporting what the body may not do (a constructor) or noting what it needs
 * from its caller (a method a constructor hands work to).
 */
class BodyWalk {
 public:
  BodyWalk(ClassInitInfo& info, bool isConstructor)
      : info_(info), isConstructor_(isConstructor) {
    if (!isConstructor) {
      // A method cannot see what its caller assigned, so every field may
      // already hold a value and none is guaranteed to
      assignedOnSomePath_ = info.allFields();
    }
  }

  /**
   * Walks one expression and everything under it.
   */
  void walk(const ExprAST& expr);

  /**
   * Reports the fields still unassigned where a constructor body ends.
   * Nothing is reported when control cannot get there.
   */
  void requireEveryFieldAtEnd(const ExprAST& body, std::optional<Position> loc);

  /**
   * What the walk learned, for a method a constructor hands work to.
   */
  MethodSummary summary() const {
    return {assignedOnEveryPath_, assignedOnSomePath_, needs_};
  }

 private:
  ClassInitInfo& info_;
  bool isConstructor_;
  // Fields assigned on every path to the current point
  std::set<std::string> assignedOnEveryPath_;
  // Fields assigned on some path to the current point
  std::set<std::string> assignedOnSomePath_;
  // Fields this body needs its caller to have assigned already
  std::set<std::string> needs_;

  std::string className() const { return info_.classType().getDisplayName(); }

  /**
   * The name of a field the object is still missing, or an empty string once
   * every field has a value.
   */
  std::string firstUnassignedField() const;

  /**
   * Records that the body reaches a field's value here. In a constructor
   * that is an error unless the field has one; in a method it becomes
   * something the caller has to have done.
   */
  void noteFieldRead(const std::string& field, const std::string& what,
                     std::optional<Position> loc);

  /**
   * Records that the body reaches the whole object here — every field it has
   * not assigned itself needs a value.
   */
  void noteObjectUse(const std::string& what, std::optional<Position> loc);

  /**
   * Handles `this.method(...)`: what the method needs, and what it assigns
   * on the caller's behalf.
   */
  void walkCallOnThis(const CallExprAST& call, const std::string& methodName);

  void walkChildren(const ExprAST& expr);

  /**
   * Walks the alternatives of a choice — the arms of an `if`, a ternary, a
   * `match`, or a `try` and its `catch` clauses. Each starts from the state
   * before the choice. A null entry stands for a path that runs nothing, as
   * an `if` without an `else` has.
   */
  void walkBranches(const std::vector<const ExprAST*>& branches);

  /**
   * Walks a loop: `once` runs before the first test, `body` and `then` may
   * run any number of times, including none.
   */
  void walkLoop(const std::vector<const ExprAST*>& once, const ExprAST* body,
                const ExprAST* then);

  /**
   * Treats every field written anywhere inside as assigned on some path.
   * Used for a body that can run more than once, and for a `try` block, which
   * a `catch` clause may pick up part-way through.
   */
  void noteEveryWrittenField(const ExprAST& expr);
};

const MethodSummary& ClassInitInfo::summaryOf(const std::string& methodName) {
  auto found = summaries_.find(methodName);
  if (found != summaries_.end()) return found->second;

  const FunctionAST* body = nullptr;
  for (const auto& method : methods_) {
    if (method.function &&
        method.function->getProto().getName() == methodName &&
        method.function->hasBody()) {
      body = method.function.get();
      break;
    }
  }
  // A method whose body is not here, or one already being walked further up
  // the chain, tells us nothing we can lean on
  if (!body || !inProgress_.insert(methodName).second) {
    return summaries_.emplace(methodName, unknownSummary()).first->second;
  }

  BodyWalk walk(*this, /*isConstructor=*/false);
  walk.walk(body->getBody());
  inProgress_.erase(methodName);
  return summaries_.emplace(methodName, walk.summary()).first->second;
}

std::string BodyWalk::firstUnassignedField() const {
  for (const auto& field : info_.classType().getFields()) {
    if (!assignedOnEveryPath_.count(field.name)) return field.name;
  }
  return "";
}

void BodyWalk::noteFieldRead(const std::string& field, const std::string& what,
                             std::optional<Position> loc) {
  if (assignedOnEveryPath_.count(field)) return;
  if (!isConstructor_) {
    needs_.insert(field);
    return;
  }
  logAndThrowError("Field '" + field + "' of '" + className() + "' is " + what +
                       " before it is assigned. A constructor assigns a field "
                       "before reading it back",
                   loc);
}

void BodyWalk::noteObjectUse(const std::string& what,
                             std::optional<Position> loc) {
  std::string missing = firstUnassignedField();
  if (missing.empty()) return;
  if (!isConstructor_) {
    for (const auto& field : info_.allFields()) {
      if (!assignedOnEveryPath_.count(field)) needs_.insert(field);
    }
    return;
  }
  logAndThrowError("Cannot " + what + " in the constructor of '" + className() +
                       "' while field '" + missing +
                       "' has no value yet. A constructor assigns every field, "
                       "on its own or through its own methods, before the "
                       "object may be read from or passed on",
                   loc);
}

void BodyWalk::walkCallOnThis(const CallExprAST& call,
                              const std::string& methodName) {
  for (const auto& arg : call.getArgs()) {
    if (arg) walk(*arg);
  }

  // Once the object is whole this is an ordinary call, and the method is an
  // ordinary method: nothing to work out, nothing to tag
  if (firstUnassignedField().empty()) return;

  const MethodSummary& summary = info_.summaryOf(methodName);
  for (const auto& field : summary.needs) {
    noteFieldRead(field, "read by method '" + methodName + "'",
                  call.getLocation());
  }
  assignedOnEveryPath_.insert(summary.assigns.begin(), summary.assigns.end());
  assignedOnSomePath_.insert(summary.mayAssign.begin(),
                             summary.mayAssign.end());
}

void BodyWalk::requireEveryFieldAtEnd(const ExprAST& body,
                                      std::optional<Position> loc) {
  if (exprDiverges(body)) return;
  std::string missing = firstUnassignedField();
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
  std::set<std::string> everyBefore = assignedOnEveryPath_;
  std::set<std::string> someBefore = assignedOnSomePath_;
  std::set<std::string> someAfter = assignedOnSomePath_;
  std::set<std::string> everyAfter;
  bool anyFallsThrough = false;

  for (const ExprAST* branch : branches) {
    assignedOnEveryPath_ = everyBefore;
    assignedOnSomePath_ = someBefore;
    // A branch that returns or throws never reaches the code after the
    // choice, so neither what it assigned nor what it left behind counts
    if (branch) {
      walk(*branch);
      if (exprDiverges(*branch)) continue;
    }
    someAfter.insert(assignedOnSomePath_.begin(), assignedOnSomePath_.end());
    if (!anyFallsThrough) {
      everyAfter = assignedOnEveryPath_;
      anyFallsThrough = true;
    } else {
      intersectInto(everyAfter, assignedOnEveryPath_);
    }
  }

  assignedOnSomePath_ = std::move(someAfter);
  assignedOnEveryPath_ = anyFallsThrough ? std::move(everyAfter) : everyBefore;
}

void BodyWalk::walkLoop(const std::vector<const ExprAST*>& once,
                        const ExprAST* body, const ExprAST* then) {
  for (const ExprAST* expr : once) {
    if (expr) walk(*expr);
  }
  if (!body) return;
  // The body may run no times at all, so nothing it assigns is guaranteed
  // afterwards; it may also run again, so no write in it starts a field's life
  std::set<std::string> guaranteed = assignedOnEveryPath_;
  noteEveryWrittenField(*body);
  walk(*body);
  if (then) walk(*then);
  assignedOnEveryPath_ = std::move(guaranteed);
}

void BodyWalk::noteEveryWrittenField(const ExprAST& expr) {
  if (expr.getType() == ASTNodeType::MEMBER_ASSIGNMENT) {
    const auto& assign = static_cast<const MemberAssignmentAST&>(expr);
    if (isThis(assign.getObject())) {
      assignedOnSomePath_.insert(assign.getMemberName());
    }
  }
  forEachChild(expr,
               [this](const ExprAST& child) { noteEveryWrittenField(child); });
}

void BodyWalk::walk(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::MEMBER_ASSIGNMENT: {
      const auto& assign = static_cast<const MemberAssignmentAST&>(expr);
      const std::string& field = assign.getMemberName();
      if (isThis(assign.getObject()) && info_.isField(field)) {
        // The value is produced before the store, so it is walked first
        walk(*assign.getValue());
        if (!assignedOnSomePath_.count(field)) {
          assign.setFieldWriteKind(FieldWriteKind::StartsLife);
        } else if (assignedOnEveryPath_.count(field)) {
          assign.setFieldWriteKind(FieldWriteKind::ReplacesValue);
        } else {
          assign.setFieldWriteKind(FieldWriteKind::MayReplaceValue);
        }
        assignedOnSomePath_.insert(field);
        assignedOnEveryPath_.insert(field);
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
      if (!isConstructor_) return;
      std::string missing = firstUnassignedField();
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
      const auto& tryCatch = static_cast<const TryCatchExprAST&>(expr);
      // A throw can leave the try block part-way through, so a catch clause
      // may find fields the try block assigned
      noteEveryWrittenField(tryCatch.getTryBlock());
      std::vector<const ExprAST*> paths{&tryCatch.getTryBlock()};
      for (const auto& clause : tryCatch.getCatchClauses()) {
        paths.push_back(clause.body.get());
      }
      walkBranches(paths);
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
    case ASTNodeType::FUNCTION:
      // A body that is called later, perhaps many times: no write in it
      // starts a field's life
      noteEveryWrittenField(expr);
      break;
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
  ClassInitInfo info(classType, methods);
  BodyWalk walk(info, /*isConstructor=*/true);
  walk.walk(constructor.getBody());
  walk.requireEveryFieldAtEnd(constructor.getBody(), constructor.getLocation());
}

}  // namespace sun
