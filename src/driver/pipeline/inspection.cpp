/** Writes driver debug artifacts and LLVM IR views. */
#include <llvm/Support/FileSystem.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "driver/driver.h"
#include "support/display_path.h"
#include "support/terminal.h"

/** Coordinates compilation, dependency loading, linking, and execution. */
namespace sun::driver {

// Set debug mode and create the debug output folder
void Driver::setDebugMode(bool enable, const std::string& inputFile) {
  debugMode_ = enable;
  if (enable && !inputFile.empty()) {
    // Create debug folder: <basename>_debug/
    std::filesystem::path inputPath(inputFile);
    std::string basename = inputPath.stem().string();
    debugFolder_ = (inputPath.parent_path() / (basename + "_debug")).string();
    if (debugFolder_.empty() || debugFolder_ == "_debug") {
      debugFolder_ = basename + "_debug";
    }
    std::filesystem::create_directories(debugFolder_);
    sun::support::messageStream("info")
        << "Debug output folder: " << sun::support::displayPath(debugFolder_)
        << "/\n";
  }
}

// Helper to dump only user-defined globals and functions to any raw_ostream.
// Used by both terminal printing (with colors) and debug file output.
void Driver::dumpUserDefinedIR(llvm::raw_ostream& OS) {
  // Get user-defined functions from codegen visitor
  const auto& userDefined = codegenVisitor->getUserDefinedFunctions();

  // Print user-defined global variables (skip library globals)
  for (auto& gv : ctx->mainModule->globals()) {
    std::string name = gv.getName().str();
    if (name.empty()) {
      continue;
    }
    // Skip internal symbols starting with _
    if (name[0] == '_') {
      continue;
    }
    // Skip library module globals (vtables, etc.)
    if (name.rfind("std_", 0) == 0) {
      continue;
    }
    // Skip prefixed symbols from moon imports; a bundle build's own globals
    // carry its prefix too, and those are the user's
    if (name.rfind("$", 0) == 0 &&
        (ownBundleHash_.empty() ||
         name.rfind("$" + ownBundleHash_ + "$_", 0) != 0)) {
      continue;
    }
    gv.print(OS);
    OS << "\n";
  }

  // Print only user-defined functions
  for (auto& func : ctx->mainModule->functions()) {
    std::string name = func.getName().str();
    // Skip declarations (no body)
    if (func.isDeclaration()) {
      continue;
    }
    // Only print functions that are user-defined
    if (userDefined.count(name) == 0) {
      continue;
    }

    func.print(OS);
    OS << "\n";
  }
}

// Print only IR for user-defined functions (filter out library code)
void Driver::printUserDefinedIR() {
  // ANSI color codes for cyan output
  const char* cyan = "\033[36m";
  const char* reset = "\033[0m";

  llvm::outs() << cyan << "; LLVM IR (user-defined only):\n";
  dumpUserDefinedIR(llvm::outs());
  llvm::outs() << reset;
}

/**
 * Helper: recursively collect all functions reachable from a given function
 */
static void collectReachableFunctions(llvm::Function* func,
                                      std::set<llvm::Function*>& visited) {
  if (!func || func->isDeclaration() || visited.count(func)) {
    return;
  }
  visited.insert(func);

  for (auto& BB : *func) {
    for (auto& I : BB) {
      // Direct calls
      if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
        if (auto* callee = call->getCalledFunction()) {
          collectReachableFunctions(callee, visited);
        }
      }
      // Invoke instructions (for exception handling)
      if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
        if (auto* callee = invoke->getCalledFunction()) {
          collectReachableFunctions(callee, visited);
        }
      }
    }
  }
}

// Print IR for all functions reachable from main() (includes stdlib)
void Driver::printReachableIR() {
  const char* cyan = "\033[36m";
  const char* reset = "\033[0m";

  llvm::outs() << cyan << "; LLVM IR (reachable from main):\n";

  // Start from main and collect all reachable functions
  std::set<llvm::Function*> reachable;
  if (auto* mainFunc = ctx->mainModule->getFunction("main")) {
    collectReachableFunctions(mainFunc, reachable);
  }

  // Global initializers are reachable through llvm.global_ctors, not main
  if (auto* ctors = ctx->mainModule->getGlobalVariable("llvm.global_ctors")) {
    if (ctors->hasInitializer()) {
      if (auto* entries =
              llvm::dyn_cast<llvm::ConstantArray>(ctors->getInitializer())) {
        for (auto& op : entries->operands()) {
          auto* entry = llvm::cast<llvm::ConstantStruct>(op.get());
          if (auto* fn = llvm::dyn_cast<llvm::Function>(
                  entry->getOperand(1)->stripPointerCasts())) {
            collectReachableFunctions(fn, reachable);
          }
        }
      }
    }
  }

  // Print reachable functions (sorted by name for stable output)
  std::vector<llvm::Function*> sorted(reachable.begin(), reachable.end());
  std::sort(sorted.begin(), sorted.end(),
            [](llvm::Function* a, llvm::Function* b) {
              return a->getName() < b->getName();
            });

  for (auto* func : sorted) {
    func->print(llvm::outs());
    llvm::outs() << "\n";
  }

  llvm::outs() << reset;
}

// Write only the user-defined portion of the IR to a file (used in debug mode).
// No ANSI colors, plain text suitable for .ll file.
void Driver::writeUserDefinedIR(const std::string& path) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(path, EC);
  if (EC) {
    sun::support::messageStream("warning")
        << "Could not write " << sun::support::displayPath(path) << ": "
        << EC.message() << "\n";
    return;
  }

  OS << "; LLVM IR (user-defined only)\n";
  OS << "; Generated in debug mode - library and imported symbols filtered "
        "out\n\n";

  dumpUserDefinedIR(OS);
}

}  // namespace sun::driver
