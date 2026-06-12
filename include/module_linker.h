#pragma once

#include <llvm/IR/Module.h>

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "library_cache.h"
#include "moon_import.h"

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

  /// Register available modules without linking their bitcode
  /// Builds symbol-to-module mapping for deferred linking
  /// @param moduleKeys List of module keys to make available
  void registerAvailableModules(const std::vector<std::string>& moduleKeys);

  /// Register available modules with module name remapping (aliasing)
  /// This enables using multiple versions of the same library by remapping
  /// module names at link time (e.g., "sun" -> "sun_v1")
  /// @param moonImport Moon import configuration with optional remapping
  void registerAvailableModulesWithRemap(const MoonImport& moonImport);

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
  bool linkModuleRecursive(const std::string& moduleKey);

  /// Build symbol-to-module mapping from module metadata
  void buildSymbolMap(const std::string& moduleKey);

  /// Build symbol-to-module mapping with module name remapping
  void buildSymbolMapWithRemap(
      const std::string& moduleKey,
      const std::unordered_map<std::string, std::string>& moduleRemap);

  /// Remap a symbol name by replacing module names according to remap config
  /// e.g., "$hash$_sun_Vec_push" -> "$hash$_sun_v1_Vec_push"
  std::string remapSymbolName(
      const std::string& symbol,
      const std::unordered_map<std::string, std::string>& moduleRemap) const;

  llvm::Module& target_;
  std::set<std::string> linkedModules_;
  std::set<std::string>
      linkedContentHashes_;  // Content hashes of linked bitcode
  std::set<std::string> availableModules_;
  std::unordered_map<std::string, std::string>
      symbolToModule_;  // mangled name -> moduleKey

  /// Module key -> remap configuration (for aliased modules)
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      moduleRemaps_;

  std::string error_;
};

}  // namespace sun
