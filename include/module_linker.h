#pragma once

#include <llvm/IR/Module.h>

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "library_cache.h"

namespace sun {

/// Links precompiled .moon modules into a target LLVM module
class ModuleLinker {
 public:
  /// Create a linker for the given target module
  /// @param targetModule The module to link into
  explicit ModuleLinker(llvm::Module& targetModule);

  /// Link a precompiled module by import path
  /// @param importPath The import path (e.g., "stdlib/allocator.sun")
  /// @return true on success
  bool linkModule(const std::string& importPath);

  /// Link multiple modules, resolving dependencies transitively
  /// @param importPaths List of import paths
  /// @return true if all modules linked successfully
  bool linkModules(const std::vector<std::string>& importPaths);

  /// Register available modules without linking their bitcode
  /// Builds symbol-to-module mapping for deferred linking
  /// @param importPaths List of import paths to make available
  void registerAvailableModules(const std::vector<std::string>& importPaths);

  /// Declare all exported functions as external declarations in target module
  /// This allows codegen to reference functions before actual linking
  /// Must call registerAvailableModules() first
  void declareAvailableFunctions();

  /// Link only the modules needed to resolve undefined symbols in target
  /// Must call registerAvailableModules() first
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
  bool linkModuleRecursive(const std::string& importPath);

  /// Build symbol-to-module mapping from module metadata
  void buildSymbolMap(const std::string& importPath);

  llvm::Module& target_;
  std::set<std::string> linkedModules_;
  std::set<std::string> availableModules_;
  std::unordered_map<std::string, std::string>
      symbolToModule_;  // mangled name -> importPath
  std::string error_;
};

}  // namespace sun
