#include "moon_bundling/module_linker.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/Linker/Linker.h>

#include "moon_bundling/library_cache.h"
#include "moon_bundling/module_types.h"
#include "moon_bundling/moon.h"
#include "semantic_analysis/struct_names.h"

namespace sun {

namespace {

/// The canonical name a struct from a scanned module should unify under in
/// the target: well-known runtime structs map to their unsuffixed name, and a
/// trailing ".N" that LLVM added to keep same-named types apart is stripped
/// (codegen mints the unsuffixed one from the Sun type).
std::string canonicalStructName(llvm::StringRef name) {
  for (const auto& info : sun::StructNames::All) {
    if (name.starts_with(info.name)) return info.name;
  }
  size_t dot = name.rfind('.');
  if (dot != llvm::StringRef::npos && dot + 1 < name.size() &&
      name.substr(dot + 1).find_first_not_of("0123456789") ==
          llvm::StringRef::npos) {
    return name.substr(0, dot).str();
  }
  return name.str();
}

/// Map a type from a scanned module to the target context. Scanned modules
/// live in their own LLVMContext, so every type is rebuilt: named structs
/// unify by canonical name, everything else structurally. `visited` carries
/// in-progress structs so recursive types terminate.
llvm::Type* mapTypeToTarget(
    llvm::Type* srcType, llvm::LLVMContext& ctx,
    std::unordered_map<llvm::Type*, llvm::Type*>& visited) {
  if (!srcType) return nullptr;
  auto it = visited.find(srcType);
  if (it != visited.end()) return it->second;

  llvm::Type* mapped = nullptr;
  if (auto* structTy = llvm::dyn_cast<llvm::StructType>(srcType)) {
    if (structTy->isLiteral()) {
      llvm::SmallVector<llvm::Type*, 8> elems;
      for (llvm::Type* elem : structTy->elements()) {
        elems.push_back(mapTypeToTarget(elem, ctx, visited));
      }
      mapped = llvm::StructType::get(ctx, elems, structTy->isPacked());
      visited[srcType] = mapped;
      return mapped;
    }
    // Identified struct: unify by canonical name; create-then-fill so a
    // recursive body can refer back to the struct being built.
    llvm::StructType* dst = nullptr;
    if (structTy->hasName()) {
      std::string name = canonicalStructName(structTy->getName());
      dst = llvm::StructType::getTypeByName(ctx, name);
      if (!dst) dst = llvm::StructType::create(ctx, name);
    } else {
      dst = llvm::StructType::create(ctx);
    }
    visited[srcType] = dst;
    if (dst->isOpaque() && !structTy->isOpaque()) {
      llvm::SmallVector<llvm::Type*, 8> elems;
      for (llvm::Type* elem : structTy->elements()) {
        elems.push_back(mapTypeToTarget(elem, ctx, visited));
      }
      dst->setBody(elems, structTy->isPacked());
    }
    return dst;
  }
  if (auto* intTy = llvm::dyn_cast<llvm::IntegerType>(srcType)) {
    mapped = llvm::IntegerType::get(ctx, intTy->getBitWidth());
  } else if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcType)) {
    mapped = llvm::PointerType::get(ctx, ptrTy->getAddressSpace());
  } else if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(srcType)) {
    mapped = llvm::ArrayType::get(
        mapTypeToTarget(arrTy->getElementType(), ctx, visited),
        arrTy->getNumElements());
  } else if (auto* vecTy = llvm::dyn_cast<llvm::VectorType>(srcType)) {
    mapped = llvm::VectorType::get(
        mapTypeToTarget(vecTy->getElementType(), ctx, visited),
        vecTy->getElementCount());
  } else if (auto* fnTy = llvm::dyn_cast<llvm::FunctionType>(srcType)) {
    llvm::SmallVector<llvm::Type*, 8> params;
    for (llvm::Type* p : fnTy->params()) {
      params.push_back(mapTypeToTarget(p, ctx, visited));
    }
    mapped = llvm::FunctionType::get(
        mapTypeToTarget(fnTy->getReturnType(), ctx, visited), params,
        fnTy->isVarArg());
  } else if (srcType->isVoidTy()) {
    mapped = llvm::Type::getVoidTy(ctx);
  } else if (srcType->isHalfTy()) {
    mapped = llvm::Type::getHalfTy(ctx);
  } else if (srcType->isBFloatTy()) {
    mapped = llvm::Type::getBFloatTy(ctx);
  } else if (srcType->isFloatTy()) {
    mapped = llvm::Type::getFloatTy(ctx);
  } else if (srcType->isDoubleTy()) {
    mapped = llvm::Type::getDoubleTy(ctx);
  } else if (srcType->isFP128Ty()) {
    mapped = llvm::Type::getFP128Ty(ctx);
  } else {
    // Only reached for exotic types that never appear in Sun signatures
    mapped = srcType;
  }
  visited[srcType] = mapped;
  return mapped;
}

