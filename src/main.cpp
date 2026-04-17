// main.cpp
#include <glob.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "compiler.h"
#include "driver.h"
#include "error.h"
#include "library_cache.h"
#include "module_linker.h"
#include "moon.h"
#include "parser.h"

/// Extract type signature from an AST type annotation
static std::string typeAnnotationToString(const TypeAnnotation& ta) {
  return ta.toString();
}

/// Extract type signature from an optional type annotation
static std::string optTypeAnnotationToString(
    const std::optional<TypeAnnotation>& ta) {
  return ta ? ta->toString() : "void";
}

/// Check if a type name is a primitive or built-in type (should not be
/// qualified)
static bool isPrimitiveOrBuiltinType(const std::string& name) {
  static const std::set<std::string> primitives = {
      "void", "bool", "i8",  "i16", "i32",    "i64",   "u8",    "u16",
      "u32",  "u64",  "f32", "f64", "string", "slice", "IError"};
  static const std::set<std::string> typeKeywords = {
      "raw_ptr", "static_ptr", "ref", "fn", "lambda", "array"};
  return primitives.count(name) > 0 || typeKeywords.count(name) > 0;
}

/// Qualify a type annotation string with namespace prefix
/// User-defined types get the prefix, primitives and type params don't
static std::string qualifyTypeAnnotation(
    const TypeAnnotation& ta, const std::string& nsPrefix,
    const std::set<std::string>& typeParams) {
  // Handle array types
  if (ta.isArray() && ta.elementType) {
    std::string result =
        "array<" + qualifyTypeAnnotation(*ta.elementType, nsPrefix, typeParams);
    for (size_t dim : ta.arrayDimensions) {
      result += ", " + std::to_string(dim);
    }
    result += ">";
    if (ta.canError) result += ", error";
    return result;
  }

  // Handle pointer types
  if (ta.isRawPointer() && ta.elementType) {
    return "raw_ptr(" +
           qualifyTypeAnnotation(*ta.elementType, nsPrefix, typeParams) + ")";
  }
  if (ta.isStaticPointer() && ta.elementType) {
    return "static_ptr(" +
           qualifyTypeAnnotation(*ta.elementType, nsPrefix, typeParams) + ")";
  }
  if (ta.isReference() && ta.elementType) {
    return "ref(" +
           qualifyTypeAnnotation(*ta.elementType, nsPrefix, typeParams) + ")";
  }

  // Handle function types
  if (ta.isFunction()) {
    std::string result = "_(";
    for (size_t i = 0; i < ta.paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += qualifyTypeAnnotation(*ta.paramTypes[i], nsPrefix, typeParams);
    }
    result += ") -> ";
    result += ta.returnType
                  ? qualifyTypeAnnotation(*ta.returnType, nsPrefix, typeParams)
                  : "void";
    return result;
  }

  // Handle lambda types
  if (ta.isLambda()) {
    std::string result = "(";
    for (size_t i = 0; i < ta.paramTypes.size(); ++i) {
      if (i > 0) result += ", ";
      result += qualifyTypeAnnotation(*ta.paramTypes[i], nsPrefix, typeParams);
    }
    result += ") -> ";
    result += ta.returnType
                  ? qualifyTypeAnnotation(*ta.returnType, nsPrefix, typeParams)
                  : "void";
    return result;
  }

  // Handle generic types: ClassName<T, U>
  if (!ta.typeArguments.empty()) {
    // Qualify the base name if it's not a primitive/keyword and not a type
    // param
    std::string baseName = ta.baseName;
    if (!nsPrefix.empty() && !isPrimitiveOrBuiltinType(baseName) &&
        typeParams.count(baseName) == 0) {
      baseName = nsPrefix + baseName;
    }
    std::string result = baseName + "<";
    for (size_t i = 0; i < ta.typeArguments.size(); ++i) {
      if (i > 0) result += ", ";
      result +=
          qualifyTypeAnnotation(*ta.typeArguments[i], nsPrefix, typeParams);
    }
    result += ">";
    if (ta.canError) result += ", error";
    return result;
  }

  // Simple type name: qualify if it's a user-defined type
  std::string result = ta.baseName;
  if (!nsPrefix.empty() && !isPrimitiveOrBuiltinType(result) &&
      typeParams.count(result) == 0) {
    result = nsPrefix + result;
  }
  if (ta.canError) result += ", error";
  return result;
}

