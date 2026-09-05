// tests/tooling/backend/test_debug_info.cpp - DWARF debug info (-g) tests
//
// In-process tests assert on the module's debug metadata; driver-level tests
// emit real objects/executables and check them with llvm-dwarfdump and gdb,
// skipping when a tool is unavailable.

#include <gtest/gtest.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "driver/compiler.h"
#include "driver/driver.h"
#include "driver/execution_utils.h"

namespace {

constexpr const char* kSimpleProgram = R"(
function add(a: i32, b: i32) i32 {
  var sum: i32 = a + b;
  return sum;
}

function main() i32 {
  var x: i32 = 3;
  var y: i32 = 4;
  var s: i32 = add(x, y);
  return s;
}
)";

constexpr const char* kClassProgram = R"(
class Point {
  var x: i32;
  var y: i32;

  init(px: i32, py: i32) {
    this.x = px;
    this.y = py;
  }

  method magsq() i32 {
    return this.x * this.x + this.y * this.y;
  }
}

function main() i32 {
  var p = Point(3, 4);
  var m = p.magsq();
  return m;
}
)";

std::unique_ptr<Driver> compileWithDebug(const std::string& source,
                                         const std::string& triple = "") {
  initTestEnvironment();
  auto driver = Driver::createForAOT("debug_test", triple, /*debugInfo=*/true);
  driver->compileString(source);
  return driver;
}

std::string printModule(llvm::Module& module) {
  std::string text;
  llvm::raw_string_ostream os(text);
  module.print(os, nullptr);
  return text;
}

bool haveTool(const std::string& tool) {
  return std::system(("command -v " + tool + " >/dev/null 2>&1").c_str()) == 0;
}

std::string findTool(const std::vector<std::string>& candidates) {
  for (const auto& tool : candidates) {
    if (haveTool(tool)) return tool;
  }
  return "";
}

std::string readFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Emit an object file and run llvm-dwarfdump --verify on it; returns the
// verifier's exit code, or -1 when llvm-dwarfdump is unavailable.
int dwarfdumpVerify(llvm::Module& module, const std::string& objPath) {
  std::string tool = findTool({"llvm-dwarfdump-20", "llvm-dwarfdump"});
  if (tool.empty()) return -1;
  std::string errorMsg;
  EXPECT_TRUE(sun::emitObjectFile(module, objPath, errorMsg)) << errorMsg;
  int rc =
      std::system((tool + " --verify " + objPath + " >/dev/null 2>&1").c_str());
  return WEXITSTATUS(rc);
}

}  // namespace

// ============================================================================
// In-process metadata assertions
// ============================================================================

TEST(Tooling_Backend_DebugInfo, module_flags_present_with_g) {
  auto driver = compileWithDebug(kSimpleProgram);
  auto& module = driver->getModule();
  EXPECT_NE(module.getModuleFlag("Debug Info Version"), nullptr);
  EXPECT_NE(module.getModuleFlag("Dwarf Version"), nullptr);
  EXPECT_NE(module.getNamedMetadata("llvm.dbg.cu"), nullptr);
}

TEST(Tooling_Backend_DebugInfo, subprograms_attached_to_functions) {
  auto driver = compileWithDebug(kSimpleProgram);
  auto& module = driver->getModule();

  auto* mainFunc = module.getFunction("main");
  ASSERT_NE(mainFunc, nullptr);
  auto* mainSP = mainFunc->getSubprogram();
  ASSERT_NE(mainSP, nullptr);
  EXPECT_GT(mainSP->getLine(), 0u);

  // Overloadable functions get mangled symbols (add$i32$i32); find by the
  // subprogram's source name, which is what debuggers match on.
  llvm::DISubprogram* addSP = nullptr;
  for (auto& func : module) {
    if (auto* sp = func.getSubprogram(); sp && sp->getName() == "add") {
      addSP = sp;
    }
  }
  ASSERT_NE(addSP, nullptr);
  EXPECT_NE(addSP, mainSP);
}

