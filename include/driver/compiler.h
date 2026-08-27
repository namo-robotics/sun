#pragma once

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

namespace sun {

/// Native libraries to link against, from -l / -L on the command line.
/// Distinct from LibraryCache's search paths, which locate Sun .moon
/// libraries rather than native shared objects.
struct LinkOptions {
  std::vector<std::string> libraries;    // -lfoo  -> "foo"
  std::vector<std::string> searchPaths;  // -Ldir  -> "dir"
  // Static archives carried inside imported .moon bundles, extracted to
  // disk. Passed to the linker by path, after -l libraries so they can
  // satisfy those libraries' undefined symbols.
  std::vector<std::string> archives;
  std::string targetTriple;  // --target -> cross linker needed
  std::string sysroot;       // --sysroot -> target's root fs
  bool staticLink = false;   // --static -> self-contained binary
};

/// Quote a string for safe use as a single argument in a /bin/sh command.
/// The link command runs through std::system, and -l/-L values come from the
/// user, so they must not be able to inject shell syntax.
inline std::string shellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    // A single-quoted string ends at the next quote; close, escape, reopen.
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

/// Whether `tool` exists on PATH.
inline bool haveTool(const std::string& tool) {
  return std::system(
             ("command -v " + shellQuote(tool) + " >/dev/null 2>&1").c_str()) ==
         0;
}

/// The triple a link is actually for: the explicit --target, or the host.
inline llvm::Triple effectiveLinkTriple(const std::string& targetTriple) {
  return llvm::Triple(targetTriple.empty() ? llvm::sys::getDefaultTargetTriple()
                                           : targetTriple);
}

/// Pick the link driver for a target. SUN_CC overrides the choice entirely.
///
/// Static links prefer a musl toolchain for the target architecture
/// (`<arch>-linux-musl-gcc`, as shipped by musl.cc) when one is installed:
/// musl is designed for static linking (glibc's static binaries still dlopen
/// NSS modules for name lookups), is MIT-licensed (no LGPL relink obligation
/// on the embedded binary), and produces roughly half the binary size. When
/// no musl toolchain is present, the triple's own GCC with -static is used.
///
/// A macOS target links with Apple's own `cc` on a Mac; from any other host
/// there is no driver to pick — linking Mach-O needs the Apple SDK, which
/// cannot ship with a Linux toolchain — so stop at --emit-obj and link the
/// object on a Mac.
///
/// Otherwise: host builds use `cc`; cross builds prefer a triple-prefixed
/// GCC (`aarch64-linux-gnu-gcc`), falling back to `clang --target=<triple>`.
/// Returns "" when no capable link driver exists on this machine.
inline std::string linkerCommandFor(const std::string& targetTriple,
                                    bool staticLink = false) {
  if (const char* env = std::getenv("SUN_CC")) {
    return env;
  }

  if (effectiveLinkTriple(targetTriple).isOSDarwin()) {
    llvm::Triple host(llvm::sys::getDefaultTargetTriple());
    return host.isOSDarwin() ? "cc" : "";
  }

  if (staticLink) {
    llvm::Triple triple = effectiveLinkTriple(targetTriple);
    std::string muslGcc = triple.getArchName().str() + "-linux-musl-gcc";
    if (haveTool(muslGcc)) {
      return muslGcc;
    }
  }

  if (targetTriple.empty()) {
    return "cc";
  }
  std::string crossGcc = targetTriple + "-gcc";
  if (haveTool(crossGcc)) {
    return crossGcc;
  }
  if (haveTool("clang")) {
    std::string cmd = "clang --target=" + shellQuote(targetTriple);
    // LLD links any target; the GNU ld clang would otherwise invoke is
    // usually built for the host only.
    if (haveTool("ld.lld")) {
      cmd += " -fuse-ld=lld";
    }
    return cmd;
  }
  return "";
}

/// Make libraries named by -l visible to the JIT.
/// The JIT resolves externs through DynamicLibrarySearchGenerator over the
/// current process, so a library only has to be dlopen'd into this process
/// for its symbols to become reachable.
///
/// Returns the names that could not be loaded. A name appearing here is NOT
/// necessarily an error: libc and libm are already resident in this process
/// (and glibc ships their .so as a linker script, which dlopen cannot open),
/// so their symbols resolve regardless. Callers should warn rather than
/// abort, and let the JIT report a genuinely missing symbol.
inline std::vector<std::string> loadDynamicLibraries(const LinkOptions& opts) {
  std::vector<std::string> failed;

  for (const auto& lib : opts.libraries) {
    // An explicit path or filename is used as-is; a bare name gets the
    // platform's lib<name>.so decoration and is looked for in -L dirs first,
    // then left to the system loader (LD_LIBRARY_PATH, ldconfig).
    bool looksLikePath = lib.find('/') != std::string::npos ||
                         lib.find(".so") != std::string::npos ||
                         lib.find(".dylib") != std::string::npos;

    std::vector<std::string> candidates;
    if (looksLikePath) {
      candidates.push_back(lib);
    } else {
      for (const auto& dir : opts.searchPaths) {
        candidates.push_back(dir + "/lib" + lib + ".so");
        candidates.push_back(dir + "/lib" + lib + ".dylib");
      }
      candidates.push_back("lib" + lib + ".so");
      candidates.push_back("lib" + lib + ".dylib");

      // Without a -dev package there is no unversioned libfoo.so symlink,
      // only libfoo.so.N (macOS: libfoo.N.dylib). The loader will not guess
      // a version, but inside a directory the user explicitly pointed us at
      // we can.
      for (const auto& dir : opts.searchPaths) {
        std::error_code ec;
        std::string soPrefix = "lib" + lib + ".so.";
        std::string dylibPrefix = "lib" + lib + ".";
        for (llvm::sys::fs::directory_iterator it(dir, ec), end;
             it != end && !ec; it.increment(ec)) {
          llvm::StringRef name = llvm::sys::path::filename(it->path());
          if (name.starts_with(soPrefix) ||
              (name.starts_with(dylibPrefix) && name.ends_with(".dylib"))) {
            candidates.push_back(it->path());
          }
        }
      }
    }

    bool loaded = false;
    for (const auto& candidate : candidates) {
      if (!llvm::sys::DynamicLibrary::LoadLibraryPermanently(candidate.c_str(),
                                                             nullptr)) {
        loaded = true;
        break;
      }
    }

    if (!loaded) failed.push_back(lib);
  }

  return failed;
}

/// Emits an object file from the given LLVM module
/// Returns true on success, false on failure
inline bool emitObjectFile(llvm::Module& module, const std::string& outputPath,
                           std::string& errorMsg) {
  // Honor a triple codegen already chose (set by --target); default to the
  // host otherwise.
  std::string targetTriple = module.getTargetTriple();
  if (targetTriple.empty()) {
    targetTriple = llvm::sys::getDefaultTargetTriple();
    module.setTargetTriple(targetTriple);
  }

  // Lookup the target
  std::string error;
  auto target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
  if (!target) {
    errorMsg = "Failed to lookup target: " + error;
    return false;
  }

  // Create target machine. Debug builds (-g) use CodeGenOptLevel::None so the
  // backend does not merge instructions across source lines (e.g. folding a
  // subtraction into a later comparison's flags), which makes stepping jumpy.
  auto cpu = "generic";
  auto features = "";
  llvm::TargetOptions opt;
  bool hasDebugInfo = module.getModuleFlag("Debug Info Version") != nullptr;
  auto targetMachine = target->createTargetMachine(
      targetTriple, cpu, features, opt, llvm::Reloc::PIC_,
      /*CM=*/std::nullopt,
      hasDebugInfo ? llvm::CodeGenOptLevel::None
                   : llvm::CodeGenOptLevel::Default);

  if (!targetMachine) {
    errorMsg = "Failed to create target machine";
    return false;
  }

  module.setDataLayout(targetMachine->createDataLayout());

  // Open output file
  std::error_code ec;
  llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
  if (ec) {
    errorMsg = "Could not open output file: " + ec.message();
    return false;
  }

  // Emit object code
  llvm::legacy::PassManager pass;
  auto fileType = llvm::CodeGenFileType::ObjectFile;

  if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
    errorMsg = "Target machine cannot emit object file";
    return false;
  }

