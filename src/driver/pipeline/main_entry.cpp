/** Adapts fallible entrypoints to native or JIT return values. */
#include "driver/driver.h"

#include <sstream>

#include "moon_bundling/moon.h"
#include "parsing/lowering_pass.h"
#include "semantic_analysis/semantic_pipeline.h"
#include "support/error.h"
#include "types/class_type.h"
#include "types/enum_type.h"
#include "types/pointer_types.h"

/** Coordinates compilation and execution of program entrypoints. */
namespace sun::driver {

/** Reports whether the JIT can safely call a function returning this type. */
static bool supportsJITReturn(const sun::types::TypePtr& type) {
  if (type->isPrimitive()) {
    return true;
  }
  if (auto pointer = std::dynamic_pointer_cast<sun::types::StaticPointerType>(type)) {
    return pointer->getPointeeType()->isUInt8();
  }
  if (auto pointer = std::dynamic_pointer_cast<sun::types::RawPointerType>(type)) {
    return pointer->getPointeeType()->isUInt8() ||
           pointer->getPointeeType()->isInt8();
  }
  return false;
}

sun::ast::FunctionAST* Driver::prepareMainEntry(sun::ast::BlockExprAST& block,
                                             bool execute) {
  using namespace sun::ast;
  using sun::support::logAndThrowError;
  // Bundles retain the source function's ordinary ABI for their importers.
  if (!ownBundleHash_.empty()) {
    return nullptr;
  }
  FunctionAST* main = nullptr;
  for (const auto& statement : block.getBody()) {
    auto* function = dynamic_cast<FunctionAST*>(statement.get());
    if (function && function->getProto().getName() == "main") {
      main = function;
      break;
    }
  }
  if (!main) {
    return nullptr;
  }
  const auto& proto = main->getProto();
  const auto valueType = proto.getResolvedReturnType();
  if (auto enumeration = std::dynamic_pointer_cast<sun::types::EnumType>(valueType);
      enumeration && !enumeration->hasPayload() && !proto.canThrow()) {
    return nullptr;
  }
  if (!valueType || (execute ? !supportsJITReturn(valueType)
                            : !(valueType->isVoid() || valueType->isInt32()))) {
    logAndThrowError("Unsupported return type for 'main': '" +
                        (valueType ? valueType->toString() : "unknown") + "'" +
                        (execute ? "" : "; compiled programs require i32 or void"),
                    main->getLocation());
  }
  if (!proto.canThrow()) return nullptr;
  const auto& args = proto.getResolvedParamTypes();
  auto argv = args.size() == 2
                  ? std::dynamic_pointer_cast<sun::types::RawPointerType>(args[1])
                  : nullptr;
  auto argument = argv
                      ? std::dynamic_pointer_cast<sun::types::RawPointerType>(
                            argv->getPointeeType())
                      : nullptr;
  if (!args.empty() &&
      (args.size() != 2 || !args[0]->isInt32() || !argument ||
       !argument->getPointeeType()->isInt8())) {
    logAndThrowError("Fallible 'main' must take no arguments or "
                     "(argc: i32, argv: raw_ptr<raw_ptr<i8>>)", main->getLocation());
  }

  auto& scope = analyzer->context().currentScope();
  std::string prefix = "sun_main_exception";
  while (scope.hasSymbol(prefix + "_entry") ||
         scope.hasSymbol(prefix + "_exit")) {
    prefix += "_";
  }
  const bool unit = valueType->isVoid();
  auto returnName = unit ? (execute ? "void" : "i32") : valueType->toString();
  std::string fallback = "0";
  if (valueType->isBool()) {
    fallback = "false";
  } else if (valueType->isFloatingPoint()) {
    fallback = "0.0" + returnName;
  } else if (valueType->isChar()) {
    fallback = "'\\0'";
  } else if (valueType->isStaticPointer()) {
    returnName = "static_ptr<u8>";
    fallback = "\"\"";
  } else if (auto pointer =
                 std::dynamic_pointer_cast<sun::types::RawPointerType>(valueType)) {
    returnName = "raw_ptr<" + pointer->getPointeeType()->toString() + ">";
    fallback = "null";
  }
  std::ostringstream source;
  source << "/** Exits after the entrypoint has unwound its owned resources. */\n"
         << "extern \"C\" function " << prefix << "_exit(code: i32) void as \"exit\";\n"
         << "/** Reports an uncaught entrypoint exception without allocating. */\n"
         << "function " << prefix << "_entry(";
  if (!args.empty()) source << "argc: i32, argv: raw_ptr<raw_ptr<i8>>";
  source << ") " << returnName << " {\ntry {\n"
         << (unit ? "" : "return ") << "main("
         << (args.empty() ? "" : "argc, argv") << ");\n";
  if (unit) source << (execute ? "return;\n" : "return 0;\n");
  source << "} catch {\nunsafe { __file_write(2, \"Unhandled main exception\\n\"); "
         << prefix << "_exit(1); };\n"
         << (unit && execute ? "return;\n" : "return " + fallback + ";\n")
         << "}\n}\n";
  auto parser = sun::parsing::Parser::createStringParser(source.str());
  parser.setFilePath("<main-exception-adapter>");
  auto generated = parser.parseProgram();
  sun::parsing::LoweringPass().run(*generated);
  auto& pipeline = analyzer->pipeline();
  pipeline.prepareGenerated(*generated, std::vector<std::string>{});
  using sun::semantic_analysis::PortableDeclarationKey;
  const auto identity = PortableDeclarationKey::fromDeclaration(
      proto.getDeclarationId(), analysisResults->declarations);
  PortableDeclarationKey::assignOriginals(
      *generated, analysisResults->declarations,
      sun::moon_bundling::computeSha256Hex(identity.encoding() + source.str()));
  pipeline.declarations().run(*generated);
  for (auto& statement : generated->mutableBody()) {
    analyzer->analyzeExpr(*statement);
  }
  sun::semantic_analysis::passes::SpecializationBodyAnalysisPass(*analyzer).run();
  auto* adapter = static_cast<FunctionAST*>(generated->getBody().back().get());
  for (auto& statement : generated->mutableBody()) {
    block.addExpression(std::move(statement));
  }
  return adapter;
}

}  // namespace sun::driver