TEST(Tooling_Backend_DebugInfo, locals_and_params_have_dbg_declares) {
  auto driver = compileWithDebug(kSimpleProgram);
  std::string ir = printModule(driver->getModule());

  EXPECT_NE(ir.find("#dbg_declare"), std::string::npos);
  EXPECT_NE(ir.find("!DILocalVariable(name: \"sum\""), std::string::npos);
  EXPECT_NE(ir.find("!DILocalVariable(name: \"a\", arg: 1"), std::string::npos);
  EXPECT_NE(ir.find("!DILocalVariable(name: \"b\", arg: 2"), std::string::npos);
}

TEST(Tooling_Backend_DebugInfo, class_types_described_with_members) {
  auto driver = compileWithDebug(kClassProgram);
  std::string ir = printModule(driver->getModule());

  EXPECT_NE(ir.find("!DICompositeType(tag: DW_TAG_structure_type, "
                    "name: \"Point\""),
            std::string::npos);
  EXPECT_NE(ir.find("!DIDerivedType(tag: DW_TAG_member, name: \"x\""),
            std::string::npos);
  // 'this' receiver is an artificial pointer parameter
  EXPECT_NE(ir.find("!DILocalVariable(name: \"this\", arg: 1"),
            std::string::npos);
}

TEST(Tooling_Backend_DebugInfo, shadowed_variables_get_lexical_block_scopes) {
  auto driver = compileWithDebug(R"(
function main() i32 {
  var x: i32 = 1;
  var r: i32 = 0;
  if (x > 0) {
    var x: i32 = 2;
    r = x + 10;
  }
  return r + x;
}
)");
  std::string ir = printModule(driver->getModule());

  EXPECT_NE(ir.find("!DILexicalBlock"), std::string::npos);
  // Both x's exist as distinct variables (one function-scoped, one
  // block-scoped) — count the DILocalVariable entries named x.
  size_t count = 0;
  for (size_t pos = 0; (pos = ir.find("!DILocalVariable(name: \"x\"", pos)) !=
                       std::string::npos;
       ++pos) {
    ++count;
  }
  EXPECT_EQ(count, 2u);
}

TEST(Tooling_Backend_DebugInfo, moon_bundle_carries_debug_info_into_g_compile) {
  initTestEnvironment();
  auto imports = getStdlibMoonImports();
  if (imports.empty()) GTEST_SKIP() << "stdlib.moon not built";

  auto driver = Driver::createForAOT("moon_debug_test", "", /*debugInfo=*/true);
  driver->setMoonImports(imports);
  driver->compileString(R"(
using std;
function main() i32 {
  println("hi");
  return 0;
}
)");

  // The linked stdlib println must arrive with its subprogram intact.
  llvm::DISubprogram* printlnSP = nullptr;
  for (auto& func : driver->getModule()) {
    if (auto* sp = func.getSubprogram(); sp && sp->getName() == "println") {
      printlnSP = sp;
    }
  }
  ASSERT_NE(printlnSP, nullptr);
  EXPECT_NE(printlnSP->getFile()->getFilename().find("print.sun"),
            std::string::npos);
}

TEST(Tooling_Backend_DebugInfo, moon_debug_info_stripped_from_non_g_compile) {
  initTestEnvironment();
  auto imports = getStdlibMoonImports();
  if (imports.empty()) GTEST_SKIP() << "stdlib.moon not built";

  auto driver = Driver::createForAOT("moon_strip_test");
  driver->setMoonImports(imports);
  driver->compileString(R"(
using std;
function main() i32 {
  println("hi");
  return 0;
}
)");
  auto& module = driver->getModule();

  // The stdlib bundle is built with -g, but a non-g compile must not keep it:
  // no compile units, no subprograms, and no "Debug Info Version" flag (which
  // would drop the backend to CodeGenOptLevel::None).
  EXPECT_EQ(module.getNamedMetadata("llvm.dbg.cu"), nullptr);
  EXPECT_EQ(module.getModuleFlag("Debug Info Version"), nullptr);
  for (auto& func : module) {
    EXPECT_EQ(func.getSubprogram(), nullptr)
        << func.getName().str() << " kept its subprogram";
  }
}