/// Create a function type in the target context with mapped parameter and
/// return types
llvm::FunctionType* remapFunctionType(
    llvm::FunctionType* srcFuncType, llvm::LLVMContext& ctx,
    std::unordered_map<llvm::Type*, llvm::Type*>& visited) {
  return llvm::cast<llvm::FunctionType>(
      mapTypeToTarget(srcFuncType, ctx, visited));
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

  // Scanned modules get their own context: sharing struct types between the
  // target and a future link source corrupts llvm::Linker's type mapping (it
  // strips names off types the target's own definitions still use).
  if (!scanContext_) scanContext_ = std::make_unique<llvm::LLVMContext>();

  // Every module of a bundle normally points at one shared code image, so
  // scanning per module key would parse the same bitcode once per module.
  // Scan each (code image, aliasing) pair once instead; a repeat scan can only
  // re-derive declarations the first one already made.
  std::set<std::string> scanned;

  for (const auto& moduleKey : availableModules_) {
    // Check if this module has aliasing configured
    auto remapIt = moduleRemaps_.find(moduleKey);
    bool hasRemap =
        (remapIt != moduleRemaps_.end() && !remapIt->second.empty());
    const auto* remap = hasRemap ? &remapIt->second : nullptr;

    std::string bitcodeId = LibraryCache::instance().getBitcodeId(moduleKey);
    if (!bitcodeId.empty()) {
      std::string scanKey = bitcodeId;
      if (remap) {
        for (const auto& [from, to] :
             std::map<std::string, std::string>(remap->begin(), remap->end())) {
          scanKey += "|" + from + "=" + to;
        }
      }
      if (!scanned.insert(scanKey).second) continue;
    }

    // Load the bitcode module to scan its functions directly
    // This captures all concrete functions including generic specializations
    // NOTE: Symbols in the bitcode are ALREADY prefixed with the content hash
    // (done at moon bundle creation time), so we don't add prefixes here.
    auto owned = LibraryCache::instance().loadModule(moduleKey, *scanContext_);
    if (!owned) continue;

    llvm::Module* libModule = owned.get();
    // Cache for cross-context type mapping, shared by every declaration made
    // from this code image
    std::unordered_map<llvm::Type*, llvm::Type*> typeMap;

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

      // Rebuild the function type in the target context, unifying struct
      // types by canonical name (static_ptr_struct.19 -> static_ptr_struct)
      llvm::FunctionType* funcType =
          remapFunctionType(func.getFunctionType(), ctx, typeMap);

      // Create external declaration with the (potentially aliased) name
      llvm::Function* decl = llvm::Function::Create(
          funcType, llvm::Function::ExternalLinkage, declaredName, &target_);
      // Callers pick call vs invoke from this tag; keep it on the declaration
      if (func.hasFnAttribute("sun.canthrow")) {
        decl->addFnAttr("sun.canthrow");
      }
      // Keeps a C extern exempt from symbol prefixing if this module is
      // itself bundled.
      if (func.hasFnAttribute("sun.cabi")) {
        decl->addFnAttr("sun.cabi");
      }

      // Map the aliased name to the module for linking
      symbolToModule_[declaredName] = moduleKey;
    }

    // Same for module-level variables the bitcode defines: the importer holds
    // a declaration, so the defining module has to be pulled in.
    for (const auto& global : libModule->globals()) {
      if (!global.hasInitializer())
        continue;  // a declaration, not a definition
      if (!global.hasName() || global.getName().empty()) continue;

      std::string globalName = global.getName().str();
      if (globalName[0] == '_') continue;  // internal helpers and literals

      std::string declaredName = globalName;
      if (remap) {
        declaredName = remapSymbolName(globalName, *remap);
      }

      symbolToModule_[declaredName] = moduleKey;
    }
  }
}

