/** Compares declarations and ownership contracts without merging identities. */
#pragma once

#include <string>
#include <vector>

#include "types/types.h"

/** Checks correspondence between independently compiled declarations. */
namespace sun::abi {
class AbiCompatibilityChecker;

/** Explains the first obligation that a correspondence check could not prove.
 */
struct CorrespondenceResult {
  bool proven = false;
  std::string reason;
  std::vector<std::string> path;

  /** Formats a dependency path followed by the unproved obligation. */
  std::string explanation() const;
  /** Adds the containing declaration to a failed dependency check. */
  CorrespondenceResult within(const std::string& component) const;
};

/** Shares representation and implementation checks across transfer boundaries.
 */
class CorrespondenceChecker {
 public:
  /** Uses only the import authorizations belonging to this compilation. */
  explicit CorrespondenceChecker(const AbiCompatibilityChecker& imports);
  /** Checks an owned move; success never authorizes an implicit copy. */
  CorrespondenceResult checkOwnedTransfer(const types::TypePtr& source,
                                          const types::TypePtr& target) const;

  /** Checks use through a borrow, including the owner's storage invariants. */
  CorrespondenceResult checkBorrow(const types::TypePtr& source,
                                   const types::TypePtr& target) const;

 private:
  const AbiCompatibilityChecker& imports_;
  /** Checks fields and payloads without assuming pointer ownership. */
  CorrespondenceResult checkStorage(const types::TypePtr& source,
                                    const types::TypePtr& target) const;
  /** Keeps type arguments invariant except for proven nominal transfers. */
  CorrespondenceResult checkArguments(
      const std::vector<types::TypePtr>& source,
      const std::vector<types::TypePtr>& target) const;
};
}  // namespace sun::abi
