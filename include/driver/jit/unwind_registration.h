/** Declares compiler-side unwind metadata registration for the in-process JIT. */
#pragma once

/** Declares the LLVM execution and object-linking services used by the JIT. */
namespace llvm::orc {
class ExecutionSession;
class ObjectLinkingLayer;
}

/** Provides services used only while loading or executing JIT-generated code. */
namespace sun::driver::jit {
/** Adds unwind registration, eager indexing, and module-local personality cells.
 * The layer owns registration state and removes it before unmapping its code.
 * This compiler-side service is not part of the generated program's runtime.
 */
void addUnwindRegistration(llvm::orc::ObjectLinkingLayer& layer,
                           llvm::orc::ExecutionSession& session);
}  // namespace sun::driver::jit
