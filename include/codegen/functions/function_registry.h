#pragma once

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueHandle.h>

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "codegen/codegen_state.h"
#include "semantic_analysis/types.h"

/**
 * Map resolved declarations to LLVM functions and track their origins.
 * Method emission still uses symbol-based helpers during the migration.
 */
class FunctionRegistry {
 public:
  /** Share the module and declaration table used by this codegen run. */
  explicit FunctionRegistry(CodegenState& state) : state_(state) {}

  FunctionRegistry(const FunctionRegistry&) = delete;
  FunctionRegistry& operator=(const FunctionRegistry&) = delete;

  // ---------------------------------------------------------------
  // Where functions came from
  // ---------------------------------------------------------------

  // Snapshot the module's current declarations. Call after the precompiled
  // bitcode has been declared but before codegen starts.
  void snapshotPrecompiled(llvm::Module& module) {
    for (auto& f : module) {
      if (!f.getName().empty()) precompiled_.insert(f.getName().str());
    }
  }

  // True if the function was declared from precompiled bitcode rather than
  // by codegen itself
  bool isPrecompiled(const std::string& name) const {
    return precompiled_.count(name) > 0;
  }

  // Note a function as user-written, so an IR dump includes it
  void noteUserDefined(const std::string& name) { userDefined_.insert(name); }

  // The user-written function names, for filtering an IR dump
  const std::set<std::string>& userDefined() const { return userDefined_; }

  // ---------------------------------------------------------------
  // Finding functions
  // ---------------------------------------------------------------

  /** Bind a source declaration to its created or imported LLVM function. */
  void registerFunction(sun::DeclarationId declaration,
                        llvm::Function* function);

  /** Find the emitted function selected by semantic declaration resolution. */
  llvm::Function* lookupFunctionById(sun::DeclarationId declaration);

  /**
   * Finds the LLVM function for a class method. Tries the mangled name with
   * its parameter suffix first, then the plain "TypeName_methodName" form
   * that simple and legacy cases use.
   */
  llvm::Function* findClassMethod(
      const std::shared_ptr<sun::ClassType>& classType,
      const std::string& typeName, const std::string& methodName);

  /**
   * Finds a method by mangled name, declaring it as an external with the
   * closure ABI signature if the module does not have it yet. The external
   * resolves from the defining module at link or JIT time, which is how an
   * imported or precompiled class's methods are reached.
   */
  llvm::Function* getOrDeclareMethodFunction(
      const std::string& mangledName,
      const std::vector<sun::TypePtr>& paramTypes,
      const sun::TypePtr& returnType, bool canThrow);

 private:
  std::unordered_map<sun::DeclarationId, llvm::WeakTrackingVH> functionsById_;
  CodegenState& state_;

  // Declared from precompiled bitcode before codegen started
  std::set<std::string> precompiled_;

  // Written by the user, as opposed to pulled in from a library
  std::set<std::string> userDefined_;
};
