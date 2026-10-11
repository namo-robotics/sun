/** Verifies that full LLVM exports can be parsed and contain native
 * declarations. */
#include <gtest/gtest.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "cli/compile_command.h"
#include "cli/options.h"

/** Owns temporary source and LLVM artifacts under the workspace. */
class Tooling_Cli_LLVMOutput : public testing::Test {
 protected:
  std::filesystem::path directory;

  /** Creates an isolated output directory for this process. */
  void SetUp() override {
    directory = std::filesystem::current_path() / "tmp" /
                ("llvm-output-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
  }

  /** Removes generated files, including when an assertion fails. */
  void TearDown() override { std::filesystem::remove_all(directory); }
};

/** The export must be a complete module, not the diagnostic function-only dump.
 */
TEST_F(Tooling_Cli_LLVMOutput, emits_parseable_complete_module) {
  const auto source = directory / "main.sun";
  const auto output = directory / "main.ll";
  std::ofstream(source) << R"(
/** Calls a native function without requiring its implementation at export time. */
extern "C" function native_value(value: i32) i32;
/** Keeps the external declaration reachable in the exported module. */
function main() i32 { return unsafe { native_value(42); }; }
)";
  sun::cli::BuildRunOptions options;
  options.inputFiles = {source.string()};
  options.outputFile = output.string();
  options.compileMode = true;
  options.emitObjOnly = true;
  options.emitLLVMOnly = true;
  options.shared.optimize = false;
  ASSERT_EQ(sun::cli::runCompileCommand(options), 0);
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(output.string(), diagnostic, context);
  ASSERT_NE(module, nullptr) << diagnostic.getMessage().str();
  ASSERT_NE(module->getFunction("main"), nullptr);
  EXPECT_FALSE(module->getFunction("main")->isDeclaration());
  ASSERT_NE(module->getFunction("native_value"), nullptr);
  EXPECT_TRUE(module->getFunction("native_value")->isDeclaration());
  EXPECT_FALSE(module->getTargetTriple().empty());
}
