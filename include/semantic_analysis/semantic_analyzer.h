/**
 * Owns a semantic analysis session and its cooperating analyzers.
 * The pipeline orders declaration preparation and body analysis. The single
 * node entry point establishes source context and dispatches to declaration,
 * statement, or expression checking; specialized analyzers own the rules.
 */

#pragma once

/** Provides shared diagnostics, source tracking, and compiler utilities. */
namespace sun::support {
struct Position;
}

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/type_annotation.h"
#include "semantic_analysis/access_checker.h"
#include "semantic_analysis/body_analysis/body_analyzer.h"
#include "semantic_analysis/call_analysis/call_analyzer.h"
#include "semantic_analysis/class_analysis/class_analyzer.h"
#include "semantic_analysis/declaration_analysis/declaration_analyzer.h"
#include "semantic_analysis/enum_analysis/enum_analyzer.h"
#include "semantic_analysis/expression_analysis/expression_analyzer.h"
#include "semantic_analysis/generic_analysis/generic_specializer.h"
#include "semantic_analysis/inference_result.h"
#include "semantic_analysis/interface_analysis/interface_analyzer.h"
#include "semantic_analysis/passes/declaration_collection_pass.h"
#include "semantic_analysis/semantic_context.h"
#include "semantic_analysis/semantic_pipeline.h"
#include "semantic_analysis/semantic_scope.h"
#include "semantic_analysis/type_analysis/type_resolver.h"
#include "semantic_analysis/type_registry.h"

// Forward declarations
/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::ast::ClassDefinitionAST;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::ast::LambdaAST;
using sun::ast::PrototypeAST;

}  // namespace sun::semantic_analysis

/** Resolves names and types and checks program semantics. */
namespace sun::semantic_analysis {

/**
 * Alias for use in this header and semantic analyzer implementations
 */
using QualifiedName = sun::semantic_analysis::QualifiedName;

/**
 * Own the semantic context, pipeline, and expression-checking helpers.
 * The pipeline owns the passes, which share this session's state and helpers.
 */
class SemanticAnalyzer {
  // Scopes, symbol tables, the type registry and the current class. Shared by
  // reference with everything else this analysis run is made of.
  SemanticContext ctx_;

  // One persistent pipeline owns all passes for this analysis session.
  sun::semantic_analysis::SemanticPipeline pipeline_{*this};

  // Recursively checks statements and manages function scopes.
  BodyAnalyzer bodies_{ctx_, *this};

  // Builds and caches every specialization the program asks for.
  GenericSpecializer generics_{ctx_, *this};

  // Resolve written types and scoped substitutions, requesting specializations
  // as needed.
  type_analysis::TypeResolver resolver_{ctx_, generics_, *this};

  // Checks enum definitions, variant construction, and match patterns.
  EnumAnalyzer enums_{ctx_, *this, generics_, resolver_};

  // Resolves interface contracts and checks their implementations.
  InterfaceAnalyzer interfaces_{ctx_, *this, generics_, resolver_};

  DeclarationAnalyzer declarations_{ctx_, *this};
  ClassAnalyzer classes_{ctx_, *this};
  ExpressionAnalyzer expressions_{ctx_, *this};

  // Resolves and checks every form of call.
  CallAnalyzer calls_{ctx_, *this, generics_, resolver_};

 public:
  /** Create the shared context, checking helpers, and pipeline for a program.
   */
  explicit SemanticAnalyzer(
      std::shared_ptr<sun::semantic_analysis::AnalysisResults> results)
      : ctx_(std::move(results)) {}

  /** Keep pass and helper references tied to this session. */
  SemanticAnalyzer(const SemanticAnalyzer &) = delete;
  /** Disallows assignment so ownership and object identity cannot be
   * duplicated. */
  SemanticAnalyzer &operator=(const SemanticAnalyzer &) = delete;

  /** Scopes, symbol tables and the type registry of this analysis run. */
  SemanticContext &context() { return ctx_; }

  /** The persistent pipeline that owns and orders this session's passes. */
  sun::semantic_analysis::SemanticPipeline &pipeline() { return pipeline_; }

  /** Recursive statement and function-body checking. */
  BodyAnalyzer &bodies() { return bodies_; }

  /** Monomorphization: the specializations this run has built. */
  GenericSpecializer &generics() { return generics_; }

  /** Type-annotation resolution and scoped substitutions. */
  type_analysis::TypeResolver &typeResolver() { return resolver_; }

  /** Enum definitions, variant construction, and match patterns. */
  EnumAnalyzer &enums() { return enums_; }

  /** Interface contracts, default methods, and class conformance. */
  InterfaceAnalyzer &interfaces() { return interfaces_; }

  /** Declaration checks, signatures, and global initializer dependencies. */
  DeclarationAnalyzer &declarations() { return declarations_; }
  /** Class definitions, partial classes, and packed layout restrictions. */
  ClassAnalyzer &classes() { return classes_; }
  /** Expression checking, result types, conversions, and value access. */
  ExpressionAnalyzer &expressions() { return expressions_; }
  /** Call resolution and checking. */
  CallAnalyzer &calls() { return calls_; }

  /** The global scope, for debugging and visualization. */
  const SemanticScope &getRootScope() const { return ctx_.rootScope(); }

  /**
   * Check an expression after declaration passes have run. Resolve its type
   * and record what codegen
   * needs. expectedType is an optional hint from the context, such as the
   * declared type of the variable being assigned.
   */
  void analyzeExpr(sun::ast::ExprAST &expr,
                   sun::types::TypePtr expectedType = nullptr);
};
}  // namespace sun::semantic_analysis