  pass.run(module);
  dest.flush();

  return true;
}

/// Links the object file to create an executable
/// Uses the system C compiler (cc) as the linker
/// Returns true on success, false on failure
inline bool linkExecutable(const std::string& objectPath,
                           const std::string& outputPath, std::string& errorMsg,
                           const LinkOptions& linkOpts = {}) {
  llvm::Triple triple = effectiveLinkTriple(linkOpts.targetTriple);
  bool isDarwin = triple.isOSDarwin();

  // Build linker command using a C compiler driver so it handles the C
  // runtime and startup files. The C++ runtime library provides the Itanium
  // C++ ABI symbols (__cxa_throw, __cxa_allocate_exception, typeinfo,
  // _Unwind_*) that native exception handling lowers to: libstdc++ on
  // Linux, libc++ (with libc++abi) on macOS.
  std::string linker =
      linkerCommandFor(linkOpts.targetTriple, linkOpts.staticLink);
  if (linker.empty()) {
    if (isDarwin) {
      errorMsg = "cannot link for '" + linkOpts.targetTriple +
                 "' from this machine: Mach-O linking needs Apple's SDK. "
                 "Stop at --emit-obj and link the object on a Mac (cc -o "
                 "prog prog.o -lc++), or set SUN_CC to a capable driver";
    } else {
      errorMsg = "no link driver for target '" + linkOpts.targetTriple +
                 "': install " + linkOpts.targetTriple +
                 "-gcc or clang (or set SUN_CC), or stop at --emit-obj and "
                 "link on the target machine";
    }
    return false;
  }

  std::string cmd =
      linker + " -o " + shellQuote(outputPath) + " " + shellQuote(objectPath);

  // Static: copy libc/libstdc++ into the binary instead of referencing their
  // .so files. The result runs on any Linux of the same architecture with no
  // loader, no shared-library dependencies and no glibc version coupling —
  // the deployment shape embedded targets want. Needs the toolchain's static
  // archives (libc.a from libc6-dev, libstdc++.a from libstdc++-dev).
  // macOS cannot do this at all: Apple ships no static libSystem or crt0.
  if (linkOpts.staticLink) {
    if (isDarwin) {
      errorMsg = "fully static binaries are not supported on macOS";
      return false;
    }
    cmd += " -static";
  }

  // Cross links need the target's root filesystem for libc and crt files.
  // Apple's driver spells the SDK root -isysroot (and finds it by itself
  // when none is given).
  if (!linkOpts.sysroot.empty()) {
    cmd += isDarwin ? " -isysroot " + shellQuote(linkOpts.sysroot)
                    : " --sysroot=" + shellQuote(linkOpts.sysroot);
  }

  // -L before -l: the search paths must be in effect when libraries resolve.
  for (const auto& dir : linkOpts.searchPaths) {
    cmd += " -L" + shellQuote(dir);
  }
  for (const auto& lib : linkOpts.libraries) {
    cmd += " -l" + shellQuote(lib);
  }
  // Bundle-carried archives go in by path. On GNU linkers,
  // --start-group/--end-group lets them resolve each other's symbols
  // regardless of order (libssl needs libcrypto, and a bundle may carry them
  // either way round). Apple's ld64 rejects those flags and resolves
  // archives iteratively anyway, so there the paths go in plain.
  if (!linkOpts.archives.empty()) {
    if (!isDarwin) cmd += " -Wl,--start-group";
    for (const auto& archive : linkOpts.archives) {
      cmd += " " + shellQuote(archive);
    }
    if (!isDarwin) cmd += " -Wl,--end-group";
  }

  cmd += isDarwin ? " -lc++" : " -lstdc++";

  int result = std::system(cmd.c_str());
  if (result != 0) {
    errorMsg = "Linker failed with exit code: " + std::to_string(result);
    return false;
  }

  return true;
}

