#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>

#include "driver/driver.h"
#include "driver/execution_utils.h"

TEST(Tooling_Backend_Optimization, buffer_accessor_inlines_after_moon_linking) {
  sun::driver::initTestEnvironment();
  for (bool optimize : {false, true}) {
    auto driver =
        sun::driver::Driver::createForAOT("buffer_inline", "", false, optimize);
    driver->setMoonImports(sun::driver::getStdlibMoonImports());
    std::string readerSymbol;
    driver->setMetadataCallback([&](const sun::ast::BlockExprAST& program,
                                    sun::semantic_analysis::SemanticAnalyzer&
                                        analyzer) {
      for (const auto& node : program.getBody()) {
        auto* function = dynamic_cast<const sun::ast::FunctionAST*>(node.get());
        if (function && function->getProto().getName() == "read_byte")
          readerSymbol =
              sun::semantic_analysis::PortableDeclarationKey::fromDeclaration(
                  function->getDeclarationId(),
                  analyzer.context().results().declarations)
                  .symbol("function");
      }
    });
    driver->compileString(R"(
      using std;
      function read_byte(buf: const ref ContiguousBuffer<u8>, index: i64) u8 {
        return unsafe { buf.get_unchecked(index); };
      }
      function main() i32 { return 0; }
    )");
    ASSERT_FALSE(readerSymbol.empty());
    llvm::Function* reader = driver->getModule().getFunction(readerSymbol);
    ASSERT_NE(reader, nullptr);
    unsigned calls = 0;
    unsigned byteLoads = 0;
    for (auto& block : *reader) {
      for (auto& instruction : block) {
        if (llvm::isa<llvm::CallBase>(instruction)) ++calls;
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
          if (load->getType()->isIntegerTy(8)) ++byteLoads;
        }
      }
    }
    if (optimize) {
      EXPECT_EQ(calls, 0u);
      EXPECT_EQ(byteLoads, 1u);
    } else {
      EXPECT_GT(calls, 0u);
    }
  }
}

/** JIT pruning removes unused definitions while library compilation keeps them.
 */
TEST(Tooling_Backend_Optimization, jit_prunes_without_losing_library_exports) {
  sun::driver::initTestEnvironment();
  const std::string source = R"(
    /** Provides an exported definition unused by this entrypoint. */
    function unused() i32 { return 99; }
    /** Returns an observable value from the entrypoint. */
    function main() i32 { return 17; }
  )";
  for (bool optimize : {false, true}) {
    auto jit = sun::driver::Driver::createForJIT("prune", false, optimize);
    EXPECT_EQ(jit->executeString(source), 17);
    for (const auto& function : jit->getModule())
      if (!function.isDeclaration()) EXPECT_EQ(function.getName(), "main");

    auto library =
        sun::driver::Driver::createForAOT("exports", "", false, optimize);
    library->compileString(source);
    unsigned definitions = 0;
    for (const auto& function : library->getModule())
      if (!function.isDeclaration()) ++definitions;
    EXPECT_GE(definitions, 2u);

    auto executable =
        sun::driver::Driver::createForAOT("executable", "", false, optimize);
    executable->setExecutable(true);
    executable->compileString(source);
    for (const auto& function : executable->getModule())
      if (!function.isDeclaration()) EXPECT_EQ(function.getName(), "main");
  }
}
