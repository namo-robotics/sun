/** Collects tests and generates the compiled test runner. */
#include <filesystem>
#include <fstream>
#include <inja/inja.hpp>
#include <sstream>

#include "driver/driver.h"
#include "generated/test_runner_template.h"
#include "lsp/name_ranges.h"
#include "lsp/text_positions.h"
#include "moon_bundling/library_cache.h"
#include "stdlib_support.h"
#include "support/display_path.h"
#include "support/error.h"
#include "support/source_manager.h"
#include "support/sun_path.h"
#include "support/terminal.h"

using sun::ast::ASTNodeType;
using sun::ast::BlockExprAST;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::ast::ModuleAST;
using sun::parsing::Parser;
using sun::support::logAndThrowError;
using sun::support::SourceManager;

/** Coordinates compilation, dependency loading, linking, and execution. */
namespace sun::driver {

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/** Records a test name, source location, and result adapter for the generated
 * runner.
 */
struct TestEntry {
  std::string name;
  std::string runner;
  sun::support::Position location;
  std::string label;
};

/**
 * Walk the item-level statements of a merged program, recursing through
 * module bodies. Strip mode erases every test function; collect mode makes
 * each one public — along with every module on the way down to it — because
 * the synthesized runner lives at root scope and could not name anything
 * module-private. Records each test's dotted path. Runs before moon-stub
 * injection, so only user-written modules are visited.
 */
void visitTests(BlockExprAST& block, std::vector<std::string>& modulePath,
                bool strip, std::vector<TestEntry>* found, bool& sawAny) {
  auto& body = block.mutableBody();
  for (auto it = body.begin(); it != body.end();) {
    ExprAST* stmt = it->get();
    if (stmt && stmt->getType() == ASTNodeType::MODULE) {
      auto& module = static_cast<ModuleAST&>(*stmt);
      modulePath.push_back(module.getName());
      bool sawInModule = false;
      visitTests(module.mutableBody(), modulePath, strip, found, sawInModule);
      if (sawInModule && !strip) {
        module.setVisibility(sun::semantic_analysis::Visibility::Public);
      }
      sawAny |= sawInModule;
      modulePath.pop_back();
      ++it;
      continue;
    }
    if (stmt && stmt->getType() == ASTNodeType::FUNCTION &&
        static_cast<FunctionAST&>(*stmt).isTest()) {
      sawAny = true;
      if (strip) {
        it = body.erase(it);
        continue;
      }
      auto& func = static_cast<FunctionAST&>(*stmt);
      func.setVisibility(sun::semantic_analysis::Visibility::Public);
      std::string path;
      for (const auto& segment : modulePath) {
        path += segment + ".";
      }
      path += func.getProto().getName();
      const auto& result = *func.getProto().getReturnType();
      const bool arithmetic =
          result.baseName == "_Result" && result.typeArguments.size() == 2 &&
          result.typeArguments[1]->baseName == "ArithmeticError";
      found->push_back({path,
                        arithmetic                     ? "run_arithmetic"
                        : result.baseName == "_Result" ? "run_one"
                                                       : "run_result",
                        func.getLocation(), func.getProto().getName()});
      ++it;
      continue;
    }
    ++it;
  }
}

/**
 * Remove a root-level `main` so the synthesized runner can take its place.
 */
void removeRootMain(BlockExprAST& block) {
  auto& body = block.mutableBody();
  for (auto it = body.begin(); it != body.end(); ++it) {
    ExprAST* stmt = it->get();
    if (stmt && stmt->getType() == ASTNodeType::FUNCTION &&
        static_cast<FunctionAST&>(*stmt).getProto().getName() == "main") {
      body.erase(it);
      return;
    }
  }
}

/**
 * The test binary's entry point, rendered from the embedded runner template
 * (src/driver/test_runner_template.inja.sun) with the collected dotted test
 * names and runner adapters. JSON is used only at the template boundary.
 */
std::string synthesizeTestRunner(const std::vector<TestEntry>& tests) {
  inja::Environment env;
  // Jinja-style whitespace handling, so the template's {% for %} lines
  // leave no blank lines behind in the rendered runner.
  env.set_trim_blocks(true);
  env.set_lstrip_blocks(true);
  nlohmann::json data;
  data["tests"] = nlohmann::json::array();
  nlohmann::json listing = {{"version", 1}, {"tests", nlohmann::json::array()}};
  for (const auto& test : tests) {
    data["tests"].push_back({{"name", test.name}, {"runner", test.runner}});
    const auto file = test.location.filePath.value_or("");
    const auto source =
        sun::support::SourceManager::instance().getSource(file).value_or("");
    const auto range = sun::lsp::nameRange(test.location, test.label, source);
    const auto start =
        sun::lsp::lspPositionFromByteOffset(source, range.offset);
    const auto end = sun::lsp::lspPositionFromByteOffset(
        source, range.endOffset.value_or(range.offset));
    listing["tests"].push_back(
        {{"id", test.name},
         {"label", test.label},
         {"file", file},
         {"range",
          {{"start", {{"line", start.line}, {"character", start.character}}},
           {"end", {{"line", end.line}, {"character", end.character}}}}}});
  }
  // JSON-encode twice: once for the wire payload, then for a Sun string
  // literal.
  data["listing_literal"] = nlohmann::json(listing.dump()).dump();
  return env.render(kTestRunnerTemplate, data);
}

}  // namespace

void Driver::applyTestHandling(BlockExprAST& blockAst) {
  std::vector<std::string> modulePath;
  std::vector<TestEntry> tests;
  bool sawAny = false;

  if (testHandling_ == TestHandling::Strip) {
    visitTests(blockAst, modulePath, /*strip=*/true, nullptr, sawAny);
    hasTests_ |= sawAny;
    return;
  }

  visitTests(blockAst, modulePath, /*strip=*/false, &tests, sawAny);
  hasTests_ |= sawAny;
  if (tests.empty()) {
    logAndThrowError(
        "no test functions found: declare tests with 'test_function' or list "
        "test-only sources under 'test_files:' in the manifest");
  }

  // The runner and its assertions live in std.test, so the test build needs
  // the standard library (same rule as string interpolation).
  if (!hasStdlibImport(moonImports_) && !declaresStdlibString(blockAst)) {
    logAndThrowError(
        "test functions require the standard library. Add "
        "'moon \"stdlib.moon\"' to your manifest or use --moon stdlib.moon");
  }

  removeRootMain(blockAst);

  std::string runnerSrc = synthesizeTestRunner(tests);

  if (debugMode_ && !debugFolder_.empty()) {
    std::string runnerPath = debugFolder_ + "/test_runner.sun";
    std::ofstream runnerFile(runnerPath);
    if (runnerFile) {
      runnerFile << runnerSrc;
      sun::support::logPathMessage("debug", "  Generated: ", runnerPath);
    } else {
      sun::support::messageStream("warning")
          << "Could not write " << sun::support::displayPath(runnerPath)
          << "\n";
    }
  }

  static constexpr const char* kRunnerPath = "<test-runner>";
  SourceManager::instance().addSource(kRunnerPath, runnerSrc);
  auto runnerParser = Parser::createStringParser(runnerSrc);
  runnerParser.setFilePath(kRunnerPath);
  auto runnerAst = runnerParser.parseProgram();
  for (auto& stmt : runnerAst->mutableBody()) {
    blockAst.mutableBody().push_back(std::move(stmt));
  }
}

}  // namespace sun::driver
