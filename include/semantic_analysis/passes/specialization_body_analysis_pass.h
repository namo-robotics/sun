#pragma once

/** Resolves declarations and checks the meaning of Sun programs. */
namespace sun::semantic_analysis {
class SemanticAnalyzer;

/** Provides the ordered preparation and registration passes for analysis. */
namespace passes {
/** Check all queued specialization bodies after declarations are ready. */
class SpecializationBodyAnalysisPass {
 public:
  /** Borrow the analyzer; its specializer is accessed only when running. */
  explicit SpecializationBodyAnalysisPass(SemanticAnalyzer& analyzer)
      : analyzer_(analyzer) {}
  /** Drain queued bodies, including specializations requested by those bodies.
   */
  void run();

 private:
  SemanticAnalyzer& analyzer_;
};
}  // namespace passes
}  // namespace sun::semantic_analysis
