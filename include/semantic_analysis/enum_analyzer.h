// enum_analyzer.h — Semantic analysis of enum definitions and uses.

#pragma once

#include <memory>
#include <string>

#include "semantic_analysis/types.h"

class SemanticAnalyzer;
class SemanticContext;
class GenericSpecializer;
class TypeInferer;
class ExprAST;
class EnumDefinitionAST;
class CallExprAST;
class MemberAccessAST;
class MatchExprAST;
struct GenericEnumInfo;
struct Position;

/** Checks enum definitions, variant construction, and match patterns. */
class EnumAnalyzer {
 public:
  /** Share the context and analysis helpers for this compilation. */
  EnumAnalyzer(SemanticContext &ctx, SemanticAnalyzer &sema,
               GenericSpecializer &generics, TypeInferer &types)
      : ctx_(ctx), sema_(sema), generics_(generics), types_(types) {}

  /**
   * Enum definition analysis: validation, payload resolution, registration
   * (generic enums register as templates).
   */
  void analyzeEnumDefinition(EnumDefinitionAST &enumDef);

  /** Check that an enum payload type is supported and not recursive by value.
   */
  void validateEnumPayloadType(const sun::TypePtr &type,
                               const std::shared_ptr<sun::EnumType> &enumType,
                               const std::string &variantName,
                               const Position &location);

  /**
   * Call interception for EnumName.Variant(args...) on concrete and generic
   * enums; returns true when the call was an enum construction.
   */
  bool tryAnalyzeEnumConstruction(CallExprAST &callExpr,
                                  sun::TypePtr expectedType);

  /**
   * Member-access interception for generic enum unit variants (Option.None);
   * returns true when handled (type arguments taken from the expected type).
   */
  bool tryAnalyzeGenericEnumUnitVariant(MemberAccessAST &memberAccess,
                                        sun::TypePtr expectedType);

  /**
   * Match analysis on enum discriminants: variant patterns, payload bindings,
   * exhaustiveness.
   */
  void analyzeEnumMatch(MatchExprAST &matchExpr,
                        const std::shared_ptr<sun::EnumType> &enumType,
                        sun::TypePtr expectedType);

 private:
  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;
  GenericSpecializer &generics_;
  TypeInferer &types_;

  /**
   * The enum name an expression spells, bare ("E") or through its module
   * path ("a.b.E"); empty when it is not such a path or a local variable
   * shadows its head.
   */
  std::string enumPathOf(const ExprAST &object);

  /**
   * Check a concrete enum variant construction, EnumName.Variant(args...),
   * against the payload the variant declares.
   */
  void analyzeEnumVariantConstruction(
      CallExprAST &callExpr, MemberAccessAST &memberAccess,
      const std::shared_ptr<sun::EnumType> &enumType);

  /**
   * Option.Some(42): infer type arguments from payload args (falling back to
   * the expected type), instantiate, then check like a concrete construction.
   */
  void analyzeGenericEnumConstruction(CallExprAST &callExpr,
                                      MemberAccessAST &memberAccess,
                                      const std::string &genericName,
                                      const GenericEnumInfo &genericInfo,
                                      sun::TypePtr expectedType);
};
