// analysis_results.h — What analysis concludes about a whole program.

#pragma once

#include <memory>

#include "semantic_analysis/constants/global_init_table.h"
#include "semantic_analysis/declaration_table.h"
#include "semantic_analysis/type_registry.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/**
 * Everything analysis concludes about a program that does not belong to a
 * single syntax node. Per-node conclusions, such as an expression's type, are
 * stored on the nodes themselves (see ast/analysis.h); this holds the rest.
 *
 * One is created per compilation and shared by the analyzer, which fills it
 * in, and code generation, which reads it. It is complete once the analysis
 * pipeline has run.
 */
struct AnalysisResults {
  // Who is declared: the identity of every declaration in the program.
  // Declared before `types`, which is built on top of it.
  DeclarationTable declarations;

  // What types exist: declared classes, interfaces and enums, and the
  // specializations of generic ones. Shared, because a nominal type can
  // outlive the code that looked it up.
  std::shared_ptr<TypeRegistry> types =
      std::make_shared<TypeRegistry>(declarations);

  // How each file-scope variable gets its first value: computed at compile
  // time and written into the program, or computed by the startup function.
  constants::GlobalInitTable globalInits;

  /** Starts with no declarations beyond the builtin ones. */
  AnalysisResults() = default;
  /** The type registry refers to `declarations`, so the parts stay together. */
  AnalysisResults(const AnalysisResults&) = delete;
  /** The type registry refers to `declarations`, so the parts stay together. */
  AnalysisResults& operator=(const AnalysisResults&) = delete;
};

}  // namespace sun::semantic_analysis
