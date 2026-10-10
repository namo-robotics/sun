#include "codegen/intrinsics/declarations.h"

#include <sstream>

#include "generated/intrinsic_source.h"
#include "parsing/doc_comments.h"
#include "parsing/parser.h"
#include "support/error.h"
#include "types/type_factory.h"

/** Provides compiler-owned intrinsic declarations without emitting functions.
 */
namespace sun::codegen::intrinsics {
/** Keeps the parsed prelude private to this translation unit. */
namespace {

/** Owns parsed declarations for the compiler lifetime; never analyzed or
 * emitted. */
struct Declarations {
  std::unique_ptr<sun::ast::BlockExprAST> program;
  std::vector<const sun::ast::PrototypeAST*> prototypes;

  /** Parses the embedded prelude and attaches its source documentation. */
  Declarations() {
    std::istringstream input(intrinsicSource());
    sun::parsing::Parser parser(input);
    parser.setFilePath("<compiler>/intrinsics.sun");
    parser.getNextToken();
    program = parser.parseProgram();
    sun::parsing::attachDocComments(*program, intrinsicSource());
    for (const auto& node : program->getBody()) {
      if (node->getType() != sun::ast::ASTNodeType::FUNCTION) {
        sun::support::logAndThrowError(
            "Expected a compiler intrinsic declaration");
      }
      const auto& function = static_cast<const sun::ast::FunctionAST&>(*node);
      const auto& proto = function.getProto();
      if (function.hasBody() ||
          (proto.getTypeParameters().empty() && !proto.hasReturnType())) {
        sun::support::logAndThrowError(
            "Invalid compiler intrinsic declaration: " + proto.getName());
      }
      prototypes.push_back(&proto);
    }
  }
};

}  // namespace

const std::string& intrinsicSource() {
  static const std::string source(kIntrinsicSource);
  return source;
}

std::span<const sun::ast::PrototypeAST* const> intrinsicDeclarations() {
  static const Declarations declarations;
  return declarations.prototypes;
}

sun::types::TypePtr intrinsicType(const sun::ast::TypeAnnotation& annotation) {
  using sun::types::Types;
  if (annotation.baseName == "raw_ptr" && annotation.elementType) {
    return Types::RawPointer(intrinsicType(*annotation.elementType));
  }
  if (annotation.baseName == "static_ptr" && annotation.elementType) {
    return Types::StaticPointer(intrinsicType(*annotation.elementType));
  }
  if (auto type = Types::fromString(annotation.baseName)) return type;
  sun::support::logAndThrowError("Unsupported fixed intrinsic type: " +
                                 annotation.baseName);
}

}  // namespace sun::codegen::intrinsics
