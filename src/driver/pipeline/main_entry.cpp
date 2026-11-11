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
  const auto resultType = proto.getResolvedReturnType();
  auto result = std::dynamic_pointer_cast<sun::types::EnumType>(resultType);
  // Plain enums retain their integer return representation.
  if (result && !result->hasPayload()) {
    return nullptr;
  }
  auto valueType = resultType;
  if (result) {
    const auto& variants = result->getVariants();
    if (variants.empty() || variants.front().payloadTypes.size() > 1) {
      logAndThrowError("'main' requires an enum with zero or one success payload",
                      main->getLocation());
    }
    valueType = variants.front().payloadTypes.empty()
                    ? sun::types::Types::Void()
                    : variants.front().payloadTypes.front();
    for (size_t i = 1; i < variants.size(); ++i) {
      if (variants[i].payloadTypes.size() != 1) {
        logAndThrowError("'main' requires each failure variant to have one payload",
                        main->getLocation());
      }
    }
  }
  if (!valueType || (execute ? !supportsJITReturn(valueType)
                            : !(valueType->isVoid() || valueType->isInt32()))) {
    logAndThrowError(
        std::string("Unsupported ") + (result ? "success payload" : "return type") +
            " for 'main': '" + (valueType ? valueType->toString() : "unknown") +
            "'" + (execute ? "" : "; compiled programs require i32 or void"),
        main->getLocation());
  }
  if (!result) {
    return nullptr;
  }
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
  std::string prefix = "sun_main_result";
  while (scope.hasSymbol(prefix + "_entry") ||
         scope.hasSymbol(prefix + "_type") ||
         scope.hasSymbol(prefix + "_exit")) {
    prefix += "_";
  }
  scope.declareEnum(prefix + "_type", result);
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
  const auto successValue = unit ? (execute ? "" : "0") : "value";
  const auto& variants = result->getVariants();
  std::ostringstream source;
  source << "/** Terminates after the entrypoint's result has been dropped. */\n"
         << "extern \"C\" function " << prefix << "_exit(code: i32) void as \"exit\";\n"
         << "/** Unwraps success or reports failure and exits unsuccessfully. */\n"
         << "function " << prefix << "_entry(";
  if (!args.empty()) {
    source << "argc: i32, argv: raw_ptr<raw_ptr<i8>>";
  }
  // Sun has no standalone block statement. A constant branch scopes the
  // owned result so its cleanup runs before the native exit call.
  source << ") " << returnName << " {\n"
         << "if (true) {\nvar result = main("
         << (args.empty() ? "" : "argc, argv") << ");\nmatch result {\n"
         << prefix << "_type." << variants.front().name
         << (unit ? "" : "(value)") << " => { return " << successValue
         << "; },\n";
  auto reportable = analyzer->context().lookupInterface("std.IError");
  for (size_t i = 1; i < variants.size(); ++i) {
    const auto& variant = variants[i];
    source << prefix << "_type." << variant.name << "(error) => {\n";
    auto error = std::dynamic_pointer_cast<sun::types::ClassType>(
        variant.payloadTypes.front());
    if (error && reportable && error->implementsInterface(*reportable)) {
      source << "match error.message() {\n"
             << "std.AllocationResult.Ok(message) => std.io.eprintln(message),\n"
             << "_ => std.io.eprintln(\"unable to allocate error message\")\n};\n";
    } else {
      source << "unsafe { __file_write(2, \"Unhandled main error: "
             << variant.name << "\\n\"); };\n";
    }
    source << "},\n";
  }
  // The external exit declaration returns void, so analysis still requires a
  // return statement even though the process cannot reach it.
  source << "};\n}\nunsafe { " << prefix << "_exit(1); };\n"
         << (unit && execute ? "return;\n" : "return " + fallback + ";\n")
         << "}\n";
  auto parser = sun::parsing::Parser::createStringParser(source.str());
  parser.setFilePath("<main-result-adapter>");
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
