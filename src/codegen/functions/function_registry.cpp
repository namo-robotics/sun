// Function declarations map directly to their LLVM functions.

#include "codegen/functions/function_registry.h"

using sun::support::logAndThrowError;

using namespace llvm;

/** Provides the registry of generated functions and their metadata. */
namespace sun::codegen::functions {

void FunctionRegistry::registerFunction(
    sun::semantic_analysis::DeclarationId declaration, Function* function) {
  const auto& record = state_.analysis->declarations.get(declaration);
  if (record.kind != sun::semantic_analysis::DeclarationKind::Function &&
      record.kind != sun::semantic_analysis::DeclarationKind::Lambda)
    logAndThrowError("Function registration requires a callable declaration");
  if (!function) logAndThrowError("Cannot register a missing LLVM function");
  auto& existing = functionsById_[declaration];
  if (existing && existing != function)
    logAndThrowError("Declaration already has a different LLVM function");
  existing = function;
}

Function* FunctionRegistry::lookupFunctionById(
    sun::semantic_analysis::DeclarationId declaration) {
  auto found = functionsById_.find(declaration);
  if (found == functionsById_.end() || !found->second)
    logAndThrowError("Resolved function declaration has not been emitted");
  return llvm::cast<Function>(static_cast<llvm::Value*>(found->second));
}

}  // namespace sun::codegen::functions
