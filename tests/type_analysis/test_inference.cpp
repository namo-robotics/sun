/** Tests pure type computations without constructing a semantic session. */
#include <gtest/gtest.h>

#include "semantic_analysis/type_analysis/generic_type_arguments.h"

using namespace sun::semantic_analysis::type_analysis;
using namespace sun::semantic_analysis;
using namespace sun::types;

/** Helpers for comparing inference values and failures. */
namespace {
/** Require a successful result and return its type for further assertions. */
TypePtr computed(TypeResult result) {
  auto* type = std::get_if<TypePtr>(&result);
  EXPECT_NE(type, nullptr);
  return type ? *type : nullptr;
}
/** Check a structured failure without invoking diagnostic machinery. */
void fails(TypeResult result, InferenceFailure::Kind kind) {
  auto* failure = std::get_if<InferenceFailure>(&result);
  ASSERT_NE(failure, nullptr);
  EXPECT_EQ(failure->kind, kind);
}
}  // namespace

/** Literal defaults preserve signed boundaries and explicit suffix types. */
TEST(TypeAnalysis, NumericLiteralBoundaries) {
  EXPECT_EQ(computed(TypeInferer::number(true, INT32_MAX, false)),
            Types::Int32());
  EXPECT_EQ(computed(TypeInferer::number(true, uint64_t(INT32_MAX) + 1, true)),
            Types::Int32());
  EXPECT_EQ(computed(TypeInferer::number(true, uint64_t(INT32_MAX) + 1, false)),
            Types::Int64());
  EXPECT_EQ(computed(TypeInferer::number(true, uint64_t(INT64_MAX) + 1, true)),
            Types::Int64());
  EXPECT_EQ(computed(TypeInferer::number(true, UINT64_MAX, false)),
            Types::UInt64());
  EXPECT_EQ(computed(TypeInferer::number(false, 0, false)), Types::Float64());
  EXPECT_EQ(computed(TypeInferer::number(true, 7, false, Types::UInt8())),
            Types::UInt8());
}

/** Numeric and unary results depend only on supplied operand types. */
TEST(TypeAnalysis, Operators) {
  EXPECT_EQ(computed(TypeInferer::numeric(Types::Int16(), Types::Int64())),
            Types::Int64());
  EXPECT_EQ(computed(TypeInferer::numeric(Types::UInt32(), Types::Int32())),
            Types::UInt32());
  EXPECT_EQ(computed(TypeInferer::numeric(Types::Float32(), Types::Float64())),
            Types::Float64());
  EXPECT_EQ(computed(TypeInferer::unary(Types::Bool(), true)), Types::Bool());
  EXPECT_EQ(
      computed(TypeInferer::unary(Types::Reference(Types::Int32()), false)),
      Types::Int32());
  fails(TypeInferer::numeric({}, Types::Int32()),
        InferenceFailure::Kind::MissingType);
}

/** Branch compatibility comes from checking, while inference selects the
 * result. */
TEST(TypeAnalysis, BranchResults) {
  EXPECT_EQ(computed(TypeInferer::branches(Types::Float32(), Types::Float64(),
                                           true, true)),
            Types::Float64());
  EXPECT_EQ(computed(TypeInferer::branches(Types::Int32(), Types::Int64(), true,
                                           false)),
            Types::Int64());
  fails(TypeInferer::branches(Types::Bool(), Types::String(), false, false),
        InferenceFailure::Kind::BranchMismatch);
  fails(TypeInferer::branches({}, Types::Int32(), false, false),
        InferenceFailure::Kind::MissingType);
}

/** Array construction preserves dimensions and does not modify the element
 * type. */
TEST(TypeAnalysis, ArraysAndIndices) {
  auto row = Types::Array(Types::Int32(), {3});
  auto matrix = computed(TypeInferer::array(row, 2, {}, false));
  ASSERT_NE(matrix, nullptr);
  EXPECT_EQ(static_cast<const ArrayType&>(*matrix).getDimensions(),
            (std::vector<size_t>{2, 3}));
  EXPECT_EQ(static_cast<const ArrayType&>(*row).getDimensions(),
            (std::vector<size_t>{3}));
  EXPECT_EQ(computed(TypeInferer::index(matrix, 2)), Types::Int32());
  fails(TypeInferer::index(matrix, 1), InferenceFailure::Kind::IndexDimensions);
  fails(TypeInferer::index(Types::Int32(), 1),
        InferenceFailure::Kind::NotArray);
  fails(TypeInferer::array({}, 0, {}, false),
        InferenceFailure::Kind::EmptyArray);
  auto widened =
      computed(TypeInferer::array(Types::Int32(), 4, Types::Int64(), true));
  EXPECT_EQ(static_cast<const ArrayType&>(*widened).getElementType(),
            Types::Int64());
  EXPECT_EQ(computed(TypeInferer::index(Types::Array(Types::Int32(), {}), 3)),
            Types::Int32());
}

