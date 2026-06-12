// src/driver.cpp
#include "driver.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "ast/manifest_ast.h"
#include "borrow_checker/borrow_checker.h"
#include "debug/ast_dot_generator.h"
#include "debug/scope_tree_generator.h"
#include "error.h"
#include "library_cache.h"
#include "module_linker.h"
#include "source_manager.h"
#include "sun_path.h"

static llvm::ExitOnError ExitOnErr;
using llvm::orc::ThreadSafeModule;

// ---------------------------------------------------------------------------
// Manifest processing helpers
// ---------------------------------------------------------------------------

/// Extract ManifestAST from a parsed program's top-level statements.
/// Returns nullptr if no manifest block is found.
static const ManifestAST* findManifest(const BlockExprAST& program) {
  for (const auto& stmt : program.getBody()) {
    if (stmt && stmt->getType() == ASTNodeType::MANIFEST) {
      return static_cast<const ManifestAST*>(stmt.get());
    }
  }
  return nullptr;
}

/// Resolve a manifest path - first relative to baseDir, then via SUN_PATH.
static std::string resolveManifestPath(const std::string& path,
                                       const std::string& baseDir) {
  std::filesystem::path p(path);
  if (p.is_absolute()) {
    return path;
  }
  // First try relative to base directory
  auto relative = std::filesystem::path(baseDir) / p;
  if (std::filesystem::exists(relative)) {
    return relative.lexically_normal().string();
  }
  // Then try SUN_PATH
  auto resolved = sun::SunPath::resolve(path);
  if (!resolved.empty()) {
    return resolved.string();
  }
  // Fall back to path as-is (will error later if not found)
  return path;
}

/// Process manifest and populate source files and moon imports.
/// baseDir is used to resolve relative paths.
static void processManifest(const ManifestAST& manifest,
                            const std::string& baseDir,
                            std::vector<std::string>& sunFiles,
                            std::vector<sun::MoonImport>& moonImports) {
  // Process sun dependencies
  for (const auto& sunDep : manifest.getSuns()) {
    std::string resolved = resolveManifestPath(sunDep.path, baseDir);
    sunFiles.push_back(resolved);
  }

  // Process moon dependencies
  for (const auto& moonDep : manifest.getMoons()) {
    std::string resolved = resolveManifestPath(moonDep.path, baseDir);
    if (moonDep.rename.has_value()) {
      // Create moon import with renaming
      moonImports.emplace_back(resolved, moonDep.rename.value(),
                               moonDep.rename.value());
    } else {
      moonImports.emplace_back(resolved);
    }
  }
}

// Factory method for JIT execution
std::unique_ptr<Driver> Driver::createForJIT(const std::string& moduleName) {
  ensureLLVMInitialized();

  auto jit = SunJIT::Create();
  if (!jit) {
    llvm::errs() << "Failed to create SunJIT: " << toString(jit.takeError())
                 << "\n";
    std::abort();
  }

  auto jitShared = std::shared_ptr<SunJIT>(std::move(jit.get()));
  auto ctx = std::make_unique<CodegenContext>(moduleName, jitShared);

  // Register runtime symbols for JIT
  auto& mainDylib = ctx->jit->getMainJITDylib();
  cantFail(mainDylib.define(llvm::orc::absoluteSymbols(
      {{ctx->jit->getExecutionSession().intern("putchard"),
        ExecutorSymbolDef(ExecutorAddr::fromPtr(&putchard),
                          JITSymbolFlags::Exported)}})));

  auto typeRegistry = std::make_shared<sun::TypeRegistry>();
  auto codegenVisitor = std::make_unique<CodegenVisitor>(*ctx, typeRegistry);
  auto analyzer = std::make_unique<SemanticAnalyzer>(typeRegistry);

  return std::unique_ptr<Driver>(new Driver(std::move(ctx), typeRegistry,
                                            std::move(codegenVisitor),
                                            std::move(analyzer)));
}

// Factory method for AOT compilation
std::unique_ptr<Driver> Driver::createForAOT(const std::string& moduleName) {
  ensureLLVMInitialized();

  auto ctx = std::make_unique<CodegenContext>(moduleName, nullptr);
  auto typeRegistry = std::make_shared<sun::TypeRegistry>();
  auto codegenVisitor = std::make_unique<CodegenVisitor>(*ctx, typeRegistry);
  auto analyzer = std::make_unique<SemanticAnalyzer>(typeRegistry);

  return std::unique_ptr<Driver>(new Driver(std::move(ctx), typeRegistry,
                                            std::move(codegenVisitor),
                                            std::move(analyzer)));
}

