#pragma once

#include <span>
#include <string>

#include "ast/prototype_ast.h"
#include "types/type.h"

/** Provides compiler-owned intrinsic declarations without emitting functions.
 */
namespace sun::codegen::intrinsics {

/** Returns the embedded source used to parse declarations and render types. */
const std::string& intrinsicSource();

/** Returns immutable signatures parsed once, independently of user programs. */
std::span<const sun::ast::PrototypeAST* const> intrinsicDeclarations();

/** Resolves primitive and pointer types used by fixed intrinsic signatures. */
sun::types::TypePtr intrinsicType(const sun::ast::TypeAnnotation& annotation);

}  // namespace sun::codegen::intrinsics
