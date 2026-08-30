#include "driver/driver.h"

#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "ast/manifest_ast.h"
#include "borrow_checker/borrow_checker.h"
#include "debug/ast_dot_generator.h"
#include "debug/scope_tree_generator.h"
#include "driver/manifest_processor.h"
#include "moon_bundling/library_cache.h"
#include "moon_bundling/module_linker.h"
#include "moon_bundling/proto_importer.h"
#include "parsing/lowering_pass.h"
#include "support/error.h"
#include "support/source_manager.h"
#include "support/stage_timer.h"
#include "support/sun_path.h"

static llvm::ExitOnError ExitOnErr;
using llvm::orc::ThreadSafeModule;

/// Strip library code the program never uses before handing a module to the
/// JIT. ORC eagerly compiles every defined function in an added module, so
/// linked-but-unused stdlib code would dominate JIT time. Internalize
/// everything except the entry points, then GlobalDCE drops whatever main
/// can't reach (references through vtables/globals are preserved).
static void stripUnreachableForJIT(llvm::Module& module) {
  for (auto& F : module) {
    // Global initializers need no exemption: they are internal already, and
    // llvm.global_ctors (appending linkage, never discarded) keeps them and
    // everything they call alive through GlobalDCE.
    if (!F.isDeclaration() && F.getName() != "main") {
      F.setLinkage(llvm::GlobalValue::InternalLinkage);
    }
  }
  for (auto& G : module.globals()) {
    if (!G.isDeclaration() && !G.getName().starts_with("llvm.")) {
      G.setLinkage(llvm::GlobalValue::InternalLinkage);
    }
  }

  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb;
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  llvm::ModulePassManager mpm;
  mpm.addPass(llvm::GlobalDCEPass());
  mpm.run(module, mam);
}

/// Make a module's global initializers callable under the JIT. They are
/// internal functions registered in llvm.global_ctors — one per linked module,
/// uniquified by the IR linker — and the JIT resolves symbols by name, which
/// cannot reach an internal function. So wrap every ctor entry in a single
/// external runner for the driver to look up and call before main, in the
/// same order the AOT init_array would use. Returns false when the module has
/// no constructors and there is nothing to run.
static bool wrapStaticCtorsForJIT(llvm::Module& module) {
  auto* ctors = module.getGlobalVariable("llvm.global_ctors");
  if (!ctors || !ctors->hasInitializer()) return false;
  auto* entries =
      llvm::dyn_cast<llvm::ConstantArray>(ctors->getInitializer());
  if (!entries) return false;

  // Each entry is { i32 priority, ptr function, ptr data }. Lower priority
  // runs first; entries with equal priority keep their link order.
  std::vector<std::pair<uint64_t, llvm::Function*>> fns;
  for (auto& op : entries->operands()) {
    auto* entry = llvm::cast<llvm::ConstantStruct>(op.get());
    auto* fn = llvm::dyn_cast<llvm::Function>(
        entry->getOperand(1)->stripPointerCasts());
    if (!fn || fn->isDeclaration()) continue;
    uint64_t priority =
        llvm::cast<llvm::ConstantInt>(entry->getOperand(0))->getZExtValue();
    fns.push_back({priority, fn});
  }
  if (fns.empty()) return false;
  std::stable_sort(fns.begin(), fns.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });

  auto* runnerType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(module.getContext()), false);
  auto* runner =
      llvm::Function::Create(runnerType, llvm::GlobalValue::ExternalLinkage,
                             "__sun_run_static_ctors", module);
  auto* entry =
      llvm::BasicBlock::Create(module.getContext(), "entry", runner);
  llvm::IRBuilder<> builder(entry);
  for (const auto& [priority, fn] : fns) builder.CreateCall(fn);
  builder.CreateRetVoid();
  return true;
}

/// Check if stdlib.moon is included in moon imports
static bool hasStdlibImport(const std::vector<sun::MoonImport>& moonImports) {
  for (const auto& moonImport : moonImports) {
    // Check if the path ends with stdlib.moon
    if (moonImport.path.find("stdlib.moon") != std::string::npos) {
      return true;
    }
  }
  return false;
}

