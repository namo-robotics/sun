#pragma once

// function_registry.h — Which functions exist, and how to call them
//
// Codegen keeps two books on functions, both plain bookkeeping over
// the module being built:
//
//   which functions came from precompiled bitcode  — so a declaration made by
//       codegen is never mistaken for one a .moon bundle already supplied
//   which functions the user wrote  — so an IR dump can leave library code out
//
// It also owns the lookups that answer "what LLVM function does this name
// mean", including the two cases where the answer is not simply the name: a
// renamed extern is declared under its C symbol, and a method not yet emitted
// is declared on demand with the closure ABI.

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "codegen/abi/extern_c.h"
#include "codegen/codegen_state.h"
#include "semantic_analysis/types.h"

/**
 * The registry of functions in the module being built: their calling
 * conventions, where they came from, and how to find them by name.
 */
class FunctionRegistry {
 public:
  FunctionRegistry(CodegenState& state, sun::cabi::ExternCEmitter& externC)
      : state_(state), externC_(externC) {}

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

  /**
   * Finds a function by its resolved Sun-side name, translating a renamed
   * extern (`as "symbol"`) to the C symbol it was declared under.
   */
  llvm::Function* lookupCallTarget(const std::string& name);

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
  CodegenState& state_;
  sun::cabi::ExternCEmitter& externC_;

  // Declared from precompiled bitcode before codegen started
  std::set<std::string> precompiled_;

  // Written by the user, as opposed to pulled in from a library
  std::set<std::string> userDefined_;
};
