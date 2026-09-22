#include "semantic_analysis/passes/specialization_body_analysis_pass.h"

#include "semantic_analysis/semantic_analyzer.h"

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {
void SpecializationBodyAnalysisPass::run() {
  analyzer_.generics().analyzePendingBodies();
}
}  // namespace sun::semantic_analysis::passes