/// Compiles the LLVM module to a standalone executable
/// Returns true on success, false on failure
inline bool compileToExecutable(llvm::Module& module,
                                const std::string& outputPath,
                                std::string& errorMsg,
                                bool keepObjectFile = false,
                                const LinkOptions& linkOpts = {}) {
  // Generate temporary object file path
  std::string objectPath = outputPath + ".o";

  // Step 1: Emit object file
  if (!emitObjectFile(module, objectPath, errorMsg)) {
    return false;
  }

  // Step 2: Link to create executable
  if (!linkExecutable(objectPath, outputPath, errorMsg, linkOpts)) {
    return false;
  }

  // On Mach-O the linker leaves debug info in the object file, writing only
  // a debug map that points back at it. Bundle it into a .dSYM while the
  // object still exists; without dsymutil the object itself must survive or
  // the debug info is gone.
  bool hasDebugInfo = module.getModuleFlag("Debug Info Version") != nullptr;
  if (hasDebugInfo && effectiveLinkTriple(linkOpts.targetTriple).isOSDarwin()) {
    if (haveTool("dsymutil")) {
      std::system(("dsymutil " + shellQuote(outputPath)).c_str());
    } else {
      keepObjectFile = true;
    }
  }

  // Step 3: Clean up object file if not keeping it
  if (!keepObjectFile) {
    std::remove(objectPath.c_str());
  }

  return true;
}

}  // namespace sun
