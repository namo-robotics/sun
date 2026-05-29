#pragma once

#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/Support/TargetSelect.h>

#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "codegen.h"
#include "codegen_visitor.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "sun_value.h"

/// Driver orchestrates the compilation pipeline: parse → analyze → codegen →
/// execute. It owns all compilation components and provides static factory
/// methods for easy construction.
class Driver {
 private:
  // Owned components
  std::unique_ptr<CodegenContext> ctx;
  std::shared_ptr<sun::TypeRegistry> typeRegistry;
  std::unique_ptr<CodegenVisitor> codegenVisitor;
  std::unique_ptr<SemanticAnalyzer> analyzer;

  // Base directory for resolving relative imports
  std::string baseDir;

  // Track which files have already been imported (for cycle detection)
  std::shared_ptr<std::set<std::string>> importedFiles =
      std::make_shared<std::set<std::string>>();

  // Whether to print LLVM IR to stdout (controlled by --emit-ir)
  bool dumpIR = false;

  // If true, dump all reachable functions; if false, only user-defined
  bool dumpReachable = false;

  // Debug mode: generate AST DOT graph and IR dump
  bool debugMode_ = false;
  std::string debugFolder_;

  // Private constructor - use factory methods
  Driver(std::unique_ptr<CodegenContext> ctx,
         std::shared_ptr<sun::TypeRegistry> typeRegistry,
         std::unique_ptr<CodegenVisitor> codegenVisitor,
         std::unique_ptr<SemanticAnalyzer> analyzer)
      : ctx(std::move(ctx)),
        typeRegistry(std::move(typeRegistry)),
        codegenVisitor(std::move(codegenVisitor)),
        analyzer(std::move(analyzer)) {}

  // Internal helper: run full pipeline on parsed AST
  sun::SunValue runPipeline(std::unique_ptr<BlockExprAST> blockAst,
                            Parser& parser, bool execute, int argc = 0,
                            char** argv = nullptr);

  // Initialize LLVM targets once (thread-safe)
  static void ensureLLVMInitialized() {
    static std::once_flag flag;
    std::call_once(flag, []() {
      llvm::InitializeNativeTarget();
      llvm::InitializeNativeTargetAsmPrinter();
      llvm::InitializeNativeTargetAsmParser();
    });
  }

 public:
  /// Create a Driver for JIT execution
  static std::unique_ptr<Driver> createForJIT(
      const std::string& moduleName = "sun");

  /// Create a Driver for AOT compilation (no JIT)
  static std::unique_ptr<Driver> createForAOT(
      const std::string& moduleName = "module");

  /// Execute a source string with optional command-line arguments
  /// filePath is used for error messages (optional)
  sun::SunValue executeString(const std::string& source, int argc = 0,
                              char** argv = nullptr,
                              const std::string& filePath = "");

  /// Execute a file with optional command-line arguments
  void executeFile(const std::string& filename, int argc = 0,
                   char** argv = nullptr);

  /// Compile a source string to IR without executing
  /// filePath is used for error messages (optional)
  void compileString(const std::string& source,
                     const std::string& filePath = "");

  /// Compile a file to IR without executing
  void compileFile(const std::string& filename);

  /// Compile a file and return the parsed AST for metadata extraction
  std::unique_ptr<BlockExprAST> compileFileWithAST(const std::string& filename);

  /// Access the underlying module (for emitting object code after compilation)
  llvm::Module& getModule() { return *ctx->mainModule; }

  /// Enable/disable LLVM IR dumping to stdout
  void setDumpIR(bool dump) { dumpIR = dump; }

  /// Enable dumping all reachable functions (includes stdlib)
  void setDumpReachable(bool dump) { dumpReachable = dump; }

  /// Enable debug mode and set the debug output folder
  /// Creates <basename>_debug/ folder with ast.dot and ir.ll
  void setDebugMode(bool enable, const std::string& inputFile = "");

  /// Print only user-defined IR (filters out imports and linked libraries)
  void printUserDefinedIR();

  /// Print IR for all functions reachable from main() (includes stdlib)
  void printReachableIR();
};
