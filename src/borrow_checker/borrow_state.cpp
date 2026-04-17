// src/borrow_checker/borrow_state.cpp
// Implementation of borrow state tracking

#include "borrow_checker/borrow_state.h"

#include <algorithm>

namespace sun {

BorrowCheckResult BorrowState::addBorrow(const std::string& borrowedVar,
                                         const std::string& refName,
                                         BorrowKind kind, size_t scopeDepth,
                                         const SourceLoc& loc) {
  auto& varLoans = loans_[borrowedVar];

  // Check existing borrows on this variable
  for (const auto& loan : varLoans) {
    if (!loan.isActive) continue;

    if (kind == BorrowKind::Mutable) {
      // Mutable borrow requires no existing borrows
      return BorrowCheckResult::error(
          "cannot borrow '" + borrowedVar +
              "' as mutable because it is already borrowed",
          loan);
    } else {
      // Shared borrow is blocked by existing mutable borrow
      if (loan.isMutable()) {
        return BorrowCheckResult::error(
            "cannot borrow '" + borrowedVar +
                "' as shared because it is already mutably borrowed",
            loan);
      }
    }
  }

  // Create the new loan
  Loan newLoan(borrowedVar, refName, kind, scopeDepth, loc);
  varLoans.push_back(newLoan);
  refToTarget_[refName] = borrowedVar;

  return BorrowCheckResult::ok();
}

BorrowCheckResult BorrowState::canMutateDirectly(const std::string& var) const {
  auto it = loans_.find(var);
  if (it == loans_.end()) {
    return BorrowCheckResult::ok();
  }

  // Check if there are any active borrows
  for (const auto& loan : it->second) {
    if (loan.isActive) {
      return BorrowCheckResult::error(
          "cannot assign to '" + var + "' because it is borrowed", loan);
    }
  }

  return BorrowCheckResult::ok();
}

BorrowCheckResult BorrowState::canMutateThroughRef(
    const std::string& refName) const {
  // Find what variable this ref points to
  auto targetIt = refToTarget_.find(refName);
  if (targetIt == refToTarget_.end()) {
    // Not a known reference - probably an error elsewhere
    return BorrowCheckResult::ok();
  }

  const std::string& targetVar = targetIt->second;
  auto loansIt = loans_.find(targetVar);
  if (loansIt == loans_.end()) {
    return BorrowCheckResult::ok();
  }

  // Find the loan for this ref and check it's mutable
  const Loan* ourLoan = nullptr;
  for (const auto& loan : loansIt->second) {
    if (loan.isActive && loan.refName == refName) {
      ourLoan = &loan;
      break;
    }
  }

  if (!ourLoan) {
    // Our ref is no longer active
    return BorrowCheckResult::error(
        "cannot use reference '" + refName + "' - borrow has ended", Loan());
  }

  if (!ourLoan->isMutable()) {
    return BorrowCheckResult::error("cannot mutate through '" + refName +
                                        "' - it is a shared (immutable) borrow",
                                    *ourLoan);
  }

  // Check there are no other active borrows (our mutable borrow is exclusive)
  for (const auto& loan : loansIt->second) {
    if (loan.isActive && loan.refName != refName) {
      return BorrowCheckResult::error(
          "cannot mutate through '" + refName +
              "' while variable is also borrowed by '" + loan.refName + "'",
          loan);
    }
  }

  return BorrowCheckResult::ok();
}

BorrowCheckResult BorrowState::canRead(const std::string& var,
                                       const std::string& throughRef) const {
  // Reading is always allowed for direct access
  // For ref access, we need to check the ref is still active
  if (!throughRef.empty()) {
    auto targetIt = refToTarget_.find(throughRef);
    if (targetIt == refToTarget_.end()) {
      return BorrowCheckResult::ok();  // Unknown ref
    }

    auto loansIt = loans_.find(targetIt->second);
    if (loansIt == loans_.end()) {
      return BorrowCheckResult::ok();
    }

    // Check our ref is still active
    bool found = false;
    for (const auto& loan : loansIt->second) {
      if (loan.isActive && loan.refName == throughRef) {
        found = true;
        break;
      }
    }

    if (!found) {
      return BorrowCheckResult::error(
          "cannot use reference '" + throughRef + "' - borrow has ended",
          Loan());
    }
  }

  return BorrowCheckResult::ok();
}

void BorrowState::exitScope(size_t scopeDepth) {
  // Invalidate all borrows at or deeper than scopeDepth
  for (auto& [var, varLoans] : loans_) {
    for (auto& loan : varLoans) {
      if (loan.isActive && loan.scopeDepth >= scopeDepth) {
        loan.isActive = false;
        // Remove from refToTarget_ as well
        refToTarget_.erase(loan.refName);
      }
    }
  }

  // Clean up empty loan lists
  for (auto it = loans_.begin(); it != loans_.end();) {
    auto& varLoans = it->second;
    varLoans.erase(std::remove_if(varLoans.begin(), varLoans.end(),
                                  [](const Loan& l) { return !l.isActive; }),
                   varLoans.end());
    if (varLoans.empty()) {
      it = loans_.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<Loan> BorrowState::getActiveLoans(const std::string& var) const {
  std::vector<Loan> result;
  auto it = loans_.find(var);
  if (it != loans_.end()) {
    for (const auto& loan : it->second) {
      if (loan.isActive) {
        result.push_back(loan);
      }
    }
  }
  return result;
}

std::vector<Loan> BorrowState::getAllActiveLoans() const {
  std::vector<Loan> result;
  for (const auto& [var, varLoans] : loans_) {
    for (const auto& loan : varLoans) {
      if (loan.isActive) {
        result.push_back(loan);
      }
    }
  }
  return result;
}

bool BorrowState::isActiveRef(const std::string& name) const {
  return refToTarget_.find(name) != refToTarget_.end();
}

const std::string* BorrowState::getRefTarget(const std::string& refName) const {
  auto it = refToTarget_.find(refName);
  return it != refToTarget_.end() ? &it->second : nullptr;
}

void BorrowState::clear() {
  loans_.clear();
  refToTarget_.clear();
}

}  // namespace sun
