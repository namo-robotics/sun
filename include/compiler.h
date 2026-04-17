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

#include <cstdlib>
#include <string>

namespace sun {

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
                           std::string& errorMsg) {
  // Build linker command using the system C compiler
  // Use cc (or clang/gcc) to handle linking with C runtime
  std::string cmd = "cc -o " + outputPath + " " + objectPath;

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
                                bool keepObjectFile = false) {
  // Generate temporary object file path
  std::string objectPath = outputPath + ".o";

  // Step 1: Emit object file
  if (!emitObjectFile(module, objectPath, errorMsg)) {
    return false;
  }

  // Step 2: Link to create executable
  if (!linkExecutable(objectPath, outputPath, errorMsg)) {
    return false;
  }

  // Step 3: Clean up object file if not keeping it
  if (!keepObjectFile) {
    std::remove(objectPath.c_str());
  }

  return true;
}

}  // namespace sun
