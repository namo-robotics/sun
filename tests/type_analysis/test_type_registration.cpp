/** Checks that type registration works independently of semantic resolution. */
#include <gtest/gtest.h>

#include <sstream>

#include "parsing/parser.h"
#include "semantic_analysis/passes/declaration_identity_pass.h"
#include "semantic_analysis/passes/declaration_naming_pass.h"
#include "semantic_analysis/passes/type_registration_pass.h"

/** Registration exposes nested types and templates without resolving
 * annotations. */
TEST(TypeAnalysis_TypeRegistration, RegistersNamesWithoutResolvingTypes) {
  using namespace sun::semantic_analysis;
  std::istringstream input;
  sun::parsing::Parser parser(input);
  auto tree = parser.parseString(R"(
    /** Contains declarations whose annotations are intentionally unresolved. */
    module declarations {
      /** Leaves concrete signature resolution to declaration collection. */
      function consume(value: Missing) void {}
      /** Refers to a generic class declared later. */
      class Holder { var value: Box<i32>; }
      /** Supplies a class template without instantiating it. */
      class Box<T> { var value: T; }
      /** Supplies an ordinary interface name. */
      interface Named { var value: Missing; }
      /** Supplies an interface template. */
      interface Container<T> { var value: T; }
      /** Supplies an ordinary enum name. */
      enum State { Ready }
      /** Supplies an enum template. */
      enum Marker<T> { Empty }
      /** Supplies a function template without checking its body. */
      function identity<T>(value: T) T { return value; }
    }
  )");
  auto results = std::make_shared<AnalysisResults>();
  SemanticContext context(results);
  passes::DeclarationIdentityPass(results->declarations).run(*tree);
  passes::DeclarationNamingPass().run(*tree);
  passes::TypeRegistrationPass registration(context);
  ASSERT_NO_THROW(registration.run(*tree));

  ASSERT_NE(context.lookupModuleScope("declarations"), nullptr);
  context.enterModuleScope("declarations");
  auto holder = context.lookupClass("Holder");
  ASSERT_NE(holder, nullptr);
  EXPECT_FALSE(holder->hasField("value"));
  EXPECT_NE(context.lookupGenericClass("Box"), nullptr);
  EXPECT_NE(context.lookupInterface("Named"), nullptr);
  EXPECT_NE(context.lookupGenericInterface("Container"), nullptr);
  EXPECT_NE(context.lookupEnum("State"), nullptr);
  EXPECT_NE(context.lookupGenericEnum("Marker"), nullptr);
  EXPECT_NE(context.lookupGenericFunction("identity"), nullptr);
  EXPECT_TRUE(context.currentScope().functions.empty());
  context.exitScope();

  // Revisiting a prepared tree must preserve the registered type identity.
  ASSERT_NO_THROW(registration.run(*tree));
  context.enterModuleScope("declarations");
  EXPECT_EQ(context.lookupClass("Holder"), holder);
  context.exitScope();
}