bool ModuleLinker::linkOnlyUsedSymbols() {
  std::set<std::string> neededModules;

  // Find undefined symbols in target module that we can provide.
  // Only declarations with actual uses count — declareAvailableFunctions
  // blanket-declares every library function, so an unused declaration must
  // not pull in its module.
  for (const auto& F : target_) {
    if (F.isDeclaration() && !F.isIntrinsic() && !F.getName().empty() &&
        !F.use_empty()) {
      std::string name = F.getName().str();
      auto it = symbolToModule_.find(name);
      if (it != symbolToModule_.end()) {
        neededModules.insert(it->second);
      }
    }
  }

  // Also check for undefined globals
  for (const auto& G : target_.globals()) {
    if (G.isDeclaration() && !G.getName().empty() && !G.use_empty()) {
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

  // A bundle compiled for one target must not be linked into another: struct
  // layouts and ABI decisions are baked into its bitcode. (Old bundles carry
  // no triple; the format version bump retires those.)
  if (!metadata->target_triple().empty() &&
      !target_.getTargetTriple().empty()) {
    llvm::Triple bundleTriple(metadata->target_triple());
    llvm::Triple targetTriple(target_.getTargetTriple());
    if (bundleTriple.getArch() != targetTriple.getArch() ||
        !sameOsFamily(bundleTriple, targetTriple)) {
      error_ = "Module '" + moduleKey + "' was compiled for '" +
               metadata->target_triple() + "' but the current target is '" +
               target_.getTargetTriple() +
               "'; rebuild the .moon with --target " + targetTriple.str();
      return false;
    }
  }

  // Check content hash for deduplication
  // If we've already linked bitcode with this hash, skip it
  std::string contentHash = sun::getSymbolPrefix(*metadata);
  if (!contentHash.empty() && linkedContentHashes_.count(contentHash)) {
    // Mark as linked (for moduleKey tracking) but don't link bitcode again
    linkedModules_.insert(moduleKey);
    return true;
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
  // Parse a fresh copy into the target's context. The copy scanned by
  // declareAvailableFunctions() lives in a separate context on purpose: the
  // link source must not share struct type objects with the target, or
  // llvm::Linker's type mapper mangles the types the target already uses.
  std::unique_ptr<llvm::Module> libModule =
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

  // Ensure the library module has same data layout and triple as target
  // This avoids "Linking two modules of different data layouts" warnings
  if (libModule->getDataLayoutStr().empty()) {
    libModule->setDataLayout(target_.getDataLayout());
  }
  if (libModule->getTargetTriple().empty()) {
    libModule->setTargetTriple(target_.getTargetTriple());
  }

  // Link into target
  // Using Linker::linkModules with OverrideFromSrc to handle duplicate symbols
  if (llvm::Linker::linkModules(target_, std::move(libModule),
                                llvm::Linker::Flags::OverrideFromSrc)) {
    error_ = "LLVM linker failed for: " + moduleKey;
    return false;
  }

  linkedModules_.insert(moduleKey);
  // Record content hash to avoid linking same bitcode via different module keys
  if (!contentHash.empty()) {
    linkedContentHashes_.insert(contentHash);
  }
  return true;
}

void ModuleLinker::registerAvailableModulesWithRemap(
    const MoonImport& moonImport) {
  // Open the moon file
  auto reader = MoonReader::open(moonImport.path);
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
