#pragma once

#include <llvm/IR/Module.h>

#include <string>
#include <vector>

#include "moon.pb.h"

/** Describes the declarations and retained bodies interpreted by an importer.
 */
namespace sun::abi {
/** Canonically encodes semantic metadata, excluding diagnostic and container
 * fields. */
std::string describeSemantics(const moon::ModuleMetadata& metadata);
/** Checks declaration ownership and retained references against bundle and
 * compiler-provided identities before import. Complete import sets also reject
 * unresolved external references and allow identical embedded records. */
void validateDeclarationGraph(
    const std::vector<const moon::ModuleMetadata*>& modules,
    bool completeImportSet = false);
/** Reconstructs concrete source signatures and compares them with compiled
 * code. */
void validatePayloadDeclarations(
    const std::vector<const moon::ModuleMetadata*>& modules,
    const llvm::Module& module);
/** Checks that advertised precompiled specialization methods have definitions.
 */
void validateSpecializationPayload(const moon::ModuleMetadata& metadata,
                                   const llvm::Module& module);
}  // namespace sun::abi