TEST(Tooling_Backend_DebugInfo, no_debug_metadata_without_g) {
  initTestEnvironment();
  auto driver = Driver::createForAOT("no_debug_test");
  driver->compileString(kSimpleProgram);
  auto& module = driver->getModule();

  EXPECT_EQ(module.getModuleFlag("Debug Info Version"), nullptr);
  EXPECT_EQ(module.getModuleFlag("Dwarf Version"), nullptr);
  EXPECT_EQ(module.getNamedMetadata("llvm.dbg.cu"), nullptr);
  EXPECT_EQ(printModule(module).find("!dbg"), std::string::npos);
}

TEST(Tooling_Backend_DebugInfo, jit_executes_with_debug_info) {
  initTestEnvironment();
  auto driver = Driver::createForJIT("debug_jit_test", /*debugInfo=*/true);
  auto value = driver->executeString(kSimpleProgram);
  EXPECT_EQ(value, 7);
}

// ============================================================================
// Object emission: llvm-dwarfdump verification
// ============================================================================

TEST(Tooling_Backend_DebugInfo, object_file_dwarf_verifies) {
  auto driver = compileWithDebug(kClassProgram);
  std::string objPath = ::testing::TempDir() + "sun_debug_info_test.o";
  int rc = dwarfdumpVerify(driver->getModule(), objPath);
  if (rc < 0) GTEST_SKIP() << "llvm-dwarfdump not installed";
  EXPECT_EQ(rc, 0);
}

TEST(Tooling_Backend_DebugInfo, cross_target_object_dwarf_verifies) {
  auto driver = compileWithDebug(kSimpleProgram, "aarch64-linux-gnu");
  std::string objPath = ::testing::TempDir() + "sun_debug_info_a64_test.o";
  int rc = dwarfdumpVerify(driver->getModule(), objPath);
  if (rc < 0) GTEST_SKIP() << "llvm-dwarfdump not installed";
  EXPECT_EQ(rc, 0);
}

// ============================================================================
// End-to-end: gdb / lldb against a linked executable
// ============================================================================

namespace {

// Link kSimpleProgram with -g; returns "" (and records a skip reason) when the
// host linker is unavailable. The source is written to a real file so
// debuggers can display source lines. `name` keeps concurrently running tests
// (ctest -j) from clobbering each other's artifacts.
std::string linkSimpleDebugBinary(const std::string& name,
                                  std::string& skipReason) {
  initTestEnvironment();
  std::string srcPath = ::testing::TempDir() + name + "_src.sun";
  std::ofstream(srcPath) << kSimpleProgram;

  auto driver = Driver::createForAOT("debug_bin_test", "", /*debugInfo=*/true);
  driver->compileFile(srcPath);

  std::string binary = ::testing::TempDir() + name + "_bin";
  std::string errorMsg;
  sun::LinkOptions linkOpts;
  if (!sun::compileToExecutable(driver->getModule(), binary, errorMsg,
                                /*keepObjectFile=*/false, linkOpts)) {
    skipReason = "host link failed: " + errorMsg;
    return "";
  }
  return binary;
}

}  // namespace

TEST(Tooling_Backend_DebugInfo, gdb_breaks_reads_args_and_steps) {
  if (!haveTool("gdb")) GTEST_SKIP() << "gdb not installed";
  std::string skipReason;
  std::string binary = linkSimpleDebugBinary("sun_dbg_gdb", skipReason);
  if (binary.empty()) GTEST_SKIP() << skipReason;

  std::string outPath = ::testing::TempDir() + "sun_debug_info_gdb.out";
  std::string cmd =
      "gdb --batch -q -ex 'break add' -ex run -ex 'info args' "
      "-ex next -ex 'print sum' " +
      binary + " > " + outPath + " 2>&1";
  // Through a variable: macOS's WEXITSTATUS takes its argument's address
  // (union-wait heritage), so it rejects an rvalue.
  int rc = std::system(cmd.c_str());
  // The transcript in the failure message: a nonzero exit alone (a command
  // erroring in batch mode, the debuggee failing to launch) says nothing.
  ASSERT_EQ(WEXITSTATUS(rc), 0) << readFile(outPath);

  std::string out = readFile(outPath);
  EXPECT_NE(out.find("Breakpoint 1, add (a=3, b=4)"), std::string::npos) << out;
  EXPECT_NE(out.find("var sum: i32 = a + b;"), std::string::npos) << out;
  EXPECT_NE(out.find("$1 = 7"), std::string::npos) << out;
}

