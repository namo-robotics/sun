#include "abi/abi_validation_scope.h"

#include <utility>

/** Manages the lifetime of ABI validation state for a compilation. */
namespace sun::abi {
thread_local AbiCompatibilityChecker* AbiValidationScope::currentChecker_ =
    nullptr;

AbiValidationScope::AbiValidationScope(CompilationContext context)
    : checker_(std::move(context)), previous_(currentChecker_) {
  currentChecker_ = &checker_;
}

AbiValidationScope::~AbiValidationScope() { currentChecker_ = previous_; }
}  // namespace sun::abi
