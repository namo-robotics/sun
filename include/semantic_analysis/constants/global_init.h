// global_init.h — How a file-scope variable gets its first value.

#pragma once

#include <optional>
#include <string>

#include "semantic_analysis/constants/constant_value.h"
#include "support/position.h"

/** Evaluates constant expressions while a program is being analyzed. */
namespace sun::semantic_analysis::constants {

/** Where a file-scope variable's first value comes from. */
enum class GlobalInitKind {
  // The value was computed at compile time and is written into the program.
  Image,
  // The value is computed by the startup function, before `main` runs.
  Startup,
};

/**
 * Why a file-scope variable could not be evaluated at compile time: the first
 * thing in its initializer that stood in the way, and where that is.
 */
struct StartupReason {
  std::string message;
  sun::support::Position position;
};

/**
 * What analysis decided about one file-scope variable. Stored on the
 * variable's declaration node (VariableCreationAST::getGlobalInit) and never
 * changed once made.
 */
struct GlobalInitRecord {
  GlobalInitKind kind = GlobalInitKind::Startup;
  // Declared with `const`: only these may be read by other compile-time
  // initializers, because a `var` can change before the reader runs.
  bool isConst = false;
  // Present exactly when kind is Image.
  std::optional<ConstantValue> value;
  // Present exactly when kind is Startup.
  std::optional<StartupReason> reason;
};

}  // namespace sun::semantic_analysis::constants
