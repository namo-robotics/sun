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
#include "types/types.h"

/** Provides the registry of generated functions and their metadata. */
namespace sun::codegen::functions {
using sun::semantic_analysis::DeclarationId;

/**
 * Map resolved declarations to LLVM functions and track their origins.
 */
class FunctionRegistry {
 public:
  /** Share the module and declaration table used by this codegen run. */
  explicit FunctionRegistry(sun::codegen::CodegenState& state)
      : state_(state) {}

  /** Disallows copying so the owned state cannot be duplicated. */
  FunctionRegistry(const FunctionRegistry&) = delete;
  /** Disallows assignment so ownership and object identity cannot be duplicated. */
  FunctionRegistry& operator=(const FunctionRegistry&) = delete;

  // ---------------------------------------------------------------
  // Where functions came from
  // ---------------------------------------------------------------

  /**
   * Snapshot the module's current declarations. Call after the precompiled
   * bitcode has been declared but before codegen starts.
   */
  void snapshotPrecompiled(llvm::Module& module) {
    for (auto& f : module) {
      if (!f.getName().empty()) precompiled_.insert(f.getName().str());
    }
  }

  /**
   * True if the function was declared from precompiled bitcode rather than
   * by codegen itself
   */
  bool isPrecompiled(const std::string& name) const {
    return precompiled_.count(name) > 0;
  }

  /**
   * Note a function as user-written, so an IR dump includes it
   */
  void noteUserDefined(const std::string& name) { userDefined_.insert(name); }

  /**
   * The user-written function names, for filtering an IR dump
   */
  const std::set<std::string>& userDefined() const { return userDefined_; }

  // ---------------------------------------------------------------
  // Finding functions
  // ---------------------------------------------------------------

  /** Bind a source declaration to its created or imported LLVM function. */
  void registerFunction(DeclarationId declaration, llvm::Function* function);

  /** Find the emitted function selected by semantic declaration resolution. */
  llvm::Function* lookupFunctionById(DeclarationId declaration);

 private:
  std::unordered_map<DeclarationId, llvm::WeakTrackingVH> functionsById_;
  sun::codegen::CodegenState& state_;

  // Declared from precompiled bitcode before codegen started
  std::set<std::string> precompiled_;

  // Written by the user, as opposed to pulled in from a library
  std::set<std::string> userDefined_;
};

}  // namespace sun::codegen::functions
