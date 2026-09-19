// include/borrow_checker/loan.h
// Represents an individual borrow (loan) of a variable

#pragma once

#include <string>

#include "borrow_checker/lifetime.h"
#include "support/position.h"

/** Checks ownership and lifetimes so references cannot outlive their values. */
namespace sun::borrow_checker {

/**
 * The kind of borrow - determines what operations are allowed
 */
enum class BorrowKind {
  Shared,  // Immutable borrow - multiple allowed, no mutation through this ref
  Mutable  // Mutable borrow - exclusive, can mutate through this ref
};

/**
 * Represents an active borrow of a variable
 * When `ref r = x` is executed, a Loan is created tracking that `r` borrows
 * `x`
 */
struct Loan {
  std::string borrowedVar;  // The variable being borrowed (e.g., "x")
  std::string refName;      // The reference variable name (e.g., "r")
  BorrowKind kind;          // Shared or Mutable
  size_t scopeDepth;        // Scope level where borrow was created
  sun::support::Position
      location;             // Where the borrow occurred (for error messages)
  Lifetime lifetime;        // Lifetime of the borrowed reference
  bool isActive = true;     // False when ref goes out of scope

  /** Records a borrow, its reference binding, access mode, and lifetime. */
  Loan() = default;
  /** Records a borrow, its reference binding, access mode, and lifetime. */
  Loan(std::string borrowed, std::string ref, BorrowKind k, size_t depth,
       sun::support::Position loc, Lifetime lt = Lifetime())
      : borrowedVar(std::move(borrowed)),
        refName(std::move(ref)),
        kind(k),
        scopeDepth(depth),
        location(std::move(loc)),
        lifetime(std::move(lt)) {}

  /** Reports whether the loan grants exclusive write access to its target. */
  bool isMutable() const { return kind == BorrowKind::Mutable; }
  /** Reports whether the loan permits shared read access to its target. */
  bool isShared() const { return kind == BorrowKind::Shared; }
};

/**
 * Result of checking if a borrow/mutation is allowed
 */
struct BorrowCheckResult {
  bool allowed = true;
  std::string errorMessage;
  Loan conflictingLoan;  // The loan that caused the conflict (if any)

  /** Creates a successful borrow-check result with no conflict. */
  static BorrowCheckResult ok() { return {true, "", {}}; }

  /** Creates a failed borrow-check result retaining the conflicting loan. */
  static BorrowCheckResult error(const std::string& msg, const Loan& conflict) {
    return {false, msg, conflict};
  }
};

}  // namespace sun::borrow_checker