// Set debug mode and create the debug output folder
void Driver::setDebugMode(bool enable, const std::string& inputFile) {
  debugMode_ = enable;
  if (enable && !inputFile.empty()) {
    // Create debug folder: <basename>_debug/
    std::filesystem::path inputPath(inputFile);
    std::string basename = inputPath.stem().string();
    debugFolder_ = (inputPath.parent_path() / (basename + "_debug")).string();
    if (debugFolder_.empty() || debugFolder_ == "_debug") {
      debugFolder_ = basename + "_debug";
    }
    std::filesystem::create_directories(debugFolder_);
    llvm::outs() << "Debug output folder: " << debugFolder_ << "/\n";
  }
}

// Print only IR for user-defined functions (filter out library code)
void Driver::printUserDefinedIR() {
  // ANSI color codes for cyan output
  const char* cyan = "\033[36m";
  const char* reset = "\033[0m";

  llvm::outs() << cyan << "; LLVM IR (user-defined only):\n";

  // Get user-defined functions from codegen visitor
  const auto& userDefined = codegenVisitor->getUserDefinedFunctions();

  // Print user-defined global variables (skip library globals)
  for (auto& gv : ctx->mainModule->globals()) {
    std::string name = gv.getName().str();
    if (name.empty()) continue;
    // Skip internal symbols starting with _
    if (name[0] == '_') continue;
    // Skip library module globals (vtables, etc.)
    if (name.rfind("sun_", 0) == 0) continue;
    // Skip prefixed symbols (from moon imports)
    if (name.rfind("$", 0) == 0) continue;
    gv.print(llvm::outs());
    llvm::outs() << "\n";
  }

  // Print only user-defined functions
  for (auto& func : ctx->mainModule->functions()) {
    std::string name = func.getName().str();
    // Skip declarations (no body)
    if (func.isDeclaration()) continue;
    // Only print functions that are user-defined
    if (userDefined.count(name) == 0) continue;

    func.print(llvm::outs());
    llvm::outs() << "\n";
  }
  llvm::outs() << reset;
}

// Helper: recursively collect all functions reachable from a given function
static void collectReachableFunctions(llvm::Function* func,
                                      std::set<llvm::Function*>& visited) {
  if (!func || func->isDeclaration() || visited.count(func)) return;
  visited.insert(func);

  for (auto& BB : *func) {
    for (auto& I : BB) {
      // Direct calls
      if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
        if (auto* callee = call->getCalledFunction()) {
          collectReachableFunctions(callee, visited);
        }
      }
      // Invoke instructions (for exception handling)
      if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
        if (auto* callee = invoke->getCalledFunction()) {
          collectReachableFunctions(callee, visited);
        }
      }
    }
  }
}

// Print IR for all functions reachable from main() (includes stdlib)
void Driver::printReachableIR() {
  const char* cyan = "\033[36m";
  const char* reset = "\033[0m";

  llvm::outs() << cyan << "; LLVM IR (reachable from main):\n";

  // Start from main and collect all reachable functions
  std::set<llvm::Function*> reachable;
  if (auto* mainFunc = ctx->mainModule->getFunction("main")) {
    collectReachableFunctions(mainFunc, reachable);
  }

  // Also check __sun_static_init for global initializers
  if (auto* initFunc = ctx->mainModule->getFunction("__sun_static_init")) {
    collectReachableFunctions(initFunc, reachable);
  }

  // Print reachable functions (sorted by name for stable output)
  std::vector<llvm::Function*> sorted(reachable.begin(), reachable.end());
  std::sort(sorted.begin(), sorted.end(),
            [](llvm::Function* a, llvm::Function* b) {
              return a->getName() < b->getName();
            });

  for (auto* func : sorted) {
    func->print(llvm::outs());
    llvm::outs() << "\n";
  }

  llvm::outs() << reset;
}

