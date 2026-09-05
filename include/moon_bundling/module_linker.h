#pragma once

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "moon_bundling/library_cache.h"
#include "moon_bundling/moon_import.h"

namespace sun {

/// Links precompiled .moon modules into a target LLVM module
class ModuleLinker {
 public:
  /// Create a linker for the given target module
  /// @param targetModule The module to link into
  explicit ModuleLinker(llvm::Module& targetModule);

  /// Link a precompiled module by module key (source hash)
  /// @param moduleKey The module key (source hash)
  /// @return true on success
  bool linkModule(const std::string& moduleKey);

  /// Link multiple modules, resolving dependencies transitively
  /// @param moduleKeys List of module keys
  /// @return true if all modules linked successfully
  bool linkModules(const std::vector<std::string>& moduleKeys);

  /// Register available modules without linking their bitcode. Which
  /// symbols each provides is learned by declareAvailableFunctions().
  /// @param moduleKeys List of module keys to make available
  void registerAvailableModules(const std::vector<std::string>& moduleKeys);

  /** Register a bundle's original compiled symbols; aliases are source-only. */
  void registerAvailableBundle(const MoonImport& moonImport);

  /// Declare all exported functions as external declarations in target module
  /// This allows codegen to reference functions before actual linking, and
  /// records which module defines each symbol for linkOnlyUsedSymbols()
  /// Must call registerAvailableModules() first
  void declareAvailableFunctions();

  /// Link only the modules needed to resolve undefined symbols in target
  /// Must call declareAvailableFunctions() first
  /// @return true on success
  bool linkOnlyUsedSymbols();

  /// Get list of successfully linked modules
  const std::set<std::string>& getLinkedModules() const {
    return linkedModules_;
  }

  /// Get error message if linking failed
  const std::string& getError() const { return error_; }

 private:
  /// Link a module and its dependencies recursively
  bool linkModuleRecursive(const std::string& moduleKey);

  llvm::Module& target_;
  // Context for modules parsed by declareAvailableFunctions(). Scans use
  // their own context so the target never shares struct type objects with a
  // future llvm::Linker source module.
  std::unique_ptr<llvm::LLVMContext> scanContext_;
  std::set<std::string> linkedModules_;
  std::set<std::string>
      linkedContentHashes_;  // Content hashes of linked bitcode
  std::set<std::string> availableModules_;
  std::unordered_map<std::string, std::string>
      symbolToModule_;  // mangled name -> moduleKey

  std::string error_;
};

}  // namespace sun