TEST(Tooling_Backend_DebugInfo, gdb_debugs_jit_executed_code) {
  if (!haveTool("gdb")) GTEST_SKIP() << "gdb not installed";
  if (!std::filesystem::exists("build/sun")) {
    GTEST_SKIP() << "build/sun not found (test runs from workspace root)";
  }

  // A name that cannot collide with symbols in the sun binary itself.
  std::string srcPath = ::testing::TempDir() + "sun_debug_info_jit.sun";
  std::ofstream(srcPath) << R"(
function sununiqadd(a: i32, b: i32) i32 {
  var sum: i32 = a + b;
  return sum;
}
function main() i32 { return sununiqadd(3, 4); }
)";

  // The breakpoint stays pending until the JIT registers the object through
  // the GDB JIT interface (JITEventListener in SunJIT).
  std::string outPath = ::testing::TempDir() + "sun_debug_info_jit.out";
  std::string cmd =
      "timeout 120 gdb --batch -q -ex 'set breakpoint pending on' "
      "-ex 'break sununiqadd' -ex run -ex 'info args' -ex 'print a + b' "
      "--args build/sun -g " +
      srcPath + " > " + outPath + " 2>&1";
  // Through a variable: macOS's WEXITSTATUS takes its argument's address
  // (union-wait heritage), so it rejects an rvalue.
  int rc = std::system(cmd.c_str());
  // The transcript in the failure message: a nonzero exit alone (a command
  // erroring in batch mode, the debuggee failing to launch) says nothing.
  ASSERT_EQ(WEXITSTATUS(rc), 0) << readFile(outPath);

  std::string out = readFile(outPath);
  EXPECT_NE(out.find("Breakpoint 1, sununiqadd (a=3, b=4)"), std::string::npos)
      << out;
  EXPECT_NE(out.find("$1 = 7"), std::string::npos) << out;
}

TEST(Tooling_Backend_DebugInfo, lldb_breaks_reads_variables_and_steps) {
  std::string lldb = findTool({"lldb-20", "lldb"});
  if (lldb.empty()) GTEST_SKIP() << "lldb not installed";
  std::string skipReason;
  std::string binary = linkSimpleDebugBinary("sun_dbg_lldb", skipReason);
  if (binary.empty()) GTEST_SKIP() << skipReason;

  // DEBUGINFOD_URLS= : lldb otherwise blocks on unreachable symbol servers.
  // disable-aslr false: sandboxes may deny the personality() syscall lldb
  // uses to turn ASLR off in the debuggee.
  // `timeout` is GNU coreutils and absent on macOS, so it guards the run
  // only where it exists (mac lldb has no debuginfod to hang on).
  // --shlib scopes the breakpoint to our binary: --name matches across
  // every loaded module, and on macOS libobjc has an `add` of its own that
  // fires during process startup, long before main.
  std::string guard = haveTool("timeout") ? "timeout 120 " : "";
  std::string module = std::filesystem::path(binary).filename().string();
  std::string outPath = ::testing::TempDir() + "sun_debug_info_lldb.out";
  std::string cmd = guard + "env DEBUGINFOD_URLS= " + lldb +
                    " -b -o 'settings set target.disable-aslr false'"
                    " -o 'breakpoint set --name add --shlib " +
                    module +
                    "' -o run"
                    " -o 'frame variable' -o next -o 'print sum' " +
                    binary + " > " + outPath + " 2>&1";
  // Through a variable: macOS's WEXITSTATUS takes its argument's address
  // (union-wait heritage), so it rejects an rvalue.
  int rc = std::system(cmd.c_str());
  // The transcript in the failure message: a nonzero exit alone (a command
  // erroring in batch mode, the debuggee failing to launch) says nothing.
  ASSERT_EQ(WEXITSTATUS(rc), 0) << readFile(outPath);

  std::string out = readFile(outPath);
  EXPECT_NE(out.find("a = 3"), std::string::npos) << out;
  EXPECT_NE(out.find("b = 4"), std::string::npos) << out;
  EXPECT_NE(out.find("return sum;"), std::string::npos) << out;
  EXPECT_NE(out.find("(int) 7"), std::string::npos) << out;
}
