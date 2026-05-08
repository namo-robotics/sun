// src/driver.cpp
#include "driver.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "borrow_checker/borrow_checker.h"
#include "error.h"
#include "library_cache.h"
#include "module_linker.h"
#include "source_manager.h"

static llvm::ExitOnError ExitOnErr;
using llvm::orc::ThreadSafeModule;

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

// -------------------------------------------------------------------
// Recursively expand all ImportAST nodes in the AST tree into
// ImportScopeAST nodes, at any depth (top-level, inside functions, etc.)
// -------------------------------------------------------------------
static void expandAllImports(ExprAST& expr, Parser& parser,
                             std::set<std::string>& cycleStack);

static void expandImportsInBlock(BlockExprAST& block, Parser& parser,
                                 std::set<std::string>& cycleStack) {
  auto& body =
      const_cast<std::vector<std::unique_ptr<ExprAST>>&>(block.getBody());

  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] && body[i]->isImport()) {
      const auto& importStmt = static_cast<const ImportAST&>(*body[i]);
      body[i] = parser.expandImport(importStmt.getPath(), cycleStack);
    }
    if (body[i]) {
      expandAllImports(*body[i], parser, cycleStack);
    }
  }
}

static void expandAllImports(ExprAST& expr, Parser& parser,
                             std::set<std::string>& cycleStack) {
  switch (expr.getType()) {
    case ASTNodeType::BLOCK: {
      expandImportsInBlock(static_cast<BlockExprAST&>(expr), parser,
                           cycleStack);
      break;
    }
    case ASTNodeType::FUNCTION: {
      auto& func = static_cast<FunctionAST&>(expr);
      if (func.hasBody()) {
        expandImportsInBlock(const_cast<BlockExprAST&>(func.getBody()), parser,
                             cycleStack);
      }
      break;
    }
    case ASTNodeType::LAMBDA: {
      auto& lambda = static_cast<LambdaAST&>(expr);
      if (lambda.hasBody()) {
        expandImportsInBlock(const_cast<BlockExprAST&>(lambda.getBody()),
                             parser, cycleStack);
      }
      break;
    }
    case ASTNodeType::IMPORT_SCOPE: {
      auto& scope = static_cast<ImportScopeAST&>(expr);
      expandImportsInBlock(const_cast<BlockExprAST&>(scope.getBody()), parser,
                           cycleStack);
      break;
    }
    case ASTNodeType::NAMESPACE: {
      auto& ns = static_cast<NamespaceAST&>(expr);
      expandImportsInBlock(const_cast<BlockExprAST&>(ns.getBody()), parser,
                           cycleStack);
      break;
    }
    case ASTNodeType::CLASS_DEFINITION: {
      auto& classDef = static_cast<ClassDefinitionAST&>(expr);
      for (const auto& method : classDef.getMethods()) {
        if (method.function && method.function->hasBody()) {
          expandImportsInBlock(
              const_cast<BlockExprAST&>(method.function->getBody()), parser,
              cycleStack);
        }
      }
      break;
    }
    case ASTNodeType::IF: {
      auto& ifExpr = static_cast<IfExprAST&>(expr);
      if (ifExpr.getThen())
        expandAllImports(*ifExpr.getThen(), parser, cycleStack);
      if (ifExpr.getElse())
        expandAllImports(*ifExpr.getElse(), parser, cycleStack);
      break;
    }
    case ASTNodeType::WHILE_LOOP: {
      auto& whileExpr = static_cast<WhileExprAST&>(expr);
      if (whileExpr.getBody())
        expandAllImports(*const_cast<ExprAST*>(whileExpr.getBody()), parser,
                         cycleStack);
      break;
    }
    case ASTNodeType::FOR_LOOP: {
      auto& forExpr = static_cast<ForExprAST&>(expr);
      if (forExpr.getBody())
        expandAllImports(*const_cast<ExprAST*>(forExpr.getBody()), parser,
                         cycleStack);
      break;
    }
    case ASTNodeType::FOR_IN_LOOP: {
      auto& forInExpr = static_cast<ForInExprAST&>(expr);
      if (forInExpr.getBody())
        expandAllImports(*const_cast<ExprAST*>(forInExpr.getBody()), parser,
                         cycleStack);
      break;
    }
    case ASTNodeType::TRY_CATCH: {
      auto& tryCatch = static_cast<TryCatchExprAST&>(expr);
      expandImportsInBlock(const_cast<BlockExprAST&>(tryCatch.getTryBlock()),
                           parser, cycleStack);
      expandImportsInBlock(
          const_cast<BlockExprAST&>(*tryCatch.getCatchClause().body), parser,
          cycleStack);
      break;
    }
    case ASTNodeType::MATCH: {
      auto& matchExpr = static_cast<MatchExprAST&>(expr);
      for (const auto& arm : matchExpr.getArms()) {
        if (arm.body) expandAllImports(*arm.body, parser, cycleStack);
      }
      break;
    }
    default:
      // Other node types don't contain blocks with imports
      break;
  }
}

sun::SunValue Driver::runPipeline(std::unique_ptr<BlockExprAST> blockAst,
                                  Parser& parser, bool execute, int argc,
                                  char** argv) {
  sun::SunValue result = sun::VoidValue{};

  if (!blockAst) {
    llvm::errs() << "Error: Failed to parse program.\n";
    return result;
  }

  // Expand imports in-place into ImportScopeAST nodes (at any AST depth)
  std::set<std::string> cycleStack;
  expandImportsInBlock(*blockAst, parser, cycleStack);

  // Run semantic analysis on the unified AST
  analyzer->analyzeBlock(*blockAst);

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
  if (!precompiledImports.empty()) {
    linker.registerAvailableModules(precompiledImports);
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
  if (!precompiledImports.empty()) {
    if (!linker.linkOnlyUsedSymbols()) {
      throw SunError(SunError::Kind::Semantic,
                     "Failed to link precompiled module: " + linker.getError());
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
    printUserDefinedIR();
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
  baseDir = filePath.parent_path().string();
  importedFiles->insert(std::filesystem::canonical(filePath).string());

  std::ifstream file(filename);
  if (!file.is_open()) {
    llvm::errs() << "Error: Could not open file '" << filename << "'\n";
    return;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  executeString(source, argc, argv, filePath.string());
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
  baseDir = filePath.parent_path().string();
  importedFiles->insert(std::filesystem::canonical(filePath).string());

  std::ifstream file(filename);
  if (!file.is_open()) {
    llvm::errs() << "Error: Could not open file '" << filename << "'\n";
    return;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  compileString(source, filePath.string());
}
