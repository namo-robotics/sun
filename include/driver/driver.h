#pragma once

#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/Support/TargetSelect.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "support/error.h"
#include "moon_bundling/moon_import.h"
#include "parsing/parser.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "driver/sun_value.h"

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

  // Whether to print LLVM IR to stdout (controlled by --emit-ir)
  bool dumpIR = false;

  // If true, dump all reachable functions; if false, only user-defined
  bool dumpReachable = false;

  // Debug mode: generate AST DOT graph and IR dump
  bool debugMode_ = false;
  std::string debugFolder_;

  // Moon libraries to preload for single-file compilation modes
  std::vector<sun::MoonImport> moonImports_;
  std::vector<std::string> protoFiles_;
  bool dumpProtoSun_ = false;

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

  // Front half of the pipeline shared by compilation and analysis: lowering,
  // moon stub injection and semantic analysis (no borrow check, no codegen)
  void analyzeProgram(BlockExprAST& blockAst, Parser& parser);

  // Register a source string for error reporting and create its parser
  Parser prepareStringParser(const std::string& source,
                             const std::string& filePath);

  // Read, parse and merge every source file plus synthesized .proto modules.
  // sourceOverrides maps a canonical path to text used instead of the file
  // on disk (an editor's unsaved buffer).
  std::unique_ptr<BlockExprAST> parseAndMergeFiles(
      const std::vector<std::string>& sourceFiles,
      const std::vector<std::string>& protoFiles,
      const std::map<std::string, std::string>& sourceOverrides);

  // Initialize LLVM targets once (thread-safe)
  static void ensureLLVMInitialized() {
    static std::once_flag flag;
    std::call_once(flag, []() {
      llvm::InitializeNativeTarget();
      llvm::InitializeNativeTargetAsmPrinter();
      llvm::InitializeNativeTargetAsmParser();
    });
  }

  // Synthesize Sun modules for manifest .proto files and parse them into
  // parsedFiles/canonicalPaths (see ProtoImporter)
  void parseSynthesizedProtoModules(
      const std::vector<std::string>& protoFiles,
      std::vector<std::unique_ptr<BlockExprAST>>& parsedFiles,
      std::vector<std::string>& canonicalPaths);

  void dumpUserDefinedIR(llvm::raw_ostream& OS);
  void writeUserDefinedIR(const std::string& path);

 public:
  /// Create a Driver for JIT execution. debugInfo enables DWARF emission (-g).
  static std::unique_ptr<Driver> createForJIT(
      const std::string& moduleName = "sun", bool debugInfo = false);

  /// Create a Driver for AOT compilation (no JIT). A non-empty targetTriple
  /// cross-compiles for that target (object/IR emission only — linking and
  /// execution stay host-only). debugInfo enables DWARF emission (-g).
  static std::unique_ptr<Driver> createForAOT(
      const std::string& moduleName = "module",
      const std::string& targetTriple = "", bool debugInfo = false);

  /// Execute a source string with optional command-line arguments
  /// filePath is used for error messages (optional)
  sun::SunValue executeString(const std::string& source, int argc = 0,
                              char** argv = nullptr,
                              const std::string& filePath = "");

  /// Execute a file with optional command-line arguments; returns main()'s
  /// value (VoidValue for void main)
  sun::SunValue executeFile(const std::string& filename, int argc = 0,
                            char** argv = nullptr);

  /// Compile a source string to IR without executing
  /// filePath is used for error messages (optional)
  void compileString(const std::string& source,
                     const std::string& filePath = "");

  /// Compile a file to IR without executing
  void compileFile(const std::string& filename);

  /// A program that was parsed and semantically analyzed but not compiled.
  /// `ast` is null only when parsing failed. `error` holds the first error
  /// raised; the tree keeps every type resolved before it, which is what
  /// editor tooling needs while a file is mid-edit.
  struct AnalyzedProgram {
    std::unique_ptr<BlockExprAST> ast;
    std::optional<SunError> error;
  };

  /// Parse and analyze a source string without generating code
  AnalyzedProgram analyzeString(const std::string& source,
                                const std::string& filePath = "");

  /// Parse and analyze a set of files (merged like compileFiles) without
  /// generating code. sourceOverrides maps a canonical path to in-memory text
  /// used instead of the file on disk.
  AnalyzedProgram analyzeFiles(
      const std::vector<std::string>& sourceFiles,
      const std::vector<sun::MoonImport>& moonImports = {},
      const std::vector<std::string>& protoFiles = {},
      const std::map<std::string, std::string>& sourceOverrides = {});

  /// Set moon libraries to preload for single-file compilation modes
  /// (executeString, compileFile, etc.)
  void setMoonImports(std::vector<sun::MoonImport> imports) {
    moonImports_ = std::move(imports);
  }

  /// Set .proto schemas to import natively (see ProtoImporter): each is
  /// synthesized into ordinary Sun source and compiled with the program.
  void setProtoFiles(std::vector<std::string> protos) {
    protoFiles_ = std::move(protos);
  }

  /// Print the Sun source synthesized from each imported .proto to stdout
  void setDumpProtoSun(bool dump) { dumpProtoSun_ = dump; }

  /// Compile multiple source files with optional precompiled moon libraries
  /// This is the merged-AST compilation model: all files are parsed and merged
  /// into a single AST before semantic analysis and codegen.
  /// @param sourceFiles List of .sun source files to compile
  /// @param moonImports Precompiled .moon libraries with optional aliasing
  /// @param protoFiles .proto schemas to synthesize into Sun modules
  void compileFiles(const std::vector<std::string>& sourceFiles,
                    const std::vector<sun::MoonImport>& moonImports = {},
                    const std::vector<std::string>& protoFiles = {});

  /// Execute multiple source files with optional precompiled moon libraries
  /// @param sourceFiles List of .sun source files to compile and execute
  /// @param moonImports Precompiled .moon libraries with optional aliasing
  /// @param argc Argument count for main()
  /// @param argv Argument vector for main()
  sun::SunValue executeFiles(const std::vector<std::string>& sourceFiles,
                             const std::vector<sun::MoonImport>& moonImports = {},
                             int argc = 0, char** argv = nullptr,
                             const std::vector<std::string>& protoFiles = {});

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