/// Does this block declare `class String` directly inside `module std`?
/// Interpolation desugars to `std.String` and `std.HeapAllocator`, so the
/// stdlib's own sources satisfy it without importing stdlib.moon — which
/// they cannot do, being that library.
static bool declaresStdlibString(const BlockExprAST& block) {
  for (const auto& stmt : block.getBody()) {
    if (!stmt || stmt->getType() != ASTNodeType::MODULE) continue;
    const auto& module = static_cast<const ModuleAST&>(*stmt);
    if (module.getName() != "std") continue;
    for (const auto& member : module.getBody().getBody()) {
      if (!member || member->getType() != ASTNodeType::CLASS_DEFINITION) {
        continue;
      }
      if (static_cast<const ClassDefinitionAST&>(*member).getName() ==
          "String") {
        return true;
      }
    }
  }
  return false;
}

// Factory method for JIT execution
std::unique_ptr<Driver> Driver::createForJIT(const std::string& moduleName,
                                             bool debugInfo) {
  ensureLLVMInitialized();

  // JIT always runs on the host; .moon bundle selection must match.
  sun::LibraryCache::instance().setTargetTriple("");

  auto jit = SunJIT::Create();
  if (!jit) {
    llvm::errs() << "Failed to create SunJIT: " << toString(jit.takeError())
                 << "\n";
    std::abort();
  }

  auto jitShared = std::shared_ptr<SunJIT>(std::move(jit.get()));
  auto ctx = std::make_unique<CodegenContext>(moduleName, jitShared,
                                              /*existingContext=*/nullptr,
                                              /*targetTriple=*/"", debugInfo);

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
std::unique_ptr<Driver> Driver::createForAOT(const std::string& moduleName,
                                             const std::string& targetTriple,
                                             bool debugInfo) {
  ensureLLVMInitialized();

  // Both the parser's bundle resolution and the linker's bundle selection
  // key off this; setting it here keeps API users consistent with the CLI.
  sun::LibraryCache::instance().setTargetTriple(targetTriple);

  auto ctx = std::make_unique<CodegenContext>(moduleName, nullptr,
                                              /*existingContext=*/nullptr,
                                              targetTriple, debugInfo);
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

// Helper to dump only user-defined globals and functions to any raw_ostream.
// Used by both terminal printing (with colors) and debug file output.
void Driver::dumpUserDefinedIR(llvm::raw_ostream& OS) {
  // Get user-defined functions from codegen visitor
  const auto& userDefined = codegenVisitor->getUserDefinedFunctions();

  // Print user-defined global variables (skip library globals)
  for (auto& gv : ctx->mainModule->globals()) {
    std::string name = gv.getName().str();
    if (name.empty()) continue;
    // Skip internal symbols starting with _
    if (name[0] == '_') continue;
    // Skip library module globals (vtables, etc.)
    if (name.rfind("std_", 0) == 0) continue;
    // Skip prefixed symbols (from moon imports)
    if (name.rfind("$", 0) == 0) continue;
    gv.print(OS);
    OS << "\n";
  }

  // Print only user-defined functions
  for (auto& func : ctx->mainModule->functions()) {
    std::string name = func.getName().str();
    // Skip declarations (no body)
    if (func.isDeclaration()) continue;
    // Only print functions that are user-defined
    if (userDefined.count(name) == 0) continue;

    func.print(OS);
    OS << "\n";
  }
}

// Print only IR for user-defined functions (filter out library code)
void Driver::printUserDefinedIR() {
  // ANSI color codes for cyan output
  const char* cyan = "\033[36m";
  const char* reset = "\033[0m";

  llvm::outs() << cyan << "; LLVM IR (user-defined only):\n";
  dumpUserDefinedIR(llvm::outs());
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

  // Global initializers are reachable through llvm.global_ctors, not main
  if (auto* ctors = ctx->mainModule->getGlobalVariable("llvm.global_ctors")) {
    if (ctors->hasInitializer()) {
      if (auto* entries =
              llvm::dyn_cast<llvm::ConstantArray>(ctors->getInitializer())) {
        for (auto& op : entries->operands()) {
          auto* entry = llvm::cast<llvm::ConstantStruct>(op.get());
          if (auto* fn = llvm::dyn_cast<llvm::Function>(
                  entry->getOperand(1)->stripPointerCasts())) {
            collectReachableFunctions(fn, reachable);
          }
        }
      }
    }
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

// Write only the user-defined portion of the IR to a file (used in debug mode).
// No ANSI colors, plain text suitable for .ll file.
void Driver::writeUserDefinedIR(const std::string& path) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(path, EC);
  if (EC) {
    llvm::errs() << "Warning: Could not write " << path << ": " << EC.message()
                 << "\n";
    return;
  }

  OS << "; LLVM IR (user-defined only)\n";
  OS << "; Generated in debug mode - library and imported symbols filtered "
        "out\n\n";

  dumpUserDefinedIR(OS);
}

// ---------------------------------------------------------------------------
// Moon import processing
// ---------------------------------------------------------------------------

/// Process moon imports: collect stubs, deduplicate, check for collisions
/// with source modules and between moons, then prepend to AST.
static void processMoonImports(
    BlockExprAST& blockAst, Parser& parser,
    const std::vector<sun::MoonImport>& moonImports) {
  if (moonImports.empty()) {
    return;
  }

  // Collect top-level module names from source files
  std::unordered_set<std::string> sourceModuleNames;
  for (const auto& stmt : blockAst.getBody()) {
    if (stmt && stmt->getType() == ASTNodeType::MODULE) {
      auto& mod = static_cast<const ModuleAST&>(*stmt);
      sourceModuleNames.insert(mod.getName());
    }
  }

  // Collect, deduplicate, and check for collisions in one pass
  std::vector<std::unique_ptr<ExprAST>> finalMoonScopeASTs;
  std::unordered_set<std::string> seenHashes;
  std::unordered_map<std::string, std::pair<std::string, std::string>>
      moduleProviders;  // module_name -> (moon_path, content_hash)

  for (const auto& moonImport : moonImports) {
    auto stub = parser.collectMoonImport(moonImport);
    if (!stub) {
      continue;
    }

    if (stub->getType() != ASTNodeType::MOON_SCOPE) {
      finalMoonScopeASTs.push_back(std::move(stub));
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

        // Check collision with source file modules
        if (sourceModuleNames.count(modName)) {
          logAndThrowError("Module name collision: module '" + modName +
                           "' from '" + moonPath +
                           "' conflicts with a module in the source files.");
        }

        // Check collision with other moon modules
        auto it = moduleProviders.find(modName);
        if (it != moduleProviders.end()) {
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

    finalMoonScopeASTs.push_back(std::move(stub));
  }

  if (!finalMoonScopeASTs.empty()) {
    blockAst.prependExpressions(std::move(finalMoonScopeASTs));
  }
}

void Driver::collectNativeArchives(const std::set<std::string>& linkedModules) {
  if (linkedModules.empty()) return;

  std::error_code ec;
  if (archiveTempDir_.empty()) {
    archiveTempDir_ = std::filesystem::temp_directory_path(ec) /
                      ("sun-archives-" + std::to_string(::getpid()));
    if (ec) {
      archiveTempDir_.clear();
      return;
    }
    std::filesystem::create_directories(archiveTempDir_, ec);
    if (ec) {
      archiveTempDir_.clear();
      return;
    }
  }

  nativeArchivePaths_ = sun::LibraryCache::instance().extractNativeArchives(
      linkedModules, archiveTempDir_);
}

// Try to load a shared library holding the same code as a bundled archive
// (libssl.a -> libssl.so.3, or libssl.3.dylib on macOS).
//
// On Linux the loader's own search path finds these, so a bare name is
// enough. macOS needs full paths: Apple ships no usable OpenSSL, and the
// bare names resolve to /usr/lib/libssl.dylib and libcrypto.dylib, which
// are compatibility stubs that print "loading libcrypto in an unsafe way"
// and abort the whole process the moment they are opened. Homebrew's and
// MacPorts' builds are the real thing, and sit outside the search path.
static bool loadSharedCounterpart(const std::filesystem::path& archive) {
  std::string stem = archive.stem().string();  // "libssl"
  if (stem.rfind("lib", 0) != 0) return false;

  std::vector<std::string> candidates;
#ifdef __APPLE__
  for (const char* dir : {"/opt/homebrew/opt/openssl@3/lib/",
                          "/usr/local/opt/openssl@3/lib/", "/opt/local/lib/"}) {
    for (const char* suffix : {".3.dylib", ".dylib"}) {
      candidates.push_back(dir + stem + suffix);
    }
  }
#else
  for (const char* suffix : {".so", ".so.3", ".so.1.1"}) {
    candidates.push_back(stem + suffix);
  }
#endif

  for (const auto& candidate : candidates) {
    if (!llvm::sys::DynamicLibrary::LoadLibraryPermanently(candidate.c_str(),
                                                           nullptr)) {
      return true;
    }
  }
  return false;
}

void Driver::registerArchivesWithJIT() {
  if (!ctx->jit || nativeArchivePaths_.empty()) return;

  // Prefer shared libraries whenever all of them are present. AOT linking
  // always uses the bundled archives, so shipped binaries stay
  // self-contained either way; this only decides what the JIT resolves
  // against.
  bool allShared = true;
  for (const auto& archive : nativeArchivePaths_) {
    if (!loadSharedCounterpart(archive)) {
      allShared = false;
      break;
    }
  }
  if (allShared) return;

  // Falling back to the bundle's own archives. This is a poor substitute:
  // a static library expects a real link, and the JIT can only resolve what
  // the running process already exports. One symbol it cannot find poisons
  // every archive member that needs it — glibc's `atexit`, for instance,
  // lives in libc_nonshared.a and is invisible to dlsym — and the failure
  // surfaces far away as "failed to materialize" errors naming unrelated
  // symbols. Say where it came from so that is not a mystery.
  llvm::errs() << "Warning: no shared library found for the archives this "
                  "program's bundles carry. Linking them into the JIT "
                  "instead, which may not resolve every symbol; compiling "
                  "with -c always uses the archives and is unaffected.\n";
#ifdef __APPLE__
  llvm::errs() << "  For TLS, `brew install openssl@3` supplies one.\n";
#endif

  for (const auto& archive : nativeArchivePaths_) {
    if (auto err = ctx->jit->addStaticLibrary(archive)) {
      llvm::errs() << "Warning: could not load bundled library '" << archive
                   << "': " << llvm::toString(std::move(err)) << "\n";
    }
  }
}

void Driver::addJITStaticLibrary(const std::string& path) {
  if (!ctx->jit) return;
  if (auto err = ctx->jit->addStaticLibrary(path)) {
    logAndThrowError("cannot load static archive '" + path +
                     "': " + llvm::toString(std::move(err)));
  }
}

void Driver::analyzeProgram(BlockExprAST& blockAst, Parser& parser) {
  // Lower the lossless parse tree into the core AST before semantic analysis
  LoweringPass lowering;
  {
    sun::ScopedStage stage("lowering");
    lowering.run(blockAst);
  }

  // Interpolation desugars to std.String / std.HeapAllocator, so those types
  // have to come from somewhere: an imported stdlib.moon, or — when
  // compiling the standard library itself — its own sources.
  if ((parser.usesStringInterpolation() || lowering.usedInterpolation()) &&
      !hasStdlibImport(moonImports_) && !declaresStdlibString(blockAst)) {
    logAndThrowError(
        "String interpolation requires the standard library. Add "
        "'moon \"stdlib.moon\"' to your manifest or use --moon stdlib.moon");
  }

  // Inject AST stubs from moon imports before semantic analysis
  {
    sun::ScopedStage stage("moon imports");
    processMoonImports(blockAst, parser, moonImports_);
  }

  // Run semantic analysis on the unified AST
  {
    sun::ScopedStage stage("sema");
    analyzer->analyzeBlock(blockAst);
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

  // Debug mode: generate AST DOT graph (pre-lowering, lossless parse tree)
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

  analyzeProgram(*blockAst, parser);

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
  std::vector<sun::BorrowError> borrowErrors;
  {
    sun::ScopedStage stage("borrow check");
    borrowErrors = borrowChecker.check(*blockAst);
  }
  if (!borrowErrors.empty()) {
    // Render each error in the standard compiler format (source line and
    // caret), with a note under it for each related borrow location
    for (const auto& err : borrowErrors) {
      auto [line, prev] =
          SourceManager::instance().getLineWithContext(err.location);
      std::cerr << formatDiagnostic("Borrow Error", ansi::red, err.message,
                                    err.location, line, prev)
                << "\n";
      for (const auto& rel : err.relatedLocations) {
        auto [relLine, relPrev] =
            SourceManager::instance().getLineWithContext(rel);
        std::cerr << formatDiagnostic("Note", ansi::cyan,
                                      "related borrow here", rel, relLine,
                                      relPrev)
                  << "\n";
      }
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
  {
    sun::ScopedStage stage("codegen");
    codegenVisitor->codegen(*blockAst);
    // Emit static initialization function for globals that need runtime init
    codegenVisitor->emitStaticInitFunction();
  }

  // Link only the modules that provide symbols actually used by the code
  // This happens AFTER codegen so we know exactly which symbols are needed
  if (hasMoonImports) {
    sun::ScopedStage stage("moon link");
    if (!linker.linkOnlyUsedSymbols()) {
      throw SunError(SunError::Kind::Semantic,
                     "Failed to link precompiled module: " + linker.getError());
    }
    // Precompiled bundles may carry debug info (stdlib is built with -g); a
    // non-debug compile must not inherit it — it would bloat the binary and
    // flip the backend to CodeGenOptLevel::None.
    if (!ctx->debugInfoEnabled()) {
      sun::DebugInfoBuilder::stripFromModule(*ctx->mainModule);
    }
    // A bundle binding a C library carries that library's static archives;
    // put them on disk so the link (or the JIT) can use them.
    collectNativeArchives(linker.getLinkedModules());
  }

  // All codegen is done (including static init); emit the DI finalization
  // before any module verification.
  codegenVisitor->finalizeDebugInfo();

  // Debug mode: dump only user-defined IR after codegen (filters out stdlib /
  // moon imports)
  if (debugMode_ && !debugFolder_.empty()) {
    std::string irPath = debugFolder_ + "/ir.ll";
    writeUserDefinedIR(irPath);
    llvm::outs() << "  Generated: " << irPath << "\n";
  }

  // If not executing, handle AOT compilation specifics
  if (!execute) {
    // Handle main function return type for compiled programs
    if (auto* mainFunc = ctx->mainModule->getFunction("main")) {
      llvm::Type* retType = mainFunc->getReturnType();
      if (retType->isVoidTy()) {
        // Wrap void main() to return i32 0
        mainFunc->setName("__sun_main_void");
        // The user-defined set is keyed by name, so the program's own body
        // would drop out of an IR dump under its new name — leaving the dump
        // showing only the wrapper, calling a function that isn't there.
        codegenVisitor->noteUserDefinedFunction("__sun_main_void");
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

    // Verify the module - invalid IR is a hard compile failure
    {
      sun::ScopedStage stage("verify");
      if (llvm::verifyModule(*ctx->mainModule, &llvm::errs())) {
        throw SunError(SunError::Kind::Compile, "Module verification failed");
      }
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

  // Verify before executing - never run invalid IR
  {
    sun::ScopedStage stage("verify");
    if (llvm::verifyModule(*ctx->mainModule, &llvm::errs())) {
      throw SunError(SunError::Kind::Compile, "Module verification failed");
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
  auto jitStage = std::make_unique<sun::ScopedStage>("jit compile");
  auto anonContext = std::make_unique<llvm::LLVMContext>();

  // Clone the module into the new context
  auto moduleClone = llvm::CloneModule(*ctx->mainModule);
  stripUnreachableForJIT(*moduleClone);
  bool hasStaticCtors = wrapStaticCtorsForJIT(*moduleClone);

  // Add the cloned module to JIT with its own context
  // Archives carried by imported bundles resolve like linked libraries
  registerArchivesWithJIT();

  auto RT = ctx->jit->getMainJITDylib().createResourceTracker();
  ExitOnErr(ctx->jit->addModule(
      ThreadSafeModule(std::move(moduleClone), std::move(anonContext)), RT));

  // Run global initializers (the program's and every linked bundle's) before
  // main, the way the AOT init_array would
  if (hasStaticCtors) {
    auto initSym = ExitOnErr(ctx->jit->lookup("__sun_run_static_ctors"));
    void (*initFP)() = initSym.getAddress().toPtr<void (*)()>();
    initFP();
  }

  // Lookup and execute with appropriate type
  auto ExprSymbol = ExitOnErr(ctx->jit->lookup("main"));

  jitStage.reset();

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
  } else {
    // Anonymous source (e.g., from executeString in tests)
    effectivePath = SourceManager::instance().addAnonymousSource(source);
  }

  // Register source for error reporting
  if (!filePath.empty()) {
    SourceManager::instance().addSource(effectivePath, source);
  }

  auto parser = Parser::createStringParser(source);
  parser.setBaseDir(baseDir);
  if (!filePath.empty()) {
    parser.setFilePath(filePath);
  } else {
    parser.setFilePath(effectivePath);
  }

  auto blockAst = parser.parseProgram();
  return runPipeline(std::move(blockAst), parser, true, argc, argv);
}

sun::SunValue Driver::executeFile(const std::string& filename, int argc,
                                  char** argv) {
  std::filesystem::path filePath = std::filesystem::absolute(filename);
  std::string baseDirPath = filePath.parent_path().string();
  std::string canonical = std::filesystem::canonical(filePath).string();

  std::ifstream file(filename);
  if (!file.is_open()) {
    llvm::errs() << "Error: Could not open file '" << filename << "'\n";
    return sun::VoidValue{};
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  // First parse to check for manifest
  auto preParser = Parser::createStringParser(source);
  preParser.setFilePath(canonical);
  auto preAst = preParser.parseProgram();

  std::vector<std::string> sunFiles;
  std::vector<sun::MoonImport> moonImports = moonImports_;
  std::vector<std::string> protoFiles = protoFiles_;

  if (const auto* manifest = sun::ManifestProcessor::findManifest(*preAst)) {
    // Manifest found - collect all dependencies. The module's triple picks
    // which target: blocks apply (the host's for JIT execution).
    auto resolved = sun::ManifestProcessor::process(
        *manifest, baseDirPath, ctx->mainModule->getTargetTriple());
    sunFiles = std::move(resolved.sunFiles);
    moonImports.insert(moonImports.end(), resolved.moonImports.begin(),
                       resolved.moonImports.end());
    protoFiles.insert(protoFiles.end(), resolved.protoFiles.begin(),
                      resolved.protoFiles.end());
  }

  // Add the entrypoint file itself
  sunFiles.insert(sunFiles.begin(), canonical);

  // Use merged compilation
  return executeFiles(sunFiles, moonImports, argc, argv, protoFiles);
}

Parser Driver::prepareStringParser(const std::string& source,
                                   const std::string& filePath) {
  std::string effectivePath = filePath;
  if (!filePath.empty()) {
    std::filesystem::path sourcePath = std::filesystem::absolute(filePath);
    baseDir = sourcePath.parent_path().string();
    effectivePath = sourcePath.string();
  } else {
    // Anonymous source
    effectivePath = SourceManager::instance().addAnonymousSource(source);
  }

  // Register source for error reporting
  SourceManager::instance().addSource(effectivePath, source);

  auto parser = Parser::createStringParser(source);
  parser.setBaseDir(baseDir);
  if (!filePath.empty()) {
    parser.setFilePath(filePath);
  } else {
    parser.setFilePath(effectivePath);
  }
  return parser;
}

void Driver::compileString(const std::string& source,
                           const std::string& filePath) {
  auto parser = prepareStringParser(source, filePath);
  auto blockAst = parser.parseProgram();
  runPipeline(std::move(blockAst), parser, false);
}

Driver::AnalyzedProgram Driver::analyzeString(const std::string& source,
                                              const std::string& filePath) {
  AnalyzedProgram result;
  try {
    auto parser = prepareStringParser(source, filePath);
    result.ast = parser.parseProgram();
    if (result.ast) analyzeProgram(*result.ast, parser);
  } catch (const SunError& error) {
    result.error = error;
  }
  return result;
}

Driver::AnalyzedProgram Driver::analyzeFiles(
    const std::vector<std::string>& sourceFiles,
    const std::vector<sun::MoonImport>& moonImports,
    const std::vector<std::string>& protoFiles,
    const std::map<std::string, std::string>& sourceOverrides) {
  AnalyzedProgram result;
  moonImports_ = moonImports;
  try {
    result.ast = parseAndMergeFiles(sourceFiles, protoFiles, sourceOverrides);
    auto stubParser = Parser::createStringParser("");
    analyzeProgram(*result.ast, stubParser);
  } catch (const SunError& error) {
    result.error = error;
  }
  return result;
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

  std::vector<std::string> sunFiles;
  std::vector<sun::MoonImport> moonImports = moonImports_;
  std::vector<std::string> protoFiles = protoFiles_;

  if (const auto* manifest = sun::ManifestProcessor::findManifest(*preAst)) {
    // Manifest found - collect all dependencies. The module's triple picks
    // which target: blocks apply (the host's for JIT execution).
    auto resolved = sun::ManifestProcessor::process(
        *manifest, baseDirPath, ctx->mainModule->getTargetTriple());
    sunFiles = std::move(resolved.sunFiles);
    moonImports.insert(moonImports.end(), resolved.moonImports.begin(),
                       resolved.moonImports.end());
    protoFiles.insert(protoFiles.end(), resolved.protoFiles.begin(),
                      resolved.protoFiles.end());
  }

  // Add the entrypoint file itself
  sunFiles.insert(sunFiles.begin(), canonical);

  // Use merged compilation
  compileFiles(sunFiles, moonImports, protoFiles);
}

// ---------------------------------------------------------------------------
// Merged-AST compilation: compile multiple source files together
// ---------------------------------------------------------------------------

/// Merge multiple parsed BlockExprASTs into a single unified AST.
/// Same-named modules are merged together.
static std::unique_ptr<BlockExprAST> mergeASTs(
    std::vector<std::unique_ptr<BlockExprAST>>& parsedFiles,
    const std::vector<std::string>& filePaths) {
  std::vector<std::unique_ptr<ExprAST>> mergedBody;

  // Track modules by name so we can merge same-named modules. Emission keeps
  // first-seen order: declaration order across modules must stay stable
  // (a class field of a type from a sibling module resolves in the
  // declaration pre-pass in this order).
  std::unordered_map<std::string, std::vector<std::unique_ptr<ExprAST>>>
      moduleContents;
  std::vector<std::string> moduleOrder;

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
        if (!moduleContents.count(modName)) moduleOrder.push_back(modName);
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
  for (const auto& modName : moduleOrder) {
    auto& contents = moduleContents[modName];
    auto modBody =
        std::make_unique<BlockExprAST>(std::move(contents), BlockKind::Module);
    auto mergedMod = std::make_unique<ModuleAST>(modName, std::move(modBody));
    mergedBody.push_back(std::move(mergedMod));
  }

  // Then add non-module statements (using, functions, etc.)
  for (auto& stmt : nonModuleStatements) {
    mergedBody.push_back(std::move(stmt));
  }

  return std::make_unique<BlockExprAST>(std::move(mergedBody),
                                        BlockKind::Module);
}

void Driver::parseSynthesizedProtoModules(
    const std::vector<std::string>& protoFiles,
    std::vector<std::unique_ptr<BlockExprAST>>& parsedFiles,
    std::vector<std::string>& canonicalPaths) {
  for (const auto& synthesized :
       sun::ProtoImporter::importAll(protoFiles, baseDir)) {
    // Registered under its pseudo-path so diagnostics cite the .proto and
    // show the synthesized source line
    SourceManager::instance().addSource(synthesized.pseudoPath,
                                        synthesized.sunSource);
    if (dumpProtoSun_) {
      llvm::outs() << "// ==== " << synthesized.pseudoPath << " ====\n"
                   << synthesized.sunSource << "\n";
    }
    auto parser = Parser::createStringParser(synthesized.sunSource);
    parser.setFilePath(synthesized.pseudoPath);
    auto blockAst = parser.parseProgram();
    if (!blockAst) {
      throw SunError(
          SunError::Kind::Parse,
          "Failed to parse synthesized module for " + synthesized.pseudoPath);
    }
    canonicalPaths.push_back(synthesized.pseudoPath);
    parsedFiles.push_back(std::move(blockAst));
  }
}

std::unique_ptr<BlockExprAST> Driver::parseAndMergeFiles(
    const std::vector<std::string>& sourceFiles,
    const std::vector<std::string>& protoFiles,
    const std::map<std::string, std::string>& sourceOverrides) {
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

    std::string source;
    auto override = sourceOverrides.find(canonical);
    if (override != sourceOverrides.end()) {
      source = override->second;
    } else {
      std::ifstream file(filename);
      if (!file.is_open()) {
        throw SunError(SunError::Kind::Parse,
                       "Could not open file '" + filename + "'");
      }
      std::stringstream buffer;
      buffer << file.rdbuf();
      source = buffer.str();
    }

    // Register source for error reporting
    SourceManager::instance().addSource(canonical, source);

    // Parse (imports always error now - we use merged compilation)
    auto parser = Parser::createStringParser(source);
    parser.setFilePath(canonical);

    auto blockAst = parser.parseProgram();
    if (!blockAst) {
      throw SunError(SunError::Kind::Parse, "Failed to parse " + filename);
    }
    parsedFiles.push_back(std::move(blockAst));
  }

  // Native protobuf import: each .proto becomes ordinary Sun source, parsed
  // like any other file so the merged AST (and everything after it) sees
  // normal code
  parseSynthesizedProtoModules(protoFiles, parsedFiles, canonicalPaths);

  // Merge all parsed files into a single AST
  return mergeASTs(parsedFiles, canonicalPaths);
}

void Driver::compileFiles(const std::vector<std::string>& sourceFiles,
                          const std::vector<sun::MoonImport>& moonImports,
                          const std::vector<std::string>& protoFiles) {
  auto mergedAst = parseAndMergeFiles(sourceFiles, protoFiles, {});

  // Create a parser for runPipeline (used for precompiled imports lookup)
  auto stubParser = Parser::createStringParser("");

  // Set moonImports_ so runPipeline can use them for stub injection and linker
  // setup
  moonImports_ = moonImports;

  // Run the shared compilation pipeline
  runPipeline(std::move(mergedAst), stubParser, false);
}

sun::SunValue Driver::executeFiles(
    const std::vector<std::string>& sourceFiles,
    const std::vector<sun::MoonImport>& moonImports, int argc, char** argv,
    const std::vector<std::string>& protoFiles) {
  // For now, delegate to compileFiles then execute
  // This can be optimized later to avoid the extra JIT setup
  compileFiles(sourceFiles, moonImports, protoFiles);
  sun::SunValue result = sun::VoidValue{};

  // JIT execution similar to runPipeline
  llvm::Function* func = ctx->mainModule->getFunction("main");
  if (!func) {
    llvm::errs() << "Error: Could not find 'main' function in module.\n";
    return result;
  }

  // Get info before cloning
  llvm::Type* returnType = func->getReturnType();
  size_t mainArgCount = func->arg_size();

  // Clone module for JIT
  auto jitStage = std::make_unique<sun::ScopedStage>("jit compile");
  auto anonContext = std::make_unique<llvm::LLVMContext>();
  auto moduleClone = llvm::CloneModule(*ctx->mainModule);
  stripUnreachableForJIT(*moduleClone);
  bool hasStaticCtors = wrapStaticCtorsForJIT(*moduleClone);

  // Archives carried by imported bundles resolve like linked libraries
  registerArchivesWithJIT();

  auto RT = ctx->jit->getMainJITDylib().createResourceTracker();
  llvm::ExitOnError ExitOnErr;
  ExitOnErr(
      ctx->jit->addModule(llvm::orc::ThreadSafeModule(std::move(moduleClone),
                                                      std::move(anonContext)),
                          RT));

  // Run global initializers before main
  if (hasStaticCtors) {
    auto initSym = ExitOnErr(ctx->jit->lookup("__sun_run_static_ctors"));
    void (*initFP)() = initSym.getAddress().toPtr<void (*)()>();
    initFP();
  }

  // Execute main
  auto ExprSymbol = ExitOnErr(ctx->jit->lookup("main"));
  jitStage.reset();
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
    int32_t ret;
    if (mainHasArgs) {
      int32_t (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<int32_t (*)(int, char**)>();
      ret = FP(argc, argv);
    } else {
      int32_t (*FP)() = ExprSymbol.getAddress().toPtr<int32_t (*)()>();
      ret = FP();
    }
    result = ret;
  }

  ExitOnErr(RT->remove());
  return result;
}
