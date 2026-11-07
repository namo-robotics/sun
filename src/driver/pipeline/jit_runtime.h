/** Internal lifetime guard for code executed by the compiler's JIT. */
#pragma once

/** Compiler driver implementation. */
namespace sun::driver {
/** Drains JIT exit handlers before the generated code is unmapped. */
struct JITExitScope {
  /** Runs and releases the handlers registered by the completed program. */
  ~JITExitScope();
};
}  // namespace sun::driver