/// Qualify a type annotation with namespace prefix (convenience overload)
static std::string qualifyTypeAnnotation(
    const TypeAnnotation& ta, const std::string& nsPrefix,
    const std::vector<std::string>& typeParamVec) {
  std::set<std::string> typeParams(typeParamVec.begin(), typeParamVec.end());
  return qualifyTypeAnnotation(ta, nsPrefix, typeParams);
}

/// Qualify optional type annotation
static std::string qualifyOptTypeAnnotation(
    const std::optional<TypeAnnotation>& ta, const std::string& nsPrefix,
    const std::vector<std::string>& typeParams) {
  return ta ? qualifyTypeAnnotation(*ta, nsPrefix, typeParams) : "void";
}

// Forward declaration for recursive extraction
static void extractFromStatements(
    const std::vector<std::unique_ptr<ExprAST>>& stmts,
    sun::ModuleMetadata& metadata, const std::string& nsPrefix,
    const std::filesystem::path& moduleDir);

/// Extract a function definition from AST with optional namespace prefix
static void extractFunction(const FunctionAST& func,
                            sun::ModuleMetadata& metadata,
                            const std::string& nsPrefix) {
  const auto& proto = func.getProto();

  sun::ExportedSymbol sym;
  sym.kind = sun::ExportedSymbol::Kind::Function;
  sym.name = nsPrefix.empty() ? proto.getName() : nsPrefix + proto.getName();
  sym.mangledName = sym.name;  // Qualified name for namespaced functions

  // Get type parameters for this function
  std::vector<std::string> typeParams = proto.getTypeParameters();

  // Build type signature: (param1, param2) -> return
  std::string sig = "(";
  const auto& args = proto.getArgs();
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) sig += ", ";
    sig += qualifyTypeAnnotation(args[i].second, nsPrefix, typeParams);
  }
  sig += ") -> " +
         qualifyOptTypeAnnotation(proto.getReturnType(), nsPrefix, typeParams);
  sym.typeSignature = sig;
  sym.isPublic = true;

  metadata.exports.push_back(sym);
}

/// Extract a class definition from AST with optional namespace prefix
static void extractClass(const ClassDefinitionAST& classDef,
                         sun::ModuleMetadata& metadata,
                         const std::string& nsPrefix,
                         const std::string& importPath) {
  sun::ClassInfo classInfo;
  classInfo.name =
      nsPrefix.empty() ? classDef.getName() : nsPrefix + classDef.getName();
  classInfo.sourceFile = importPath;
  classInfo.typeParams = classDef.getTypeParameters();

  // Get class type parameters for qualification context
  std::vector<std::string> classTypeParams = classDef.getTypeParameters();

  // Convert ImplementedInterfaceAST to strings for serialization
  // Interface names should also be qualified with the namespace prefix
  for (const auto& iface : classDef.getImplementedInterfaces()) {
    // Qualify the interface name with the namespace prefix
    std::string ifaceStr =
        nsPrefix.empty() ? iface.name : nsPrefix + iface.name;
    if (!iface.typeArguments.empty()) {
      ifaceStr += "<";
      for (size_t i = 0; i < iface.typeArguments.size(); ++i) {
        if (i > 0) ifaceStr += ", ";
        ifaceStr += qualifyTypeAnnotation(iface.typeArguments[i], nsPrefix,
                                          classTypeParams);
      }
      ifaceStr += ">";
    }
    classInfo.interfaces.push_back(ifaceStr);
  }

  // Extract fields
  for (const auto& field : classDef.getFields()) {
    sun::FieldInfo fieldInfo;
    fieldInfo.name = field.name;
    fieldInfo.typeSig =
        qualifyTypeAnnotation(field.type, nsPrefix, classTypeParams);
    classInfo.fields.push_back(fieldInfo);
  }

  // Extract methods
  for (const auto& method : classDef.getMethods()) {
    sun::MethodInfo methodInfo;
    const auto& proto = method.function->getProto();
    methodInfo.name = proto.getName();

    // Combine class type params with method type params for qualification
    std::vector<std::string> methodTypeParams = classTypeParams;
    for (const auto& tp : proto.getTypeParameters()) {
      methodTypeParams.push_back(tp);
      methodInfo.typeParams.push_back(tp);
    }

    methodInfo.returnTypeSig = qualifyOptTypeAnnotation(
        proto.getReturnType(), nsPrefix, methodTypeParams);
    methodInfo.isStatic = false;

    // Capture variadic parameter info
    if (proto.hasVariadicParam()) {
      methodInfo.variadicParamName = proto.getVariadicParamName().value_or("");
      if (proto.hasVariadicConstraint()) {
        methodInfo.variadicConstraint = qualifyTypeAnnotation(
            *proto.getVariadicConstraint(), nsPrefix, methodTypeParams);
      }
    }

    // For generic methods, capture the source text
    if (method.function->hasSourceText()) {
      methodInfo.bodySource = method.function->getSourceText();
    }

    for (const auto& param : proto.getArgs()) {
      methodInfo.paramNames.push_back(param.first);
      methodInfo.paramTypeSigs.push_back(
          qualifyTypeAnnotation(param.second, nsPrefix, methodTypeParams));
    }

    classInfo.methods.push_back(methodInfo);
  }

  metadata.classes.push_back(classInfo);

  // Also add class as an exported symbol
  sun::ExportedSymbol sym;
  sym.kind = sun::ExportedSymbol::Kind::Class;
  sym.name = classInfo.name;
  sym.mangledName = classInfo.name + "_struct";
  sym.isPublic = true;
  metadata.exports.push_back(sym);
}

