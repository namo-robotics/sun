/** Shares specialization deferral across declaration preparation stages. */
#pragma once

#include "semantic_analysis/generic_specializer.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {

/** Defers specialization bodies until the outermost preparation completes.
 * Nested collectors share the pending work. On failure, the outermost guard
 * discards it so incomplete declarations cannot produce a secondary error.
 */
class DeclarationPreparationGuard {
 public:
  /** Borrows the specializer and joins or starts declaration preparation. */
  explicit DeclarationPreparationGuard(GenericSpecializer& generics)
      : generics_(generics), outermost_(!generics.isInDeclarationPrepass()) {
    generics_.setInDeclarationPrepass(true);
  }

  /** Prevents two guards from owning the same pending specialization work. */
  DeclarationPreparationGuard(const DeclarationPreparationGuard&) = delete;
  /** Keeps the guard attached to its original specialization session. */
  DeclarationPreparationGuard& operator=(const DeclarationPreparationGuard&) =
      delete;

  /** Restores normal specialization and discards work left by an error. */
  ~DeclarationPreparationGuard() {
    if (outermost_) {
      generics_.setInDeclarationPrepass(false);
      generics_.discardDeferredSpecializations();
    }
  }

  /** Checks deferred bodies once every declaration in this group is ready. */
  void complete() {
    if (outermost_) {
      generics_.setInDeclarationPrepass(false);
      generics_.analyzeDeferredSpecializations();
    }
  }

 private:
  GenericSpecializer& generics_;
  bool outermost_;
};

}  // namespace sun::semantic_analysis::passes
