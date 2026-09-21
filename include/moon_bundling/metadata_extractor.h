#pragma once

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
class BlockExprAST;
}
/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
class SemanticAnalyzer;
}

#include <optional>
#include <string>
#include <vector>

#include "moon.pb.h"

/** Builds and loads compiled Moon libraries and their declaration metadata. */
namespace sun::moon_bundling {}
/** Builds and loads compiled Moon libraries and their declaration metadata. */
namespace sun::moon_bundling {}

/** Builds and loads compiled Moon libraries and their declaration metadata. */
namespace sun::moon_bundling {

/** Export the bundle's own declarations from the successfully analyzed program.
 */
std::vector<moon::ModuleMetadata> extractAnalyzedMetadata(
    const sun::ast::BlockExprAST& program,
    sun::semantic_analysis::SemanticAnalyzer& analyzer,
    const std::string& bundleHash);

}  // namespace sun::moon_bundling