/// Extract an interface definition from AST with optional namespace prefix
static void extractInterface(const InterfaceDefinitionAST& ifaceDef,
                             sun::ModuleMetadata& metadata,
                             const std::string& nsPrefix) {
  sun::InterfaceInfo ifaceInfo;
  ifaceInfo.name =
      nsPrefix.empty() ? ifaceDef.getName() : nsPrefix + ifaceDef.getName();

  // Get interface type parameters for qualification context
  std::vector<std::string> ifaceTypeParams = ifaceDef.getTypeParameters();

  for (const auto& method : ifaceDef.getMethods()) {
    sun::MethodInfo methodInfo;
    const auto& proto = method.function->getProto();
    methodInfo.name = proto.getName();

    // Combine interface type params with method type params for qualification
    std::vector<std::string> methodTypeParams = ifaceTypeParams;
    for (const auto& tp : proto.getTypeParameters()) {
      methodTypeParams.push_back(tp);
      methodInfo.typeParams.push_back(tp);
    }

    methodInfo.returnTypeSig = qualifyOptTypeAnnotation(
        proto.getReturnType(), nsPrefix, methodTypeParams);

    // Capture variadic parameter info
    if (proto.hasVariadicParam()) {
      methodInfo.variadicParamName = proto.getVariadicParamName().value_or("");
      if (proto.hasVariadicConstraint()) {
        methodInfo.variadicConstraint = qualifyTypeAnnotation(
            *proto.getVariadicConstraint(), nsPrefix, methodTypeParams);
      }
    }

    // For generic methods with default impl, capture the source text
    if (method.function->hasSourceText()) {
      methodInfo.bodySource = method.function->getSourceText();
    }

    for (const auto& param : proto.getArgs()) {
      methodInfo.paramNames.push_back(param.first);
      methodInfo.paramTypeSigs.push_back(
          qualifyTypeAnnotation(param.second, nsPrefix, methodTypeParams));
    }

    ifaceInfo.methods.push_back(methodInfo);
  }

  metadata.interfaces.push_back(ifaceInfo);

  // Also add interface as an exported symbol
  sun::ExportedSymbol sym;
  sym.kind = sun::ExportedSymbol::Kind::Interface;
  sym.name = ifaceInfo.name;
  sym.mangledName = ifaceInfo.name;
  sym.isPublic = true;
  metadata.exports.push_back(sym);
}

