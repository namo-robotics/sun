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

bool ModuleLinker::linkModule(const std::string& importPath) {
  return linkModuleRecursive(importPath);
}

bool ModuleLinker::linkModules(const std::vector<std::string>& importPaths) {
  for (const auto& path : importPaths) {
    if (!linkModuleRecursive(path)) {
      return false;
    }
  }
  return true;
}

void ModuleLinker::registerAvailableModules(
    const std::vector<std::string>& importPaths) {
  for (const auto& path : importPaths) {
    if (availableModules_.count(path)) {
      continue;
    }
    availableModules_.insert(path);
    buildSymbolMap(path);
  }
}

void ModuleLinker::buildSymbolMap(const std::string& importPath) {
  auto* metadata = LibraryCache::instance().getMetadata(importPath);
  if (!metadata) {
    return;
  }

  // Map exported functions to this module
  for (const auto& exp : metadata->exports) {
    if (!exp.mangledName.empty()) {
      symbolToModule_[exp.mangledName] = importPath;
    }
  }

  // Map class methods - only non-generic classes have callable methods
  // Generic class specializations are handled via codegen (not metadata)
  for (const auto& cls : metadata->classes) {
    // Skip generic classes - their methods require instantiation
    if (!cls.typeParams.empty()) continue;

    std::string className = cls.name;
    for (const auto& method : cls.methods) {
      // Skip generic methods
      if (!method.typeParams.empty()) continue;

      std::string mangledName = className + "_" + method.name;
      symbolToModule_[mangledName] = importPath;
    }
  }
}

void ModuleLinker::declareAvailableFunctions() {
  auto& ctx = target_.getContext();

  for (const auto& modulePath : availableModules_) {
    // Load the bitcode module to scan its functions directly
    // This captures all concrete functions including generic specializations
    auto libModule = LibraryCache::instance().loadModule(modulePath, ctx);
    if (!libModule) continue;

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

      // Skip if already declared in target
      if (target_.getFunction(funcName)) continue;

      // Clone the function type and remap struct types to target module's types
      // This fixes type mismatches like static_ptr_struct vs
      // static_ptr_struct.19
      llvm::FunctionType* funcType =
          remapFunctionType(func.getFunctionType(), ctx);

      // Create external declaration with the remapped signature
      llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                             funcName, &target_);

      // Also add to symbol map for linking
      symbolToModule_[funcName] = modulePath;
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

bool ModuleLinker::linkModuleRecursive(const std::string& importPath) {
  // Already linked?
  if (linkedModules_.count(importPath)) {
    return true;
  }

  // Get metadata to find dependencies
  auto* metadata = LibraryCache::instance().getMetadata(importPath);
  if (!metadata) {
    error_ = "Module not found in library cache: " + importPath;
    return false;
  }

  // Link dependencies first
  for (const auto& dep : metadata->dependencies) {
    if (!linkModuleRecursive(dep)) {
      return false;
    }
  }

  // Load the module bitcode
  auto libModule =
      LibraryCache::instance().loadModule(importPath, target_.getContext());
  if (!libModule) {
    // Get detailed error from the reader
    auto* bundle = LibraryCache::instance().findBundleForModule(importPath);
    if (bundle) {
      error_ = "Failed to load bitcode for: " + importPath + " - " +
               bundle->getError();
    } else {
      error_ =
          "Failed to load bitcode for: " + importPath + " (bundle not found)";
    }
    return false;
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
    error_ = "LLVM linker failed for: " + importPath;
    return false;
  }

  linkedModules_.insert(importPath);
  return true;
}

}  // namespace sun
