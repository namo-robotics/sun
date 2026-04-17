// include/borrow_checker/borrow_state.h
// Tracks active borrows for all variables in the current analysis context

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "borrow_checker/loan.h"

namespace sun {

/// Tracks the borrow state of all variables during analysis
/// Enforces Rust-style borrow rules:
/// - At any given time, either ONE mutable borrow OR any number of shared
/// borrows
/// - Cannot mutate a variable while it has active borrows (unless through the
/// borrow)
/// - Borrows are invalidated when they go out of scope
class BorrowState {
 public:
  BorrowState() = default;

  /// Attempt to create a new borrow
  /// Returns error if this would violate borrow rules
  BorrowCheckResult addBorrow(const std::string& borrowedVar,
                              const std::string& refName, BorrowKind kind,
                              size_t scopeDepth, const SourceLoc& loc);

  /// Check if we can mutate a variable directly (not through a reference)
  /// Mutation is blocked if there are any active borrows
  BorrowCheckResult canMutateDirectly(const std::string& var) const;

  /// Check if we can mutate through a specific reference
  /// Only allowed if this is the only active mutable borrow
  BorrowCheckResult canMutateThroughRef(const std::string& refName) const;

  /// Check if we can read a variable
  /// Reading is always allowed unless there's an active mutable borrow by
  /// another ref
  BorrowCheckResult canRead(const std::string& var,
                            const std::string& throughRef = "") const;

  /// Called when exiting a scope - invalidates borrows at or deeper than
  /// scopeDepth
  void exitScope(size_t scopeDepth);

  /// Get all active loans for a variable (for error reporting)
  std::vector<Loan> getActiveLoans(const std::string& var) const;

  /// Get all active loans (for debugging)
  std::vector<Loan> getAllActiveLoans() const;

  /// Check if a name refers to an active reference
  bool isActiveRef(const std::string& name) const;

  /// Get the variable that a reference points to (if it's an active ref)
  const std::string* getRefTarget(const std::string& refName) const;

  /// Clear all state (for testing)
  void clear();

 private:
  /// Maps variable name -> list of active loans on that variable
  std::unordered_map<std::string, std::vector<Loan>> loans_;

  /// Maps reference name -> target variable (for quick lookup)
  std::unordered_map<std::string, std::string> refToTarget_;
};

}  // namespace sun