sun::SunValue Driver::runPipeline(std::unique_ptr<BlockExprAST> blockAst,
                                  Parser& parser, bool execute, int argc,
                                  char** argv) {
  sun::SunValue result = sun::VoidValue{};

  if (!blockAst) {
    llvm::errs() << "Error: Failed to parse program.\n";
    return result;
  }

  // Debug mode: generate AST DOT graph
  if (debugMode_ && !debugFolder_.empty()) {
    AstDotGenerator dotGen;
    std::string dot = dotGen.generate(blockAst.get());
    std::string dotPath = debugFolder_ + "/ast.dot";
    std::ofstream dotFile(dotPath);
    if (dotFile) {
      dotFile << dot;
      llvm::outs() << "  Generated: " << dotPath << "\n";
    } else {
      llvm::errs() << "Warning: Could not write " << dotPath << "\n";
    }
  }

  // Inject AST stubs from moon imports before semantic analysis
  if (!moonImports_.empty()) {
    std::vector<std::unique_ptr<ExprAST>> moonStubs;
    for (const auto& moonImport : moonImports_) {
      if (auto moonScope = parser.collectMoonImport(moonImport)) {
        moonStubs.push_back(std::move(moonScope));
      }
    }
    if (!moonStubs.empty()) {
      blockAst->prependExpressions(std::move(moonStubs));
    }
  }

  // Run semantic analysis on the unified AST
  analyzer->analyzeBlock(*blockAst);

  // Debug mode: generate scope tree HTML after semantic analysis
  if (debugMode_ && !debugFolder_.empty()) {
    ScopeTreeGenerator scopeGen;
    std::string html = scopeGen.generateHtml(analyzer->getRootScope());
    std::string scopePath = debugFolder_ + "/scope_tree.html";
    std::ofstream scopeFile(scopePath);
    if (scopeFile) {
      scopeFile << html;
      llvm::outs() << "  Generated: " << scopePath << "\n";
    } else {
      llvm::errs() << "Warning: Could not write " << scopePath << "\n";
    }
  }

  // Run borrow checking on the unified AST
  // Uses compile-time settings from sun::Config
  sun::BorrowChecker borrowChecker;
  auto borrowErrors = borrowChecker.check(*blockAst);
  if (!borrowErrors.empty()) {
    for (const auto& err : borrowErrors) {
      std::cerr << err.format() << "\n";
    }
    throw SunError(SunError::Kind::Semantic,
                   "Borrow check failed with " +
                       std::to_string(borrowErrors.size()) + " error(s)");
  }

  // Register precompiled modules for lazy linking
  // This builds the symbol-to-module map without loading bitcode yet
  const auto& precompiledImports = parser.getPrecompiledImports();

  sun::ModuleLinker linker(*ctx->mainModule);
  bool hasMoonImports = !precompiledImports.empty() || !moonImports_.empty();

  if (!precompiledImports.empty()) {
    linker.registerAvailableModules(precompiledImports);
  }
  for (const auto& moonImport : moonImports_) {
    linker.registerAvailableModulesWithRemap(moonImport);
  }
  if (hasMoonImports) {
    // Create forward declarations for all functions from bitcode so codegen
    // can reference them before actual linking
    linker.declareAvailableFunctions();
  }

  // Snapshot precompiled function declarations before codegen starts
  // This lets codegen distinguish bitcode declarations from forward decls
  codegenVisitor->snapshotPrecompiledFunctions();

  // Generate code into single module
  codegenVisitor->codegen(*blockAst);
  // Emit static initialization function for globals that need runtime init
  codegenVisitor->emitStaticInitFunction();

  // Link only the modules that provide symbols actually used by the code
  // This happens AFTER codegen so we know exactly which symbols are needed
  if (hasMoonImports) {
    if (!linker.linkOnlyUsedSymbols()) {
      throw SunError(SunError::Kind::Semantic,
                     "Failed to link precompiled module: " + linker.getError());
    }
  }

  // Debug mode: dump full IR after codegen
  if (debugMode_ && !debugFolder_.empty()) {
    std::string irPath = debugFolder_ + "/ir.ll";
    std::error_code EC;
    llvm::raw_fd_ostream irFile(irPath, EC);
    if (!EC) {
      ctx->mainModule->print(irFile, nullptr);
      llvm::outs() << "  Generated: " << irPath << "\n";
    } else {
      llvm::errs() << "Warning: Could not write " << irPath << ": "
                   << EC.message() << "\n";
    }
  }

  // If not executing, handle AOT compilation specifics
  if (!execute) {
    // Handle main function return type for compiled programs
    if (auto* mainFunc = ctx->mainModule->getFunction("main")) {
      llvm::Type* retType = mainFunc->getReturnType();
      if (retType->isVoidTy()) {
        // Wrap void main() to return i32 0
        mainFunc->setName("__sun_main_void");
        llvm::FunctionType* wrapperType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(ctx->mainModule->getContext()),
            mainFunc->getFunctionType()->params(), false);
        llvm::Function* wrapper =
            llvm::Function::Create(wrapperType, llvm::Function::ExternalLinkage,
                                   "main", ctx->mainModule.get());
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(
            ctx->mainModule->getContext(), "entry", wrapper);
        llvm::IRBuilder<> builder(bb);
        std::vector<llvm::Value*> args;
        for (auto& arg : wrapper->args()) {
          args.push_back(&arg);
        }
        builder.CreateCall(mainFunc, args);
        builder.CreateRet(llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(ctx->mainModule->getContext()), 0));
      } else if (!retType->isIntegerTy(32)) {
        std::string typeName;
        if (retType->isIntegerTy()) {
          typeName = "i" + std::to_string(retType->getIntegerBitWidth());
        } else if (retType->isFloatTy()) {
          typeName = "f32";
        } else if (retType->isDoubleTy()) {
          typeName = "f64";
        } else if (retType->isPointerTy()) {
          typeName = "pointer (string)";
        } else {
          typeName = "unknown";
        }
        throw SunError(SunError::Kind::Type,
                       "For compiled programs, 'main' must return i32 or void, "
                       "but found return type '" +
                           typeName + "'");
      }
    }

    // Verify the module
    if (llvm::verifyModule(*ctx->mainModule, &llvm::errs())) {
      llvm::errs() << "Error: Module verification failed\n";
    }
    return result;
  }

  // JIT execution mode - print IR if requested
  if (dumpIR) {
    if (dumpReachable) {
      printReachableIR();
    } else {
      printUserDefinedIR();
    }
  }

  llvm::Function* func = ctx->mainModule->getFunction("main");
  if (!func) {
    llvm::errs() << "Error: Could not find 'main' function in module.\n";
    return result;
  }

  // Get the return type and argument count before we clone the module
  llvm::Type* returnType = func->getReturnType();
  size_t mainArgCount = func->arg_size();

  // Create a NEW context for the JIT module
  auto anonContext = std::make_unique<llvm::LLVMContext>();

  // Clone the module into the new context
  auto moduleClone = llvm::CloneModule(*ctx->mainModule);

  // Add the cloned module to JIT with its own context
  auto RT = ctx->jit->getMainJITDylib().createResourceTracker();
  ExitOnErr(ctx->jit->addModule(
      ThreadSafeModule(std::move(moduleClone), std::move(anonContext)), RT));

  // Run static constructors if present (handles global var initialization)
  if (auto initSym = ctx->jit->lookup("__sun_static_init")) {
    void (*initFP)() = initSym->getAddress().toPtr<void (*)()>();
    initFP();
  } else {
    // Consume and ignore the "symbol not found" error
    llvm::consumeError(initSym.takeError());
  }

  // Lookup and execute with appropriate type
  auto ExprSymbol = ExitOnErr(ctx->jit->lookup("main"));

  // Check if main takes arguments (argc, argv)
  bool mainHasArgs = (mainArgCount == 2);

  // Handle all primitive return types
  if (returnType->isVoidTy()) {
    if (mainHasArgs) {
      void (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<void (*)(int, char**)>();
      FP(argc, argv);
    } else {
      void (*FP)() = ExprSymbol.getAddress().toPtr<void (*)()>();
      FP();
    }
    result = sun::VoidValue{};
  } else if (returnType->isIntegerTy()) {
    unsigned bitWidth = returnType->getIntegerBitWidth();
    if (bitWidth == 1) {
      // bool (i1)
      bool boolResult;
      if (mainHasArgs) {
        bool (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<bool (*)(int, char**)>();
        boolResult = FP(argc, argv);
      } else {
        bool (*FP)() = ExprSymbol.getAddress().toPtr<bool (*)()>();
        boolResult = FP();
      }
      result = boolResult;
    } else if (bitWidth == 8) {
      int8_t intResult;
      if (mainHasArgs) {
        int8_t (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<int8_t (*)(int, char**)>();
        intResult = FP(argc, argv);
      } else {
        int8_t (*FP)() = ExprSymbol.getAddress().toPtr<int8_t (*)()>();
        intResult = FP();
      }
      result = intResult;
    } else if (bitWidth == 16) {
      int16_t intResult;
      if (mainHasArgs) {
        int16_t (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<int16_t (*)(int, char**)>();
        intResult = FP(argc, argv);
      } else {
        int16_t (*FP)() = ExprSymbol.getAddress().toPtr<int16_t (*)()>();
        intResult = FP();
      }
      result = intResult;
    } else if (bitWidth <= 32) {
      int32_t intResult;
      if (mainHasArgs) {
        int32_t (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<int32_t (*)(int, char**)>();
        intResult = FP(argc, argv);
      } else {
        int32_t (*FP)() = ExprSymbol.getAddress().toPtr<int32_t (*)()>();
        intResult = FP();
      }
      result = intResult;
    } else {
      int64_t intResult;
      if (mainHasArgs) {
        int64_t (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<int64_t (*)(int, char**)>();
        intResult = FP(argc, argv);
      } else {
        int64_t (*FP)() = ExprSymbol.getAddress().toPtr<int64_t (*)()>();
        intResult = FP();
      }
      result = intResult;
    }
  } else if (returnType->isFloatTy()) {
    float floatResult;
    if (mainHasArgs) {
      float (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<float (*)(int, char**)>();
      floatResult = FP(argc, argv);
    } else {
      float (*FP)() = ExprSymbol.getAddress().toPtr<float (*)()>();
      floatResult = FP();
    }
    result = floatResult;
  } else if (returnType->isDoubleTy()) {
    double doubleResult;
    if (mainHasArgs) {
      double (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<double (*)(int, char**)>();
      doubleResult = FP(argc, argv);
    } else {
      double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
      doubleResult = FP();
    }
    result = doubleResult;
  } else if (returnType->isPointerTy()) {
    // Assume string (i8*) for pointer types
    const char* strResult;
    if (mainHasArgs) {
      const char* (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<const char* (*)(int, char**)>();
      strResult = FP(argc, argv);
    } else {
      const char* (*FP)() = ExprSymbol.getAddress().toPtr<const char* (*)()>();
      strResult = FP();
    }
    result = std::string(strResult ? strResult : "");
  } else if (returnType->isStructTy()) {
    // static_ptr<u8> is struct { ptr, i64 } - extract the pointer as a string
    auto* structType = llvm::cast<llvm::StructType>(returnType);
    if (structType->getNumElements() == 2 &&
        structType->getElementType(0)->isPointerTy() &&
        structType->getElementType(1)->isIntegerTy(64)) {
      struct StaticPtr {
        const char* data;
        int64_t len;
      };
      StaticPtr spResult;
      if (mainHasArgs) {
        StaticPtr (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<StaticPtr (*)(int, char**)>();
        spResult = FP(argc, argv);
      } else {
        StaticPtr (*FP)() = ExprSymbol.getAddress().toPtr<StaticPtr (*)()>();
        spResult = FP();
      }
      result =
          std::string(spResult.data ? spResult.data : "",
                      spResult.data ? static_cast<size_t>(spResult.len) : 0);
    }
  } else {
    // Unknown type - default to double for backward compatibility
    double doubleResult;
    if (mainHasArgs) {
      double (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<double (*)(int, char**)>();
      doubleResult = FP(argc, argv);
    } else {
      double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
      doubleResult = FP();
    }
    result = doubleResult;
  }

  // Remove the anonymous module
  ExitOnErr(RT->remove());

  return result;
}

sun::SunValue Driver::executeString(const std::string& source, int argc,
                                    char** argv, const std::string& filePath) {
  std::string effectivePath = filePath;
  if (!filePath.empty()) {
    std::filesystem::path sourcePath = std::filesystem::absolute(filePath);
    baseDir = sourcePath.parent_path().string();
    effectivePath = sourcePath.string();
    if (std::filesystem::exists(sourcePath)) {
      importedFiles->insert(std::filesystem::canonical(sourcePath).string());
    } else {
      importedFiles->insert(sourcePath.lexically_normal().string());
    }
  } else {
    // Anonymous source (e.g., from executeString in tests)
    effectivePath = SourceManager::instance().addAnonymousSource(source);
  }

  // Register source for error reporting
  if (!filePath.empty()) {
    SourceManager::instance().addSource(effectivePath, source);
  }

  auto parser = Parser::createStringParser(source);
  parser.setImportedFiles(importedFiles);
  parser.setBaseDir(baseDir);
  if (!filePath.empty()) {
    parser.setFilePath(filePath);
  } else {
    parser.setFilePath(effectivePath);
  }

  auto blockAst = parser.parseProgram();
  return runPipeline(std::move(blockAst), parser, true, argc, argv);
}

void Driver::executeFile(const std::string& filename, int argc, char** argv) {
  std::filesystem::path filePath = std::filesystem::absolute(filename);
  std::string baseDirPath = filePath.parent_path().string();
  std::string canonical = std::filesystem::canonical(filePath).string();

  std::ifstream file(filename);
  if (!file.is_open()) {
    llvm::errs() << "Error: Could not open file '" << filename << "'\n";
    return;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  // First parse to check for manifest
  auto preParser = Parser::createStringParser(source);
  preParser.setFilePath(canonical);
  auto preAst = preParser.parseProgram();

  if (const auto* manifest = findManifest(*preAst)) {
    // Manifest found - collect all dependencies
    std::vector<std::string> sunFiles;
    std::vector<sun::MoonImport> moonImports = moonImports_;

    processManifest(*manifest, baseDirPath, sunFiles, moonImports);

    // Add the entrypoint file itself
    sunFiles.insert(sunFiles.begin(), canonical);

    // Use merged compilation
    executeFiles(sunFiles, moonImports, argc, argv);
  } else {
    // No manifest - use single-file execution
    baseDir = baseDirPath;
    importedFiles->insert(canonical);
    executeString(source, argc, argv, filePath.string());
  }
}

void Driver::compileString(const std::string& source,
                           const std::string& filePath) {
  std::string effectivePath = filePath;
  if (!filePath.empty()) {
    std::filesystem::path sourcePath = std::filesystem::absolute(filePath);
    baseDir = sourcePath.parent_path().string();
    effectivePath = sourcePath.string();
    if (std::filesystem::exists(sourcePath)) {
      importedFiles->insert(std::filesystem::canonical(sourcePath).string());
    } else {
      importedFiles->insert(sourcePath.lexically_normal().string());
    }
  } else {
    // Anonymous source
    effectivePath = SourceManager::instance().addAnonymousSource(source);
  }

  // Register source for error reporting
  SourceManager::instance().addSource(effectivePath, source);

  auto parser = Parser::createStringParser(source);
  parser.setImportedFiles(importedFiles);
  parser.setBaseDir(baseDir);
  if (!filePath.empty()) {
    parser.setFilePath(filePath);
  } else {
    parser.setFilePath(effectivePath);
  }

  auto blockAst = parser.parseProgram();
  runPipeline(std::move(blockAst), parser, false);
}

void Driver::compileFile(const std::string& filename) {
  std::filesystem::path filePath = std::filesystem::absolute(filename);
  std::string baseDirPath = filePath.parent_path().string();
  std::string canonical = std::filesystem::canonical(filePath).string();

  std::ifstream file(filename);
  if (!file.is_open()) {
    llvm::errs() << "Error: Could not open file '" << filename << "'\n";
    return;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  // First parse to check for manifest
  auto preParser = Parser::createStringParser(source);
  preParser.setFilePath(canonical);
  auto preAst = preParser.parseProgram();

  if (const auto* manifest = findManifest(*preAst)) {
    // Manifest found - collect all dependencies
    std::vector<std::string> sunFiles;
    std::vector<sun::MoonImport> moonImports = moonImports_;

    processManifest(*manifest, baseDirPath, sunFiles, moonImports);

    // Add the entrypoint file itself
    sunFiles.insert(sunFiles.begin(), canonical);

    // Use merged compilation
    compileFiles(sunFiles, moonImports);
  } else {
    // No manifest - use single-file compilation
    baseDir = baseDirPath;
    importedFiles->insert(canonical);
    compileString(source, filePath.string());
  }
}

// ---------------------------------------------------------------------------
// Merged-AST compilation: compile multiple source files together
// ---------------------------------------------------------------------------

/// Process MoonScopeASTs: detect hash duplicates, check for module name
/// collisions, and consolidate same-named modules.
/// Returns a vector of processed MoonScopeASTs (deduplicated).
static std::vector<std::unique_ptr<ExprAST>> processMoonScopes(
    std::vector<std::unique_ptr<ExprAST>>& moonStubs) {
  std::vector<std::unique_ptr<ExprAST>> result;

  // Track seen content hashes for deduplication
  std::unordered_set<std::string> seenHashes;

  // Track which moon provides each top-level module name (for collision
  // detection) Maps module_name -> (moon_path, content_hash)
  std::unordered_map<std::string, std::pair<std::string, std::string>>
      moduleProviders;

  for (auto& stub : moonStubs) {
    if (stub->getType() != ASTNodeType::MOON_SCOPE) {
      // Pass through non-MoonScope nodes unchanged
      result.push_back(std::move(stub));
      continue;
    }

    auto& moonScope = static_cast<MoonScopeAST&>(*stub);
    const std::string& hash = moonScope.getContentHash();
    const std::string& moonPath = moonScope.getMoonPath();

    // Skip duplicate moons (same content hash)
    if (!seenHashes.insert(hash).second) {
      continue;
    }

    // Check for module name collisions
    for (const auto& bodyExpr : moonScope.getBody().getBody()) {
      if (bodyExpr->getType() == ASTNodeType::MODULE) {
        auto& mod = static_cast<ModuleAST&>(*bodyExpr);
        const std::string& modName = mod.getName();

        auto it = moduleProviders.find(modName);
        if (it != moduleProviders.end()) {
          // Check if it's from a different moon (different hash)
          if (it->second.second != hash) {
            logAndThrowError("Module name collision: module '" + modName +
                             "' is provided by both '" + it->second.first +
                             "' and '" + moonPath +
                             "'. Use module aliasing to resolve.");
          }
        } else {
          moduleProviders[modName] = {moonPath, hash};
        }
      }
    }

    result.push_back(std::move(stub));
  }

  return result;
}

/// Merge multiple parsed BlockExprASTs into a single unified AST.
/// Same-named modules are merged together.
static std::unique_ptr<BlockExprAST> mergeASTs(
    std::vector<std::unique_ptr<BlockExprAST>>& parsedFiles,
    const std::vector<std::string>& filePaths) {
  std::vector<std::unique_ptr<ExprAST>> mergedBody;

  // Track modules by name so we can merge same-named modules
  std::unordered_map<std::string, std::vector<std::unique_ptr<ExprAST>>>
      moduleContents;

  // Track non-module statements separately so we can order them after modules
  std::vector<std::unique_ptr<ExprAST>> nonModuleStatements;

  for (size_t fileIdx = 0; fileIdx < parsedFiles.size(); ++fileIdx) {
    auto& fileAST = parsedFiles[fileIdx];
    if (!fileAST) continue;

    auto& body =
        const_cast<std::vector<std::unique_ptr<ExprAST>>&>(fileAST->getBody());

    for (auto& stmt : body) {
      if (!stmt) continue;

      if (stmt->getType() == ASTNodeType::MODULE) {
        // Collect module contents for merging
        auto& mod = static_cast<ModuleAST&>(*stmt);
        const std::string& modName = mod.getName();

        // Move the module body statements to our collection
        auto& modBody = const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
            mod.getBody().getBody());
        for (auto& modStmt : modBody) {
          if (modStmt) {
            moduleContents[modName].push_back(std::move(modStmt));
          }
        }
      } else {
        // Non-module top-level statement - collect separately
        nonModuleStatements.push_back(std::move(stmt));
      }
    }
  }

  // First add merged modules (so they're defined before using statements)
  for (auto& [modName, contents] : moduleContents) {
    auto modBody = std::make_unique<BlockExprAST>(std::move(contents));
    auto mergedMod = std::make_unique<ModuleAST>(modName, std::move(modBody));
    mergedBody.push_back(std::move(mergedMod));
  }

  // Then add non-module statements (using, functions, etc.)
  for (auto& stmt : nonModuleStatements) {
    mergedBody.push_back(std::move(stmt));
  }

  return std::make_unique<BlockExprAST>(std::move(mergedBody));
}

void Driver::compileFiles(const std::vector<std::string>& sourceFiles,
                          const std::vector<sun::MoonImport>& moonImports) {
  if (sourceFiles.empty()) {
    throw SunError(SunError::Kind::Parse, "No source files specified");
  }

  // Parse all source files independently
  std::vector<std::unique_ptr<BlockExprAST>> parsedFiles;
  std::vector<std::string> canonicalPaths;
  parsedFiles.reserve(sourceFiles.size());
  canonicalPaths.reserve(sourceFiles.size());

  for (const auto& filename : sourceFiles) {
    std::filesystem::path filePath = std::filesystem::absolute(filename);
    std::string canonical = std::filesystem::canonical(filePath).string();
    canonicalPaths.push_back(canonical);

    // Track in importedFiles to prevent re-processing
    importedFiles->insert(canonical);

    // Read file
    std::ifstream file(filename);
    if (!file.is_open()) {
      throw SunError(SunError::Kind::Parse,
                     "Could not open file '" + filename + "'");
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    // Register source for error reporting
    SourceManager::instance().addSource(canonical, source);

    // Parse (imports always error now - we use merged compilation)
    auto parser = Parser::createStringParser(source);
    parser.setImportedFiles(importedFiles);
    parser.setBaseDir(filePath.parent_path().string());
    parser.setFilePath(canonical);

    auto blockAst = parser.parseProgram();
    if (!blockAst) {
      throw SunError(SunError::Kind::Parse, "Failed to parse " + filename);
    }
    parsedFiles.push_back(std::move(blockAst));
  }

  // Merge all parsed files into a single AST
  auto mergedAst = mergeASTs(parsedFiles, canonicalPaths);

  // Inject AST stubs from moon imports before semantic analysis
  // This allows the semantic analyzer to know about symbols from moon libraries
  if (!moonImports.empty()) {
    // Create a temporary parser for collecting moon stubs
    // Important: use fresh importedFiles so stubs aren't skipped
    auto stubParser = Parser::createStringParser("");
    auto freshImportedFiles = std::make_shared<std::set<std::string>>();
    stubParser.setImportedFiles(freshImportedFiles);
    std::vector<std::unique_ptr<ExprAST>> moonStubs;
    for (const auto& moonImport : moonImports) {
      if (auto moonScope = stubParser.collectMoonImport(moonImport)) {
        moonStubs.push_back(std::move(moonScope));
      }
    }
    if (!moonStubs.empty()) {
      // Process: deduplicate by hash, check for collisions
      auto processed = processMoonScopes(moonStubs);
      mergedAst->prependExpressions(std::move(processed));
    }
  }

  // Debug mode: generate AST DOT graph
  if (debugMode_ && !debugFolder_.empty()) {
    AstDotGenerator dotGen;
    std::string dot = dotGen.generate(mergedAst.get());
    std::string dotPath = debugFolder_ + "/ast.dot";
    std::ofstream dotFile(dotPath);
    if (dotFile) {
      dotFile << dot;
      llvm::outs() << "  Generated: " << dotPath << "\n";
    }
  }

  // Run semantic analysis on the unified AST
  analyzer->analyzeBlock(*mergedAst);

  // Debug mode: generate scope tree HTML
  if (debugMode_ && !debugFolder_.empty()) {
    ScopeTreeGenerator scopeGen;
    std::string html = scopeGen.generateHtml(analyzer->getRootScope());
    std::string scopePath = debugFolder_ + "/scope_tree.html";
    std::ofstream scopeFile(scopePath);
    if (scopeFile) {
      scopeFile << html;
      llvm::outs() << "  Generated: " << scopePath << "\n";
    }
  }

  // Run borrow checking
  sun::BorrowChecker borrowChecker;
  auto borrowErrors = borrowChecker.check(*mergedAst);
  if (!borrowErrors.empty()) {
    for (const auto& err : borrowErrors) {
      std::cerr << err.format() << "\n";
    }
    throw SunError(SunError::Kind::Semantic,
                   "Borrow check failed with " +
                       std::to_string(borrowErrors.size()) + " error(s)");
  }

  // Set up module linker for moon imports (with aliasing support)
  sun::ModuleLinker linker(*ctx->mainModule);
  bool hasMoonImports = false;

  for (const auto& moonImport : moonImports) {
    // Use registerAvailableModulesWithRemap which handles aliasing
    linker.registerAvailableModulesWithRemap(moonImport);
    hasMoonImports = true;
  }

  if (hasMoonImports) {
    linker.declareAvailableFunctions();
  }

  // Snapshot precompiled function declarations
  codegenVisitor->snapshotPrecompiledFunctions();

  // Generate code
  codegenVisitor->codegen(*mergedAst);
  codegenVisitor->emitStaticInitFunction();

  // Link only used symbols
  if (hasMoonImports) {
    if (!linker.linkOnlyUsedSymbols()) {
      throw SunError(SunError::Kind::Semantic,
                     "Failed to link precompiled module: " + linker.getError());
    }
  }

  // Debug mode: dump IR
  if (debugMode_ && !debugFolder_.empty()) {
    std::string irPath = debugFolder_ + "/ir.ll";
    std::error_code EC;
    llvm::raw_fd_ostream irFile(irPath, EC);
    if (!EC) {
      ctx->mainModule->print(irFile, nullptr);
      llvm::outs() << "  Generated: " << irPath << "\n";
    }
  }

  // Verify the module
  if (llvm::verifyModule(*ctx->mainModule, &llvm::errs())) {
    llvm::errs() << "Error: Module verification failed\n";
  }
}

void Driver::executeFiles(const std::vector<std::string>& sourceFiles,
                          const std::vector<sun::MoonImport>& moonImports,
                          int argc, char** argv) {
  // For now, delegate to compileFiles then execute
  // This can be optimized later to avoid the extra JIT setup
  compileFiles(sourceFiles, moonImports);

  // JIT execution similar to runPipeline
  llvm::Function* func = ctx->mainModule->getFunction("main");
  if (!func) {
    llvm::errs() << "Error: Could not find 'main' function in module.\n";
    return;
  }

  // Get info before cloning
  llvm::Type* returnType = func->getReturnType();
  size_t mainArgCount = func->arg_size();

  // Clone module for JIT
  auto anonContext = std::make_unique<llvm::LLVMContext>();
  auto moduleClone = llvm::CloneModule(*ctx->mainModule);

  auto RT = ctx->jit->getMainJITDylib().createResourceTracker();
  llvm::ExitOnError ExitOnErr;
  ExitOnErr(
      ctx->jit->addModule(llvm::orc::ThreadSafeModule(std::move(moduleClone),
                                                      std::move(anonContext)),
                          RT));

  // Run static init
  if (auto initSym = ctx->jit->lookup("__sun_static_init")) {
    void (*initFP)() = initSym->getAddress().toPtr<void (*)()>();
    initFP();
  } else {
    llvm::consumeError(initSym.takeError());
  }

  // Execute main
  auto ExprSymbol = ExitOnErr(ctx->jit->lookup("main"));
  bool mainHasArgs = (mainArgCount == 2);

  if (returnType->isVoidTy()) {
    if (mainHasArgs) {
      void (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<void (*)(int, char**)>();
      FP(argc, argv);
    } else {
      void (*FP)() = ExprSymbol.getAddress().toPtr<void (*)()>();
      FP();
    }
  } else if (returnType->isIntegerTy(32)) {
    int32_t result;
    if (mainHasArgs) {
      int32_t (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<int32_t (*)(int, char**)>();
      result = FP(argc, argv);
    } else {
      int32_t (*FP)() = ExprSymbol.getAddress().toPtr<int32_t (*)()>();
      result = FP();
    }
    (void)result;  // Result returned via process exit code
  }

  ExitOnErr(RT->remove());
}