/// Recursively extract metadata from statements, handling namespaces
static void extractFromStatements(
    const std::vector<std::unique_ptr<ExprAST>>& stmts,
    sun::ModuleMetadata& metadata, const std::string& nsPrefix,
    const std::filesystem::path& moduleDir) {
  for (const auto& stmt : stmts) {
    if (!stmt) continue;

    // Extract imports as dependencies (only at top level)
    if (stmt->isImport() && nsPrefix.empty()) {
      const auto& importStmt = static_cast<const ImportAST&>(*stmt);
      std::filesystem::path depPath = importStmt.getPath();
      if (depPath.is_relative()) {
        depPath = std::filesystem::weakly_canonical(moduleDir / depPath);
      }
      metadata.dependencies.push_back(depPath.string());
    }

    // Handle namespace/module blocks
    if (stmt->getType() == ASTNodeType::NAMESPACE) {
      const auto& nsDecl = static_cast<const NamespaceAST&>(*stmt);
      // Build qualified prefix: parent_child_
      std::string newPrefix = nsPrefix.empty()
                                  ? nsDecl.getName() + "_"
                                  : nsPrefix + nsDecl.getName() + "_";
      // Recursively extract from namespace body
      extractFromStatements(nsDecl.getBody().getBody(), metadata, newPrefix,
                            moduleDir);
    }

    // Extract function definitions
    if (stmt->getType() == ASTNodeType::FUNCTION) {
      extractFunction(static_cast<const FunctionAST&>(*stmt), metadata,
                      nsPrefix);
    }

    // Extract class definitions
    if (stmt->getType() == ASTNodeType::CLASS_DEFINITION) {
      extractClass(static_cast<const ClassDefinitionAST&>(*stmt), metadata,
                   nsPrefix, metadata.importPath);
    }

    // Extract interface definitions
    if (stmt->getType() == ASTNodeType::INTERFACE_DEFINITION) {
      extractInterface(static_cast<const InterfaceDefinitionAST&>(*stmt),
                       metadata, nsPrefix);
    }
  }
}

/// Extract module metadata from parsed AST
static sun::ModuleMetadata extractMetadata(const std::string& importPath,
                                           const BlockExprAST& ast) {
  sun::ModuleMetadata metadata;
  metadata.importPath = importPath;
  metadata.version = "1.0.0";

  // Get the directory of the current module for resolving relative imports
  std::filesystem::path moduleDir =
      std::filesystem::path(importPath).parent_path();

  // Extract metadata from all statements, handling namespaces recursively
  extractFromStatements(ast.getBody(), metadata, "", moduleDir);

  return metadata;
}

/// Parse a file and extract metadata without full compilation
static std::optional<sun::ModuleMetadata> parseAndExtractMetadata(
    const std::string& filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  std::istringstream ss(source);
  Parser parser(ss);
  parser.setBaseDir(std::filesystem::path(filename).parent_path().string());
  parser.getNextToken();

  auto ast = parser.parseProgram();
  if (!ast) {
    return std::nullopt;
  }

  return extractMetadata(filename, *ast);
}

static void printUsage(const char* programName) {
  llvm::errs() << "Usage: " << programName
               << " [options] <script.sun> [-- args...]\n";
  llvm::errs() << "Options:\n";
  llvm::errs() << "  -c, --compile     Compile to executable (default: JIT "
                  "execute)\n";
  llvm::errs() << "  -o <file>         Output executable name (default: a.out "
                  "or based on input)\n";
  llvm::errs() << "  -S                Emit assembly file\n";
  llvm::errs() << "  --emit-obj        Emit object file only (do not link)\n";
  llvm::errs() << "  --emit-ir         Print LLVM IR to stdout\n";
  llvm::errs() << "  --emit-moon     Compile to .moon precompiled library\n";
  llvm::errs()
      << "  --bundle          Bundle multiple .sun files into one .moon\n";
  llvm::errs() << "  --lib-path <dir>  Add directory to library search path\n";
  llvm::errs() << "  -h, --help        Show this help message\n";
  llvm::errs() << "\nArguments after the script file (or after --) are passed "
                  "to main(argc, argv).\n";
  llvm::errs() << "\nExamples:\n";
  llvm::errs()
      << "  sun program.sun                              # JIT execute\n";
  llvm::errs()
      << "  sun --emit-moon -o lib.moon module.sun   # Create library\n";
  llvm::errs()
      << "  sun --bundle -o stdlib.moon stdlib/*.sun   # Bundle library\n";
  llvm::errs() << "  sun --lib-path build/ program.sun            # Use "
                  "precompiled libs\n";
}

