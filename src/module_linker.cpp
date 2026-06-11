#include "module_linker.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/Linker/Linker.h>

#include "module_types.h"
#include "moon/moon.h"
#include "struct_names.h"

namespace sun {

namespace {

/// Remap a type from a loaded module to the target module's equivalent type.
/// Handles renaming of known struct types like static_ptr_struct.N ->
/// static_ptr_struct
llvm::Type* remapTypeToTarget(llvm::Type* srcType, llvm::LLVMContext& ctx) {
  if (!srcType) return nullptr;

  // Handle struct types with numbered suffixes (e.g., static_ptr_struct.19)
  if (auto* structTy = llvm::dyn_cast<llvm::StructType>(srcType)) {
    if (structTy->hasName()) {
      llvm::StringRef name = structTy->getName();

      // Check against all well-known struct types
      for (const auto& info : sun::StructNames::All) {
        if (name.starts_with(info.name)) {
          // Get or create the canonical type in the context
          llvm::StructType* canonical =
              llvm::StructType::getTypeByName(ctx, info.name);
          if (!canonical) {
            canonical = llvm::StructType::create(ctx, info.name);
            auto* ptrTy = llvm::PointerType::getUnqual(ctx);
            switch (info.layout) {
              case sun::StructNames::Layout::PtrPtr:
                canonical->setBody({ptrTy, ptrTy});
                break;
              case sun::StructNames::Layout::PtrI64:
                canonical->setBody({ptrTy, llvm::Type::getInt64Ty(ctx)});
                break;
              case sun::StructNames::Layout::PtrI32Ptr:
                canonical->setBody({ptrTy, llvm::Type::getInt32Ty(ctx), ptrTy});
                break;
            }
          }
          return canonical;
        }
      }
    }
  }

  // No remapping needed
  return srcType;
}

/// Create a function type with remapped parameter and return types
llvm::FunctionType* remapFunctionType(llvm::FunctionType* srcFuncType,
                                      llvm::LLVMContext& ctx) {
  // Remap return type
  llvm::Type* retType = remapTypeToTarget(srcFuncType->getReturnType(), ctx);

  // Remap parameter types
  llvm::SmallVector<llvm::Type*, 8> paramTypes;
  for (llvm::Type* paramType : srcFuncType->params()) {
    paramTypes.push_back(remapTypeToTarget(paramType, ctx));
  }

  return llvm::FunctionType::get(retType, paramTypes, srcFuncType->isVarArg());
}

}  // namespace

ModuleLinker::ModuleLinker(llvm::Module& targetModule)
    : target_(targetModule) {}

bool ModuleLinker::linkModule(const std::string& moduleKey) {
  return linkModuleRecursive(moduleKey);
}

bool ModuleLinker::linkModules(const std::vector<std::string>& moduleKeys) {
  for (const auto& key : moduleKeys) {
    if (!linkModuleRecursive(key)) {
      return false;
    }
  }
  return true;
}

void ModuleLinker::registerAvailableModules(
    const std::vector<std::string>& moduleKeys) {
  for (const auto& key : moduleKeys) {
    if (availableModules_.count(key)) {
      continue;
    }
    availableModules_.insert(key);
    buildSymbolMap(key);
  }
}

void ModuleLinker::buildSymbolMap(const std::string& moduleKey) {
  auto* metadata = LibraryCache::instance().getMetadata(moduleKey);
  if (!metadata) {
    return;
  }

  // Get symbol prefix for constructing qualified names
  // Note: getSymbolPrefix returns "$hash$", and bitcode uses "$hash$_name"
  // format
  std::string prefix = sun::getSymbolPrefix(*metadata);
  std::string moduleName = metadata->module_name();

  // Map exported functions to this module
  // Bitcode symbol format: prefix + "_" + moduleName + "_" + funcName
  for (int i = 0; i < metadata->functions_size(); ++i) {
    const auto& func = metadata->functions(i);
    const auto& proto = func.proto();
    std::string funcName = proto.name();

    // Construct qualified name matching bitcode: $hash$_module_func
    std::string qualifiedName;
    if (!moduleName.empty()) {
      qualifiedName = prefix + "_" + moduleName + "_" + funcName;
    } else {
      qualifiedName = prefix + "_" + funcName;
    }

    if (!qualifiedName.empty()) {
      symbolToModule_[qualifiedName] = moduleKey;
    }
  }

  // Map class methods - only non-generic classes have callable methods
  // Generic class specializations are handled via codegen (not metadata)
  for (int i = 0; i < metadata->classes_size(); ++i) {
    const auto& cls = metadata->classes(i);

    // Skip generic classes - their methods require instantiation
    if (cls.type_parameters_size() > 0) continue;

    // Construct class qualified name matching bitcode
    std::string className;
    if (!moduleName.empty()) {
      className = prefix + "_" + moduleName + "_" + cls.name();
    } else {
      className = prefix + "_" + cls.name();
    }

    for (int j = 0; j < cls.methods_size(); ++j) {
      const auto& method = cls.methods(j);
      const auto& methodProto = method.function().proto();

      // Skip generic methods
      if (methodProto.type_parameters_size() > 0) continue;

      std::string mangledName = className + "_" + methodProto.name();
      symbolToModule_[mangledName] = moduleKey;
    }
  }
}

void ModuleLinker::declareAvailableFunctions() {
  auto& ctx = target_.getContext();

  for (const auto& moduleKey : availableModules_) {
    // Load the bitcode module to scan its functions directly
    // This captures all concrete functions including generic specializations
    // NOTE: Symbols in the bitcode are ALREADY prefixed with the content hash
    // (done at moon bundle creation time), so we don't add prefixes here.
    auto libModule = LibraryCache::instance().loadModule(moduleKey, ctx);
    if (!libModule) continue;

    // Check if this module has aliasing configured
    auto remapIt = moduleRemaps_.find(moduleKey);
    bool hasRemap =
        (remapIt != moduleRemaps_.end() && !remapIt->second.empty());
    const auto* remap = hasRemap ? &remapIt->second : nullptr;

    // Scan all defined functions in the bitcode and create declarations
    for (const auto& func : libModule->functions()) {
      // Skip declarations (external functions the lib depends on)
      if (func.isDeclaration()) continue;
      // Skip LLVM intrinsics
      if (func.isIntrinsic()) continue;
      // Skip unnamed functions
      if (!func.hasName() || func.getName().empty()) continue;

      std::string funcName = func.getName().str();

      // Skip internal helper functions (start with underscore or llvm.)
      if (funcName[0] == '_' && funcName.size() > 1 && funcName[1] == '_') {
        continue;  // Skip __sun_* helper functions
      }

      // Apply aliasing if configured
      std::string declaredName = funcName;
      if (remap) {
        declaredName = remapSymbolName(funcName, *remap);
      }

      // Skip if already declared in target
      if (target_.getFunction(declaredName)) continue;

      // Clone the function type and remap struct types to target module's types
      // This fixes type mismatches like static_ptr_struct vs
      // static_ptr_struct.19
      llvm::FunctionType* funcType =
          remapFunctionType(func.getFunctionType(), ctx);

      // Create external declaration with the (potentially aliased) name
      llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                             declaredName, &target_);

      // Map the aliased name to the module for linking
      symbolToModule_[declaredName] = moduleKey;
    }
  }
}

