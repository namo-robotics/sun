// support/target_os.h — The OS names Sun exposes for compile-time target
// selection: "linux", "macos", "windows".
//
// Two features share this vocabulary and must never drift apart: the
// _target_is("...") intrinsic, and the manifest's `target: { <os>: ... }`
// blocks that include sources only when compiling for that OS.

#pragma once

#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <optional>
#include <string>

namespace sun {

/// The triple a compilation is actually for: the explicit --target, or the
/// host when none was given. Normalized first: a raw llvm::Triple parses
/// components by position, so the common three-part spelling
/// "aarch64-linux-gnu" would read "linux" as a vendor and report an unknown
/// OS.
inline llvm::Triple resolvedTargetTriple(const std::string& targetTriple) {
  return llvm::Triple(llvm::Triple::normalize(
      targetTriple.empty() ? llvm::sys::getDefaultTargetTriple()
                           : targetTriple));
}

/// The Sun OS name for a triple, or nullopt for an OS Sun has no name for.
inline std::optional<std::string> targetOsName(const llvm::Triple& triple) {
  if (triple.isOSLinux()) return "linux";
  if (triple.isOSDarwin()) return "macos";
  if (triple.isOSWindows()) return "windows";
  return std::nullopt;
}

/// Whether `name` is an OS name Sun knows. Both users of the vocabulary
/// reject unknown names outright, so a typo is a compile error rather than a
/// silently never-matching selector.
inline bool isKnownTargetOs(const std::string& name) {
  return name == "linux" || name == "macos" || name == "windows";
}

}  // namespace sun
