// include/borrow_checker/loan.h
// Represents an individual borrow (loan) of a variable

#pragma once

#include <string>

namespace sun {

/// The kind of borrow - determines what operations are allowed
enum class BorrowKind {
  Shared,  // Immutable borrow - multiple allowed, no mutation through this ref
  Mutable  // Mutable borrow - exclusive, can mutate through this ref
};

/// Source location for error reporting
struct SourceLoc {
  int line = 0;
  int column = 0;
  std::string filename;

  std::string toString() const {
    if (filename.empty()) {
      return std::to_string(line) + ":" + std::to_string(column);
    }
    return filename + ":" + std::to_string(line) + ":" + std::to_string(column);
  }
};

/// Represents an active borrow of a variable
/// When `ref r = x` is executed, a Loan is created tracking that `r` borrows
/// `x`
struct Loan {
  std::string borrowedVar;  // The variable being borrowed (e.g., "x")
  std::string refName;      // The reference variable name (e.g., "r")
  BorrowKind kind;          // Shared or Mutable
  size_t scopeDepth;        // Scope level where borrow was created
  SourceLoc location;       // Where the borrow occurred (for error messages)
  bool isActive = true;     // False when ref goes out of scope

  Loan() = default;
  Loan(std::string borrowed, std::string ref, BorrowKind k, size_t depth,
       SourceLoc loc)
      : borrowedVar(std::move(borrowed)),
        refName(std::move(ref)),
        kind(k),
        scopeDepth(depth),
        location(std::move(loc)) {}

  bool isMutable() const { return kind == BorrowKind::Mutable; }
  bool isShared() const { return kind == BorrowKind::Shared; }
};

/// Result of checking if a borrow/mutation is allowed
struct BorrowCheckResult {
  bool allowed = true;
  std::string errorMessage;
  Loan conflictingLoan;  // The loan that caused the conflict (if any)

  static BorrowCheckResult ok() { return {true, "", {}}; }

  static BorrowCheckResult error(const std::string& msg, const Loan& conflict) {
    return {false, msg, conflict};
  }
};

}  // namespace sun
