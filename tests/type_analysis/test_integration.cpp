/** Exercises semantic preparation around the pure inference boundary. */
#include <gtest/gtest.h>

#include <sstream>

#include "ast/ast_children.h"
#include "driver/execution_utils.h"
#include "parsing/parser.h"
#include "semantic_analysis/analysis_results.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "serialization/ast_deserializer.h"
#include "serialization/ast_serializer.h"

/** Calls retain the return type of the selected overload, including modules. */
TEST(TypeAnalysis_Integration, SelectedOverloadAndGenericCall) {
  EXPECT_EQ(sun::driver::executeString(R"(
    /** Supplies overloaded and generic call targets. */
    public module values {
      /** Return the narrow overload result. */
      public function choose(x: i32) i32 { return 3; }
      /** Return the wide overload result. */
      public function choose(x: bool) i64 { return 42; }
      /** Preserve the argument type and value. */
      public function identity<T>(x: T) T { return x; }
    }
    /** Exercise the prepared types and return the result. */
    function main() i64 {
      return values.identity(values.choose(true));
    }
  )"),
            42);
}

/** Expected types reach literals while unsafe bodies retain their checked
 * result. */
TEST(TypeAnalysis_Integration, ContextualLiteralsAndUnsafeResult) {
  EXPECT_EQ(sun::driver::executeString(R"(
    /** Exercise the prepared types and return the result. */
    function main() i32 {
      var narrow: u8 = 7;
      var result: u8 = true ? narrow : 9;
      var from_block = unsafe { var local: i32 = 35; local; };
      return result + from_block;
    }
  )"),
            42);
}

/** Lambda result typing preserves capture metadata and parameter types. */
TEST(TypeAnalysis_Integration, LambdaCaptureResult) {
  EXPECT_EQ(sun::driver::executeString(R"(
    /** Exercise the prepared types and return the result. */
    function main() i32 {
      var value: i32 = 40;
      var read = [const ref value](extra: i32) => i32 { return value + extra; };
      return read(2);
    }
  )"),
            42);
}

/** Helpers for checking semantic results across a syntax round trip. */
namespace {
/** Collect ordinary calls in source traversal order. */
void collectCalls(const sun::ast::ExprAST& node,
                  std::vector<const sun::ast::CallExprAST*>& calls) {
  if (node.getType() == sun::ast::ASTNodeType::CALL)
    calls.push_back(&static_cast<const sun::ast::CallExprAST&>(node));
  sun::ast::forEachChild(node, [&](const sun::ast::ExprAST& child) {
    collectCalls(child, calls);
  });
}
}  // namespace

/** Reanalysis restores the same selected declarations and numeric conversions.
 */
TEST(TypeAnalysis_Integration, CallFactsAfterSyntaxRoundTrip) {
  using namespace sun::semantic_analysis;
  using namespace sun::types;
  std::istringstream input;
  sun::parsing::Parser parser(input);
  auto original = parser.parseString(R"(
    /** Provide an overloaded call target. */
    function choose(x: i32) i32 { return x; }
    /** Provide an overloaded call target. */
    function choose(x: bool) i64 { return 42; }
    /** Accept a widened integer argument. */
    function widen(x: i64) i64 { return x; }
    /** Exercise the prepared types and return the result. */
    function main() i64 {
      var small: i16 = 7;
      var wide = widen(small);
      return choose(true);
    }
  )");
  auto registryResults = std::make_shared<AnalysisResults>();
  auto registry = registryResults->types;
  SemanticAnalyzer analyzer(registryResults);
  analyzer.pipeline().run(*original);
  std::vector<const sun::ast::CallExprAST*> before;
  collectCalls(*original, before);
  ASSERT_EQ(before.size(), 2u);
  ASSERT_EQ(before[0]->getArgConversions().size(), 1u);
  EXPECT_EQ(before[0]->getArgConversions()[0], ArgConversion::WidenNumeric);
  EXPECT_TRUE(before[1]->getResolvedType()->isInt64());
  const auto& selected =
      static_cast<const sun::ast::FunctionAST&>(*original->getBody()[1]);
  EXPECT_EQ(before[1]->getCallee()->getTargetDeclarationId(),
            selected.getDeclarationId());

  // Serialization stores syntax; computed call facts are rebuilt by analysis.
  sun::serialization::ASTSerializer serializer;
  sun::serialization::ASTDeserializer deserializer;
  auto restored =
      deserializer.deserializeProgram(serializer.serializeProgram(*original));
  auto otherRegistryResults = std::make_shared<AnalysisResults>();
  auto otherRegistry = otherRegistryResults->types;
  SemanticAnalyzer other(otherRegistryResults);
  other.pipeline().run(*restored);
  std::vector<const sun::ast::CallExprAST*> after;
  collectCalls(*restored, after);
  ASSERT_EQ(after.size(), before.size());
  const auto& restoredSelected =
      static_cast<const sun::ast::FunctionAST&>(*restored->getBody()[1]);
  EXPECT_EQ(after[1]->getCallee()->getTargetDeclarationId(),
            restoredSelected.getDeclarationId());
  for (size_t i = 0; i < after.size(); ++i) {
    EXPECT_EQ(after[i]->getArgConversions(), before[i]->getArgConversions());
    EXPECT_TRUE(
        after[i]->getResolvedType()->equals(*before[i]->getResolvedType()));
  }
}

/** A variadic generic method records its concrete signature before result
 * typing. */
TEST(TypeAnalysis_Integration, VariadicMethodSelectedSignature) {
  EXPECT_EQ(sun::driver::executeString(R"(
    /** Supplies a variadic generic method. */
    class Factory {
      /** Preserve the first argument across a variadic call. */
      public method first<T>(value: T, args...) T { return value; }
    }
    /** Exercise the prepared types and return the result. */
    function main() i64 {
      var factory = Factory();
      return factory.first<i64>(42, true);
    }
  )"),
            42);
}
