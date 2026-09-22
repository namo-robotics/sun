/** Checks explicit scheduling of prepared generic specialization bodies. */
#include <gtest/gtest.h>

#include <sstream>

#include "parsing/parser.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

/** Keeps the preparation-only fixture local to these tests. */
namespace {
using namespace sun::semantic_analysis;
using sun::types::Types;

/** Prepare declarations without checking source or specialization bodies. */
class TypeAnalysis_SpecializationBodies : public ::testing::Test {
 protected:
  std::shared_ptr<AnalysisResults> results =
      std::make_shared<AnalysisResults>();
  SemanticAnalyzer analyzer{results};
  SemanticContext& context = analyzer.context();
  std::unique_ptr<sun::ast::BlockExprAST> tree;
  std::vector<std::unique_ptr<sun::ast::BlockExprAST>> earlierTrees;

  /** Parse and register a complete test program without draining body jobs. */
  void prepare(const std::string& source) {
    std::istringstream input;
    sun::parsing::Parser parser(input);
    if (tree) earlierTrees.push_back(std::move(tree));
    tree = parser.parseString(source);
    passes::FieldInitializerPreparationPass().run(*tree);
    passes::DeclarationIdentityPass(results->declarations).run(*tree);
    passes::DeclarationNamingPass().run(*tree);
    passes::TypeRegistrationPass(context).run(*tree);
    analyzer.pipeline().declarations().run(*tree);
  }

  /** Prepare one concrete function specialization without checking its body. */
  SpecializedFunctionInfo function(const std::string& name) {
    const auto* generic = context.lookupGenericFunction(name);
    EXPECT_NE(generic, nullptr);
    return analyzer.generics()
        .instantiateGenericFunction(*generic, {Types::Int32()})
        .value();
  }
};
}  // namespace

/** Invalid class, function and method bodies fail only at the explicit drain.
 */
TEST_F(TypeAnalysis_SpecializationBodies, BodiesWaitUntilDrain) {
  prepare(R"(
    /** A class with a concrete-only invalid method body. */
    class Box<T> {
      var value: T;
      /** Must remain unchecked during shape preparation. */
      method bad() i32 { return missing; }
    }
    /** Must remain unchecked during signature preparation. */
    function bad<T>(value: T) i32 { return missing; }
    /** Supplies a method template on an ordinary class. */
    class Owner {
      /** Must remain unchecked during method specialization. */
      method bad<T>(value: T) i32 { return missing; }
    }
  )");
  auto& generics = analyzer.generics();
  auto type = generics.instantiateGenericClass("Box", {Types::Int32()});
  ASSERT_NE(type->getField("value"), nullptr);
  auto callable = function("bad");
  EXPECT_EQ(callable.returnType, Types::Int32());
  auto method = generics.instantiateGenericMethod(context.lookupClass("Owner"),
                                                  "bad", {Types::Int32()});
  ASSERT_NE(method, nullptr);
  EXPECT_EQ(method->getProto().getResolvedReturnType(), Types::Int32());
  EXPECT_TRUE(generics.hasPendingBodies());
  auto* scope = context.scope();
  const auto file = context.currentSourceFileId();
  auto owner = context.getCurrentClass();
  EXPECT_THROW(generics.analyzePendingBodies(), sun::support::SunError);
  EXPECT_FALSE(generics.hasPendingBodies());
  EXPECT_EQ(context.scope(), scope);
  EXPECT_EQ(context.currentSourceFileId(), file);
  EXPECT_EQ(context.getCurrentClass(), owner);

  // Each callable path must fail at draining even without a class job ahead.
  prepare(R"(
    /** A standalone invalid callable. */
    function standalone<T>() i32 { return missing; }
    /** A separate owner for the invalid method. */
    class Other {
      /** An invalid method specialization. */
      method bad<T>() i32 { return missing; }
    }
  )");
  function("standalone");
  EXPECT_THROW(generics.analyzePendingBodies(), sun::support::SunError);
  generics.instantiateGenericMethod(context.lookupClass("Other"), "bad",
                                    {Types::Int32()});
  EXPECT_THROW(generics.analyzePendingBodies(), sun::support::SunError);
  EXPECT_FALSE(generics.hasPendingBodies());
}

