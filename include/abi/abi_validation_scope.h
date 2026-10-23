#pragma once

#include "abi/abi_compatibility_checker.h"

/** Manages the lifetime of ABI validation state for a compilation. */
namespace sun::abi {
/** Owns the ABI checker for one compilation and makes it available through
 * AbiCompatibilityChecker::current() on the calling thread. Keep this scope
 * alive across dependency preflight and compilation so both phases share
 * validation caches, imported requirements, and warning state.
 *
 * A nested scope uses a fresh checker for a dependency build. Destruction
 * restores the previous checker, including when a build throws, so the nested
 * build cannot overwrite the parent compilation's state. Scopes must be
 * destroyed in reverse construction order on the same thread.
 */
class AbiValidationScope {
 public:
  /** Starts an isolated compilation on this thread. */
  explicit AbiValidationScope(CompilationContext context);
  /** Restores the parent compilation, including its warnings and imports. */
  ~AbiValidationScope();
  /** Keeps scope ownership tied to its original stack lifetime. */
  AbiValidationScope(const AbiValidationScope&) = delete;
  /** Prevents replacing a live thread-local scope. */
  AbiValidationScope& operator=(const AbiValidationScope&) = delete;

 private:
  friend class AbiCompatibilityChecker;
  // The innermost scope owns this checker; each thread has its own stack.
  static thread_local AbiCompatibilityChecker* currentChecker_;
  AbiCompatibilityChecker checker_;
  AbiCompatibilityChecker* previous_;
};
}  // namespace sun::abi
