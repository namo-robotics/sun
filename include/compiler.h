#pragma once

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Path.h>

#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

namespace sun {

/// Native libraries to link against, from -l / -L on the command line.
/// Distinct from LibraryCache's search paths, which locate Sun .moon
/// libraries rather than native shared objects.
struct LinkOptions {
  std::vector<std::string> libraries;      // -lfoo  -> "foo"
  std::vector<std::string> searchPaths;    // -Ldir  -> "dir"
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
      // only libfoo.so.N. The loader will not guess a version, but inside a
      // directory the user explicitly pointed us at we can.
      for (const auto& dir : opts.searchPaths) {
        std::error_code ec;
        std::string prefix = "lib" + lib + ".so.";
        for (llvm::sys::fs::directory_iterator it(dir, ec), end;
             it != end && !ec; it.increment(ec)) {
          llvm::StringRef path(it->path());
          if (llvm::sys::path::filename(path).starts_with(prefix)) {
            candidates.push_back(path.str());
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
  // Get target triple
  auto targetTriple = llvm::sys::getDefaultTargetTriple();
  module.setTargetTriple(targetTriple);

  // Lookup the target
  std::string error;
  auto target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
  if (!target) {
    errorMsg = "Failed to lookup target: " + error;
    return false;
  }

  // Create target machine
  auto cpu = "generic";
  auto features = "";
  llvm::TargetOptions opt;
  auto targetMachine = target->createTargetMachine(targetTriple, cpu, features,
                                                   opt, llvm::Reloc::PIC_);

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
                           const std::string& outputPath,
                           std::string& errorMsg,
                           const LinkOptions& linkOpts = {}) {
  // Build linker command using the system C compiler.
  // Use cc (or clang/gcc) to handle linking with the C runtime.
  // -lstdc++ provides the Itanium C++ ABI runtime (__cxa_throw,
  // __cxa_allocate_exception, typeinfo, _Unwind_*) that native exception
  // handling lowers to.
  std::string cmd = "cc -o " + shellQuote(outputPath) + " " +
                    shellQuote(objectPath);

  // -L before -l: the search paths must be in effect when libraries resolve.
  for (const auto& dir : linkOpts.searchPaths) {
    cmd += " -L" + shellQuote(dir);
  }
  for (const auto& lib : linkOpts.libraries) {
    cmd += " -l" + shellQuote(lib);
  }

  cmd += " -lstdc++";

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

  // Step 3: Clean up object file if not keeping it
  if (!keepObjectFile) {
    std::remove(objectPath.c_str());
  }

  return true;
}

}  // namespace sun