/** Reference construction and callable queries leave shared metadata intact. */
TEST(TypeAnalysis, RepeatedQueriesPreserveInputs) {
  auto callable = Types::Lambda(Types::Int64(), {Types::Int32()}, true);
  auto& lambda = static_cast<LambdaType&>(*callable);
  lambda.setHasRefCaptures(true);
  auto before = callable->toString();
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(computed(TypeInferer::call(callable)), Types::Int64());
    auto ref = computed(
        TypeInferer::reference(Types::Reference(Types::Int32()), false));
    ASSERT_NE(ref, nullptr);
    EXPECT_TRUE(ref->equals(*Types::Reference(Types::Int32(), false)));
  }
  EXPECT_EQ(callable->toString(), before);
  EXPECT_TRUE(lambda.hasRefCaptures());
  EXPECT_TRUE(lambda.canThrow());
  EXPECT_EQ(computed(TypeInferer::call(Types::Function(Types::UInt8(), {}))),
            Types::UInt8());
  fails(TypeInferer::call(Types::Bool()), InferenceFailure::Kind::NotCallable);
  fails(TypeInferer::reference({}, true), InferenceFailure::Kind::MissingType);
}

/** Written shapes retain first-binding and explicit-argument precedence. */
TEST(TypeAnalysis, GenericAnnotationBindings) {
  std::vector<sun::ast::TypeAnnotation> shapes{sun::ast::TypeAnnotation("T"),
                                               sun::ast::TypeAnnotation("T")};
  auto inferred =
      inferTypeArguments({"T"}, shapes, {Types::Int32(), Types::Int64()});
  ASSERT_TRUE(std::holds_alternative<std::vector<TypePtr>>(inferred));
  EXPECT_EQ(std::get<std::vector<TypePtr>>(inferred)[0], Types::Int32());
  auto explicitResult =
      inferTypeArguments({"T"}, shapes, {Types::Int32()}, {Types::UInt8()});
  EXPECT_EQ(std::get<std::vector<TypePtr>>(explicitResult)[0], Types::UInt8());
  auto failure = inferTypeArguments({"T", "U"}, shapes, {Types::Int32()});
  ASSERT_TRUE(std::holds_alternative<InferenceFailure>(failure));
  EXPECT_EQ(std::get<InferenceFailure>(failure).parameter, "U");
}

/** Resolved shapes preserve nested parameters and template-time bindings. */
TEST(TypeAnalysis, GenericResolvedBindings) {
  auto parameter = Types::TypeParameter("T");
  std::vector<TypePtr> shapes{Types::Reference(Types::Array(parameter, {}))};
  auto inferred =
      inferTypeArguments({"T"}, shapes, {Types::Array(Types::UInt8(), {4})});
  EXPECT_EQ(std::get<std::vector<TypePtr>>(inferred)[0], Types::UInt8());
  auto abstract =
      inferTypeArguments({"T"}, std::vector<TypePtr>{parameter}, {parameter});
  EXPECT_EQ(std::get<std::vector<TypePtr>>(abstract)[0], parameter);
  EXPECT_TRUE(mentionsTypeParameter(shapes[0]));
  EXPECT_FALSE(mentionsTypeParameter(Types::Int32()));
}

/** Failed queries return data without writing compiler diagnostics. */
TEST(TypeAnalysis, FailuresDoNotEmitDiagnostics) {
  testing::internal::CaptureStderr();
  auto result =
      TypeInferer::branches(Types::Bool(), Types::String(), false, false);
  auto diagnostics = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(diagnostics.empty());
  EXPECT_TRUE(std::holds_alternative<InferenceFailure>(result));
}
