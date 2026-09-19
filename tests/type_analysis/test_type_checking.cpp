/** Tests compatibility rules without constructing a semantic session. */
#include <gtest/gtest.h>

#include "semantic_analysis/type_analysis/type_checking.h"
#include "semantic_analysis/type_analysis/type_traits.h"

using namespace sun::semantic_analysis;
using namespace sun::types;
using namespace sun::semantic_analysis::type_analysis;

/** Numeric compatibility retains the language's existing conversion rules. */
TEST(TypeAnalysis_Checking, NumericCompatibility) {
  EXPECT_TRUE(isAssignableTo(Types::Int16(), Types::Int64()));
  EXPECT_FALSE(isAssignableTo(Types::Int64(), Types::Int16()));
  EXPECT_TRUE(isAssignableTo(Types::Float64(), Types::Float32()));
  EXPECT_FALSE(isAssignableTo(Types::Char(), Types::Int32()));
  EXPECT_FALSE(isAssignableTo({}, Types::Int32()));
}

/** Reference compatibility preserves constness and prevents compound copies. */
TEST(TypeAnalysis_Checking, ReferenceCompatibility) {
  auto mutableRef = Types::Reference(Types::Int32(), true);
  auto constRef = Types::Reference(Types::Int32(), false);
  EXPECT_TRUE(isAssignableTo(mutableRef, constRef));
  EXPECT_FALSE(isAssignableTo(constRef, mutableRef));
  EXPECT_TRUE(isAssignableTo(constRef, Types::Int32()));
  auto array = Types::Array(Types::Int32(), {2});
  EXPECT_FALSE(isAssignableTo(Types::Reference(array), array));
}

/** A function conversion cannot discard its caller's safety obligations. */
TEST(TypeAnalysis_Checking, CallableCompatibility) {
  auto plain = Types::Function(Types::Int32(), {}, false, false);
  auto throwing = Types::Function(Types::Int32(), {}, true, false);
  auto unsafe = Types::Function(Types::Int32(), {}, false, true);
  EXPECT_TRUE(isAssignableTo(plain, throwing));
  EXPECT_FALSE(isAssignableTo(throwing, plain));
  EXPECT_TRUE(isAssignableTo(plain, unsafe));
  EXPECT_FALSE(isAssignableTo(unsafe, plain));
}

/** Literal and trait predicates inspect values and type shapes only. */
TEST(TypeAnalysis_Checking, LiteralRangesAndTraits) {
  EXPECT_TRUE(literalFitsInType(255, false, Type::Kind::UInt8));
  EXPECT_FALSE(literalFitsInType(256, false, Type::Kind::UInt8));
  EXPECT_FALSE(literalFitsInType(1, true, Type::Kind::UInt8));
  EXPECT_TRUE(literalFitsInType(128, true, Type::Kind::Int8));
  EXPECT_FALSE(literalFitsInType(128, false, Type::Kind::Int8));
  auto numeric = Types::TypeParameter("_Numeric");
  EXPECT_TRUE(satisfies(Types::Int32(), numeric));
  EXPECT_TRUE(satisfies(Types::Reference(Types::Float64()), numeric));
  EXPECT_FALSE(satisfies(Types::String(), numeric));
}