/// Expand glob patterns in input files (for --bundle)
static std::vector<std::string> expandGlobs(
    const std::vector<std::string>& patterns) {
  std::vector<std::string> result;

  for (const auto& pattern : patterns) {
    glob_t globResult;
    int ret =
        glob(pattern.c_str(), GLOB_TILDE | GLOB_NOCHECK, nullptr, &globResult);

    if (ret == 0) {
      for (size_t i = 0; i < globResult.gl_pathc; ++i) {
        result.push_back(globResult.gl_pathv[i]);
      }
      globfree(&globResult);
    } else {
      // If glob fails, keep the original pattern
      result.push_back(pattern);
    }
  }

  return result;
}

int main(int argc, char* argv[]) {
  // Parse command-line arguments
  std::string outputFile;
  std::vector<std::string> inputFiles;
  std::vector<std::string> libPaths;
  bool compileMode = false;
  bool emitObjOnly = false;
  bool emitMoon = false;
  bool bundleMode = false;
  bool emitIR = false;
  int programArgStart = -1;  // Index where program arguments start

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--") {
      // Everything after -- is passed to the Sun program
      if (i + 1 < argc) {
        programArgStart = i + 1;
      }
      break;
    } else if (arg == "-c" || arg == "--compile") {
      compileMode = true;
    } else if (arg == "-o" && i + 1 < argc) {
      outputFile = argv[++i];
    } else if (arg == "--emit-obj") {
      compileMode = true;
      emitObjOnly = true;
    } else if (arg == "--emit-moon") {
      emitMoon = true;
    } else if (arg == "--emit-ir") {
      emitIR = true;
    } else if (arg == "--bundle") {
      bundleMode = true;
    } else if (arg == "--lib-path" && i + 1 < argc) {
      libPaths.push_back(argv[++i]);
    } else if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else if (arg[0] == '-') {
      llvm::errs() << "Unknown option: " << arg << "\n";
      printUsage(argv[0]);
      return 1;
    } else {
      // Input file or pattern
      inputFiles.push_back(arg);

      // For non-bundle/moon modes, stop after first input file
      if (!bundleMode && !emitMoon && inputFiles.size() == 1) {
        // Check if next args are program args (not options)
        if (i + 1 < argc && argv[i + 1][0] != '-') {
          programArgStart = i + 1;
          break;
        }
      }
    }
  }

  // Initialize library cache
  sun::LibraryCache::instance().initFromEnvironment();
  for (const auto& libPath : libPaths) {
    sun::LibraryCache::instance().addSearchPath(libPath);
  }

  // Handle --bundle mode
  if (bundleMode) {
    if (inputFiles.empty()) {
      llvm::errs() << "Error: --bundle requires input files\n";
      return 1;
    }
    if (outputFile.empty()) {
      llvm::errs() << "Error: --bundle requires -o <output.moon>\n";
      return 1;
    }

    // Expand glob patterns
    std::vector<std::string> expandedFiles = expandGlobs(inputFiles);
    if (expandedFiles.empty()) {
      llvm::errs() << "Error: No input files found\n";
      return 1;
    }

    llvm::outs() << "Bundling " << expandedFiles.size() << " files into "
                 << outputFile << "\n";

    sun::SunLibWriter bundleWriter;

    for (const auto& file : expandedFiles) {
      llvm::outs() << "  Processing: " << file << "\n";

      try {
        // First, parse to extract metadata
        auto metadataOpt = parseAndExtractMetadata(file);
        if (!metadataOpt) {
          llvm::errs() << "Error: Failed to parse " << file
                       << " for metadata\n";
          return 1;
        }
        auto metadata = *metadataOpt;

        // Then compile to get LLVM IR
        auto driver = Driver::createForAOT("bundle_module");
        driver->compileFile(file);

        bundleWriter.addModule(file, driver->getModule(), metadata);
      } catch (const SunError& e) {
        llvm::errs() << "Error compiling " << file << ": " << e.what() << "\n";
        return 1;
      }
    }

    if (!bundleWriter.write(outputFile)) {
      llvm::errs() << "Error writing bundle: " << bundleWriter.getError()
                   << "\n";
      return 1;
    }

    llvm::outs() << "Successfully created: " << outputFile << "\n";
    return 0;
  }

  // Handle --emit-moon mode
  if (emitMoon) {
    if (inputFiles.empty()) {
      llvm::errs() << "Error: --emit-moon requires an input file\n";
      return 1;
    }
    if (outputFile.empty()) {
      // Derive from input file
      outputFile = inputFiles[0];
      size_t dotPos = outputFile.rfind(".sun");
      if (dotPos != std::string::npos) {
        outputFile = outputFile.substr(0, dotPos);
      }
      outputFile += ".moon";
    }

    llvm::outs() << "Creating moon: " << inputFiles[0] << " -> " << outputFile
                 << "\n";

    try {
      // First, parse to extract metadata
      auto metadataOpt = parseAndExtractMetadata(inputFiles[0]);
      if (!metadataOpt) {
        llvm::errs() << "Error: Failed to parse " << inputFiles[0]
                     << " for metadata\n";
        return 1;
      }
      auto metadata = *metadataOpt;

      // Then compile to get LLVM IR
      auto driver = Driver::createForAOT("moon_module");
      driver->compileFile(inputFiles[0]);

      sun::SunLibWriter writer;
      writer.addModule(inputFiles[0], driver->getModule(), metadata);

      if (!writer.write(outputFile)) {
        llvm::errs() << "Error writing moon: " << writer.getError() << "\n";
        return 1;
      }

      llvm::outs() << "Successfully created: " << outputFile << "\n";
      return 0;
    } catch (const SunError& e) {
      llvm::errs() << "Error: " << e.what() << "\n";
      return 1;
    }
  }

  // Normal compilation/execution modes
  if (inputFiles.empty()) {
    llvm::errs() << "Error: No input file specified.\n";
    printUsage(argv[0]);
    return 1;
  }

  std::string inputFile = inputFiles[0];

  // Build program arguments: argv[0] = script name, followed by remaining args
  std::vector<char*> programArgv;
  programArgv.push_back(const_cast<char*>(inputFile.c_str()));
  int programArgc = 1;

  if (programArgStart > 0) {
    for (int i = programArgStart; i < argc; ++i) {
      programArgv.push_back(argv[i]);
      programArgc++;
    }
  }

  // Null-terminate for C compatibility
  programArgv.push_back(nullptr);

  // Determine output filename if not specified
  if (compileMode && outputFile.empty()) {
    // Derive output name from input (remove .sun extension if present)
    outputFile = inputFile;
    size_t dotPos = outputFile.rfind(".sun");
    if (dotPos != std::string::npos && dotPos == outputFile.length() - 4) {
      outputFile = outputFile.substr(0, dotPos);
    }
    if (emitObjOnly) {
      outputFile += ".o";
    }
  }

  if (compileMode) {
    // Compilation mode: generate executable without JIT
    llvm::outs() << "Compiling: " << inputFile << " -> " << outputFile << "\n";

    try {
      auto driver = Driver::createForAOT("main_module");
      driver->compileFile(inputFile);

      // Print IR if requested (only user-defined, not imports)
      if (emitIR) {
        driver->printUserDefinedIR();
      }

      // Emit executable
      std::string errorMsg;
      bool success;
      if (emitObjOnly) {
        success =
            sun::emitObjectFile(driver->getModule(), outputFile, errorMsg);
      } else {
        success =
            sun::compileToExecutable(driver->getModule(), outputFile, errorMsg);
      }

      if (!success) {
        llvm::errs() << "Compilation failed: " << errorMsg << "\n";
        return 1;
      }

      llvm::outs() << "Successfully compiled to: " << outputFile << "\n";
      return 0;
    } catch (const SunError& e) {
      std::cerr << e.what() << std::endl;
      return 1;
    }
  }

  // JIT execution mode (default)
  try {
    auto driver = Driver::createForJIT("main_module");
    driver->setDumpIR(emitIR);
    driver->executeFile(inputFile, programArgc, programArgv.data());
  } catch (const SunError& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  return 0;
}