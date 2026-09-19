// enum_analyzer.h — Semantic analysis of enum definitions and uses.

#pragma once

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
class CallExprAST;
}
/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
class EnumDefinitionAST;
}
/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
class ExprAST;
}
/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
class MatchExprAST;
}
/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
class MemberAccessAST;
}
/** Provides shared diagnostics, source tracking, and compiler utilities. */
namespace sun::support {
struct Position;
}

#include <memory>
#include <string>

#include "types/types.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

class SemanticAnalyzer;
class SemanticContext;
class GenericSpecializer;
/** Resolves written types within the semantic session. */
namespace type_analysis {
class TypeResolver;
}

}  // namespace sun::semantic_analysis

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {}
/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {}
/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {}
/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {}
/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {}

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

struct GenericEnumInfo;

}  // namespace sun::semantic_analysis

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {}

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Checks enum definitions, variant construction, and match patterns. */
class EnumAnalyzer {
 public:
  /** Share the context and analysis helpers for this compilation. */
  EnumAnalyzer(SemanticContext &ctx, SemanticAnalyzer &sema,
               GenericSpecializer &generics, type_analysis::TypeResolver &types)
      : ctx_(ctx), sema_(sema), generics_(generics), resolver_(types) {}

  /**
   * Enum definition analysis: validation, payload resolution, registration
   * (generic enums register as templates).
   */
  void analyzeEnumDefinition(sun::ast::EnumDefinitionAST &enumDef);

  /** Check that an enum payload type is supported and not recursive by value.
   */
  void validateEnumPayloadType(
      const sun::types::TypePtr &type,
      const std::shared_ptr<sun::types::EnumType> &enumType,
      const std::string &variantName, const sun::support::Position &location);

  /**
   * Call interception for EnumName.Variant(args...) on concrete and generic
   * enums; returns true when the call was an enum construction.
   */
  bool tryAnalyzeEnumConstruction(sun::ast::CallExprAST &callExpr,
                                  sun::types::TypePtr expectedType);

  /**
   * Member-access interception for generic enum unit variants (Option.None);
   * returns true when handled (type arguments taken from the expected type).
   */
  bool tryAnalyzeGenericEnumUnitVariant(sun::ast::MemberAccessAST &memberAccess,
                                        sun::types::TypePtr expectedType);

  /**
   * Match analysis on enum discriminants: variant patterns, payload bindings,
   * exhaustiveness.
   */
  void analyzeEnumMatch(sun::ast::MatchExprAST &matchExpr,
                        const std::shared_ptr<sun::types::EnumType> &enumType,
                        sun::types::TypePtr expectedType);

 private:
  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;
  GenericSpecializer &generics_;
  type_analysis::TypeResolver &resolver_;

  /**
   * The enum name an expression spells, bare ("E") or through its module
   * path ("a.b.E"); empty when it is not such a path or a local variable
   * shadows its head.
   */
  std::string enumPathOf(const sun::ast::ExprAST &object);

  /**
   * Check a concrete enum variant construction, EnumName.Variant(args...),
   * against the payload the variant declares.
   */
  void analyzeEnumVariantConstruction(
      sun::ast::CallExprAST &callExpr, sun::ast::MemberAccessAST &memberAccess,
      const std::shared_ptr<sun::types::EnumType> &enumType);

  /**
   * Option.Some(42): infer type arguments from payload args (falling back to
   * the expected type), instantiate, then check like a concrete construction.
   */
  void analyzeGenericEnumConstruction(sun::ast::CallExprAST &callExpr,
                                      sun::ast::MemberAccessAST &memberAccess,
                                      const std::string &genericName,
                                      const GenericEnumInfo &genericInfo,
                                      sun::types::TypePtr expectedType);
};

}  // namespace sun::semantic_analysis