bool ModuleLinker::linkOnlyUsedSymbols() {
  std::set<std::string> neededModules;

  // Find undefined symbols in target module that we can provide
  for (const auto& F : target_) {
    if (F.isDeclaration() && !F.isIntrinsic() && !F.getName().empty()) {
      std::string name = F.getName().str();
      auto it = symbolToModule_.find(name);
      if (it != symbolToModule_.end()) {
        neededModules.insert(it->second);
      }
    }
  }

  // Also check for undefined globals
  for (const auto& G : target_.globals()) {
    if (G.isDeclaration() && !G.getName().empty()) {
      std::string name = G.getName().str();
      auto it = symbolToModule_.find(name);
      if (it != symbolToModule_.end()) {
        neededModules.insert(it->second);
      }
    }
  }

  // Link only the needed modules (and their dependencies)
  for (const auto& modPath : neededModules) {
    if (!linkModuleRecursive(modPath)) {
      return false;
    }
  }

  return true;
}

bool ModuleLinker::linkModuleRecursive(const std::string& moduleKey) {
  // Already linked?
  if (linkedModules_.count(moduleKey)) {
    return true;
  }

  // Get metadata to find dependencies and content hash
  auto* metadata = LibraryCache::instance().getMetadata(moduleKey);
  if (!metadata) {
    error_ = "Module not found in library cache: " + moduleKey;
    return false;
  }

  // Note: We do NOT recursively load dependencies here.
  // Moon files are self-contained - when a.moon was created from a.sun
  // (which imported b_v1.moon), the bitcode from b_v1 was already linked
  // into a.moon. The deps list in metadata is for informational purposes
  // only (tracking what the module depends on), not for runtime loading.

  // Load the module bitcode
  // NOTE: All symbols in the bitcode are already prefixed with the content hash
  // (done at moon bundle creation time). This provides:
  // 1. Symbol isolation between different library versions
  // 2. Integrity verification - if bitcode is modified, symbols won't match
  // 3. Struct type isolation to prevent LLVM type merging issues
  auto libModule =
      LibraryCache::instance().loadModule(moduleKey, target_.getContext());
  if (!libModule) {
    // Get detailed error from the reader
    auto* bundle = LibraryCache::instance().findBundleForModule(moduleKey);
    if (bundle) {
      error_ = "Failed to load bitcode for: " + moduleKey + " - " +
               bundle->getError();
    } else {
      error_ =
          "Failed to load bitcode for: " + moduleKey + " (bundle not found)";
    }
    return false;
  }

  // Check if this module has aliasing configured
  auto remapIt = moduleRemaps_.find(moduleKey);
  if (remapIt != moduleRemaps_.end() && !remapIt->second.empty()) {
    // Rename all functions/globals in the bitcode module to use aliased names
    const auto& remap = remapIt->second;

    // Rename functions
    for (auto& func : libModule->functions()) {
      if (func.hasName() && !func.getName().empty()) {
        std::string oldName = func.getName().str();
        std::string newName = remapSymbolName(oldName, remap);
        if (newName != oldName) {
          func.setName(newName);
        }
      }
    }

    // Rename globals
    for (auto& global : libModule->globals()) {
      if (global.hasName() && !global.getName().empty()) {
        std::string oldName = global.getName().str();
        std::string newName = remapSymbolName(oldName, remap);
        if (newName != oldName) {
          global.setName(newName);
        }
      }
    }
  }

  // Ensure the library module has same data layout as target
  // This avoids "Linking two modules of different data layouts" warnings
  if (libModule->getDataLayoutStr().empty()) {
    libModule->setDataLayout(target_.getDataLayout());
  }

  // Link into target
  // Using Linker::linkModules with OverrideFromSrc to handle duplicate symbols
  if (llvm::Linker::linkModules(target_, std::move(libModule),
                                llvm::Linker::Flags::OverrideFromSrc)) {
    error_ = "LLVM linker failed for: " + moduleKey;
    return false;
  }

  linkedModules_.insert(moduleKey);
  return true;
}