/** A drain handles recursive cache hits and newly requested bodies exactly
 * once. */
TEST_F(TypeAnalysis_SpecializationBodies, DrainsNestedAndRecursiveJobs) {
  prepare(R"(
    /** Starts a mutual recursion chain. */
    function first<T>(n: i32) i32 {
      if (n == 0) { return 0; }
      return second<T>(n - 1);
    }
    /** Reuses the already published first specialization. */
    function second<T>(n: i32) i32 { return first<T>(n); }
  )");
  auto first = function("first");
  auto repeated = function("first");
  EXPECT_EQ(first.specializedAST, repeated.specializedAST);
  EXPECT_FALSE(
      first.specializedAST->getBody().getBody().back()->hasResolvedType());
  ASSERT_NO_THROW(analyzer.generics().analyzePendingBodies());
  EXPECT_FALSE(analyzer.generics().hasPendingBodies());
  EXPECT_TRUE(
      first.specializedAST->getBody().getBody().back()->hasResolvedType());
  function("first");
  function("second");
  EXPECT_FALSE(analyzer.generics().hasPendingBodies());
  EXPECT_NO_THROW(analyzer.generics().analyzePendingBodies());
}

/** Constructor initialization checks remain deferred along with class bodies.
 */
TEST_F(TypeAnalysis_SpecializationBodies, ConstructorChecksWaitUntilDrain) {
  prepare(R"(
    /** Leaves a field uninitialized for the queued constructor check. */
    class Box<T> {
      var value: T;
      /** Intentionally fails to initialize value. */
      init() {}
    }
  )");
  ASSERT_NO_THROW(
      analyzer.generics().instantiateGenericClass("Box", {Types::Int32()}));
  EXPECT_THROW(analyzer.generics().analyzePendingBodies(),
               sun::support::SunError);
  EXPECT_FALSE(analyzer.generics().hasPendingBodies());
}

/** Nested declaration collection is restored independently of body scheduling.
 */
TEST_F(TypeAnalysis_SpecializationBodies, CollectionDepthRestoresAfterFailure) {
  EXPECT_THROW(prepare(R"(
    /** Contains an unresolved field annotation. */
    module nested {
      /** Fails while collecting a nested declaration. */
      class Broken { var field: Missing; }
    }
  )"),
               sun::support::SunError);
  EXPECT_FALSE(context.isCollectingDeclarations());
}

/** A pipeline failure discards jobs prepared before the failing declaration. */
TEST_F(TypeAnalysis_SpecializationBodies, PipelineFailureDiscardsPendingJobs) {
  prepare(R"(
    /** Queues an otherwise valid body. */
    function queued<T>() i32 { return 1; }
  )");
  function("queued");
  EXPECT_TRUE(analyzer.generics().hasPendingBodies());
  EXPECT_THROW(
      analyzer.pipeline().run(
          *tree,
          [] { throw std::runtime_error("declarations callback failed"); }),
      std::runtime_error);
  EXPECT_FALSE(analyzer.generics().hasPendingBodies());
}

/** Compiled instances and symbolic class shapes never enqueue concrete bodies.
 */
