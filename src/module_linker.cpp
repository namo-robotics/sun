#include "module_linker.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/Linker/Linker.h>

#include "module_types.h"
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

  // Map exported functions to this module
  for (const auto& exp : metadata->exports) {
    if (!exp.qualifiedName.empty()) {
      symbolToModule_[exp.qualifiedName] = moduleKey;
    }
  }

  // Map class methods - only non-generic classes have callable methods
  // Generic class specializations are handled via codegen (not metadata)
  for (const auto& cls : metadata->classes) {
    // Skip generic classes - their methods require instantiation
    if (!cls.typeParams.empty()) continue;

    std::string className = cls.qualifiedName;
    for (const auto& method : cls.methods) {
      // Skip generic methods
      if (!method.typeParams.empty()) continue;

      std::string mangledName = className + "_" + method.name;
      symbolToModule_[mangledName] = moduleKey;
    }
  }
}

void ModuleLinker::declareAvailableFunctions() {
  auto& ctx = target_.getContext();

  for (const auto& moduleKey : availableModules_) {
    // Get metadata to retrieve content hash for symbol prefixing
    auto* metadata = LibraryCache::instance().getMetadata(moduleKey);
    std::string prefix = metadata ? metadata->getSymbolPrefix() : "";

    // Load the bitcode module to scan its functions directly
    // This captures all concrete functions including generic specializations
    auto libModule = LibraryCache::instance().loadModule(moduleKey, ctx);
    if (!libModule) continue;

    // Scan all defined functions in the bitcode and create declarations
    for (const auto& func : libModule->functions()) {
      // Skip declarations (external functions the lib depends on)
      if (func.isDeclaration()) continue;
      // Skip LLVM intrinsics
      if (func.isIntrinsic()) continue;
      // Skip unnamed functions
      if (!func.hasName() || func.getName().empty()) continue;

      std::string originalName = func.getName().str();

      // Skip internal helper functions (start with underscore or llvm.)
      if (originalName[0] == '_' && originalName.size() > 1 &&
          originalName[1] == '_') {
        continue;  // Skip __sun_* helper functions
      }

      // Apply content hash prefix for symbol isolation
      // Use underscore separator: $hash$ + sun_foo -> $hash$_sun_foo
      std::string prefixedName =
          prefix.empty() ? originalName : prefix + "_" + originalName;

      // Skip if already declared in target (with either name)
      if (target_.getFunction(prefixedName)) continue;
      if (target_.getFunction(originalName)) continue;

      // Clone the function type and remap struct types to target module's types
      // This fixes type mismatches like static_ptr_struct vs
      // static_ptr_struct.19
      llvm::FunctionType* funcType =
          remapFunctionType(func.getFunctionType(), ctx);

      // Create external declaration with prefixed name
      llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                             prefixedName, &target_);

      // Map prefixed name to module for linking
      symbolToModule_[prefixedName] = moduleKey;
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

  // Get the symbol prefix for this module
  std::string prefix = metadata->getSymbolPrefix();

  // Note: We do NOT recursively load dependencies here.
  // Moon files are self-contained - when a.moon was created from a.sun
  // (which imported b_v1.moon), the bitcode from b_v1 was already linked
  // into a.moon. The deps list in metadata is for informational purposes
  // only (tracking what the module depends on), not for runtime loading.

  // Load the module bitcode
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

  // Ensure the library module has same data layout as target
  // This avoids "Linking two modules of different data layouts" warnings
  if (libModule->getDataLayoutStr().empty()) {
    libModule->setDataLayout(target_.getDataLayout());
  }

  // Apply content hash prefix to all symbols for isolation
  // This prevents symbol conflicts when multiple versions of a dependency are
  // linked
  if (!prefix.empty()) {
    // Known external C runtime functions that should NOT be prefixed
    static const std::set<std::string> cRuntimeFunctions = {
        "malloc",    "free",     "realloc", "calloc",  "memset",  "memcpy",
        "memmove",   "memcmp",   "strlen",  "strcpy",  "strncpy", "strcmp",
        "strncmp",   "strcat",   "strncat", "printf",  "fprintf", "sprintf",
        "snprintf",  "puts",     "putchar", "getchar", "fopen",   "fclose",
        "fread",     "fwrite",   "fseek",   "ftell",   "fflush",  "exit",
        "abort",     "atexit",   "atoi",    "atof",    "atol",    "strtol",
        "strtod",    "qsort",    "bsearch", "rand",    "srand",   "time",
        "clock",     "difftime", "mktime",  "asctime", "ctime",   "gmtime",
        "localtime", "strftime", "sin",     "cos",     "tan",     "asin",
        "acos",      "atan",     "atan2",   "sinh",    "cosh",    "tanh",
        "exp",       "log",      "log10",   "pow",     "sqrt",    "ceil",
        "floor",     "fabs",     "fmod",    "frexp",   "ldexp",   "modf"};

    auto shouldSkipRename = [&](const std::string& name) {
      // Skip LLVM intrinsics
      if (name.starts_with("llvm.")) return true;
      // Skip already-prefixed symbols
      if (name.starts_with("$")) return true;
      // Skip known C runtime functions
      if (cRuntimeFunctions.count(name)) return true;
      // Skip functions starting with underscore (C runtime convention)
      if (!name.empty() && name[0] == '_') return true;
      return false;
    };

    // Rename all functions (definitions AND declarations for cross-module refs)
    // Skip only C runtime functions
    for (auto& func : libModule->functions()) {
      if (!func.hasName() || func.getName().empty()) continue;
      if (func.isIntrinsic()) continue;
      std::string originalName = func.getName().str();
      if (!shouldSkipRename(originalName)) {
        func.setName(prefix + "_" + originalName);
      }
    }

    // Rename all global variables (but not C runtime globals)
    for (auto& global : libModule->globals()) {
      if (!global.hasName() || global.getName().empty()) continue;
      std::string originalName = global.getName().str();
      if (!shouldSkipRename(originalName)) {
        global.setName(prefix + "_" + originalName);
      }
    }
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

}  // namespace sun