void ModuleLinker::registerAvailableModulesWithRemap(
    const MoonImport& moonImport) {
  // Open the moon file
  auto reader = SunLibReader::open(moonImport.path);
  if (!reader) {
    return;
  }

  // Register each module in the bundle
  for (const auto& moduleKey : reader->listModules()) {
    if (availableModules_.count(moduleKey)) {
      continue;
    }
    availableModules_.insert(moduleKey);

    // Store the remap configuration for this module
    if (moonImport.hasRemap()) {
      moduleRemaps_[moduleKey] = moonImport.moduleRemap;
      buildSymbolMapWithRemap(moduleKey, moonImport.moduleRemap);
    } else {
      buildSymbolMap(moduleKey);
    }
  }
}

void ModuleLinker::buildSymbolMapWithRemap(
    const std::string& moduleKey,
    const std::unordered_map<std::string, std::string>& moduleRemap) {
  auto* metadata = LibraryCache::instance().getMetadata(moduleKey);
  if (!metadata) {
    return;
  }

  std::string prefix = sun::getSymbolPrefix(*metadata);
  std::string originalModuleName = metadata->module_name();

  // Get the aliased module name (if remapped)
  std::string aliasedModuleName = originalModuleName;
  auto it = moduleRemap.find(originalModuleName);
  if (it != moduleRemap.end()) {
    aliasedModuleName = it->second;
  }

  // Map exported functions using ALIASED names
  for (int i = 0; i < metadata->functions_size(); ++i) {
    const auto& func = metadata->functions(i);
    const auto& proto = func.proto();
    std::string funcName = proto.name();

    // Construct qualified name with ALIASED module name
    std::string aliasedQualifiedName;
    if (!aliasedModuleName.empty()) {
      aliasedQualifiedName = prefix + "_" + aliasedModuleName + "_" + funcName;
    } else {
      aliasedQualifiedName = prefix + "_" + funcName;
    }

    if (!aliasedQualifiedName.empty()) {
      symbolToModule_[aliasedQualifiedName] = moduleKey;
    }
  }

  // Map class methods with aliased names
  for (int i = 0; i < metadata->classes_size(); ++i) {
    const auto& cls = metadata->classes(i);
    if (cls.type_parameters_size() > 0) continue;

    std::string className;
    if (!aliasedModuleName.empty()) {
      className = prefix + "_" + aliasedModuleName + "_" + cls.name();
    } else {
      className = prefix + "_" + cls.name();
    }

    for (int j = 0; j < cls.methods_size(); ++j) {
      const auto& method = cls.methods(j);
      const auto& methodProto = method.function().proto();
      if (methodProto.type_parameters_size() > 0) continue;

      std::string mangledName = className + "_" + methodProto.name();
      symbolToModule_[mangledName] = moduleKey;
    }
  }
}

std::string ModuleLinker::remapSymbolName(
    const std::string& symbol,
    const std::unordered_map<std::string, std::string>& moduleRemap) const {
  // Symbol format: $hash$_moduleName_... or prefix_moduleName_...
  // We need to find and replace the module name portion

  std::string result = symbol;

  for (const auto& [fromModule, toModule] : moduleRemap) {
    // Look for _fromModule_ pattern (underscore-delimited module name)
    std::string fromPattern = "_" + fromModule + "_";
    std::string toPattern = "_" + toModule + "_";

    size_t pos = result.find(fromPattern);
    if (pos != std::string::npos) {
      result.replace(pos, fromPattern.size(), toPattern);
      // Only replace first occurrence (module name appears once in symbol)
      break;
    }
  }

  return result;
}

}  // namespace sun