TEST_F(TypeAnalysis_SpecializationBodies, CompiledAndAbstractShapesSkipBodies) {
  prepare(R"(
    /** Supplies a template with bodies that must not run in either path. */
    class Box<T> {
      var value: T;
      /** Intentionally invalid if concrete body checking is attempted. */
      method bad() i32 { return missing; }
    }
  )");
  auto* generic = context.lookupGenericClass("Box");
  ASSERT_NE(generic, nullptr);
  auto parameter = generic->typeParameters[0].toSunType(
      results->declarations,
      generic->AST->declarationIdentity().typeParameters[0]);
  ASSERT_NO_THROW(
      analyzer.generics().instantiateGenericClass(*generic, {parameter}));
  EXPECT_FALSE(analyzer.generics().hasPendingBodies());

  PortableDeclarationKey::assignOriginals(*tree, results->declarations,
                                          std::string(64, 'a'));
  auto& ast = const_cast<sun::ast::ClassDefinitionAST&>(*generic->AST);
  ast.setPrecompiled(true);
  ast.addCompiledSpecialization(
      PortableDeclarationKey::specialization(
          PortableDeclarationKey::fromDeclaration(ast.getDeclarationId(),
                                                  results->declarations),
          {PortableTypeKey::primitive("i32")})
          .encoding());
  ASSERT_NO_THROW(
      analyzer.generics().instantiateGenericClass(*generic, {Types::Int32()}));
  EXPECT_FALSE(analyzer.generics().hasPendingBodies());
}

/** One drain follows work discovered across every kind of specialization job.
 */
TEST_F(TypeAnalysis_SpecializationBodies, DrainsClassFunctionAndMethodChain) {
  prepare(R"(
    /** Supplies a class and a method template requested from a queued function. */
    class Box<T> {
      var value: T;
      /** Requests another callable while its own body is being drained. */
      method answer<U>(value: U) U { return finish<U>(value); }
    }
    /** Finishes the queue's chain of requests. */
    function finish<T>(value: T) T { return value; }
    /** Requests the class and method from a delayed body. */
    function start<T>(value: T) T {
      var box: Box<T> = { value: value };
      return box.answer<T>(value);
    }
  )");
  function("start");
  ASSERT_NO_THROW(analyzer.generics().analyzePendingBodies());
  EXPECT_FALSE(analyzer.generics().hasPendingBodies());
  function("finish");
  auto box =
      analyzer.generics().instantiateGenericClass("Box", {Types::Int32()});
  auto method = analyzer.generics().instantiateGenericMethod(box, "answer",
                                                             {Types::Int32()});
  ASSERT_NE(method, nullptr);
  EXPECT_FALSE(analyzer.generics().hasPendingBodies());
}

/** A callable with unresolved type arguments does not create a concrete body
 * job. */
TEST_F(TypeAnalysis_SpecializationBodies, AbstractCallableWaitsForArguments) {
  prepare(R"(
    /** Supplies a symbolic type argument without instantiating its owner. */
    class Box<T> { var value: T; }
    /** Remains a template until a concrete argument is supplied. */
    function identity<T>(value: T) T { return value; }
  )");
  const auto* box = context.lookupGenericClass("Box");
  auto parameter = box->typeParameters[0].toSunType(
      results->declarations, box->AST->declarationIdentity().typeParameters[0]);
  auto callable = analyzer.generics().instantiateGenericFunction(
      *context.lookupGenericFunction("identity"), {parameter});
  EXPECT_FALSE(callable);
  EXPECT_FALSE(analyzer.generics().hasPendingBodies());
}

/** A free generic function called by a method does not gain a receiver. */
TEST_F(TypeAnalysis_SpecializationBodies, FreeCallableDoesNotInheritThis) {
  prepare(R"(
    /** Cannot access a caller's receiver through a free function. */
    function outside<T>() i32 { return this.value; }
    /** Requests the free function while a class context is active. */
    class Owner {
      var value: i32;
      /** Queues the free function from within a method specialization. */
      method call<T>() i32 { return outside<T>(); }
    }
  )");
  analyzer.generics().instantiateGenericMethod(context.lookupClass("Owner"),
                                               "call", {Types::Int32()});
  EXPECT_THROW(analyzer.generics().analyzePendingBodies(),
               sun::support::SunError);
  EXPECT_FALSE(analyzer.generics().hasPendingBodies());
}
