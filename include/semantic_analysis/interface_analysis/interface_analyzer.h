/** Interface contract preparation, default-body analysis, and conformance. */
#pragma once

#include <memory>
#include <set>
#include <string>

#include "semantic_analysis/declaration_id.h"
#include "types/types.h"

/** Defines declarations and expressions checked by interface analysis. */
namespace sun::ast {
class ClassDefinitionAST;
class InterfaceDefinitionAST;
class MemberAccessAST;
}  // namespace sun::ast

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
class SemanticContext;
class SemanticAnalyzer;
class GenericSpecializer;

/** Resolves written types within the semantic session. */
namespace type_analysis {
class TypeResolver;
}

/** Owns interface preparation state and checks contracts and implementations.
 */
class InterfaceAnalyzer {
 public:
  /** Borrows the context and analysis helpers for this compilation. */
  InterfaceAnalyzer(SemanticContext &ctx, SemanticAnalyzer &sema,
                    GenericSpecializer &generics,
                    type_analysis::TypeResolver &resolver)
      : ctx_(ctx), sema_(sema), generics_(generics), resolver_(resolver) {}

  /**
   * Resolves declarations and checks types in this interface definition,
   * recording the results on its syntax nodes.
   */
  void analyzeInterfaceDefinition(
      sun::ast::InterfaceDefinitionAST &interfaceDef);
  /** Resolves an interface's complete inherited shape without checking bodies.
   */
  void ensureInterfaceShape(DeclarationId declaration);
  /**
   * Records a registered class's interface claims before constraint checking.
   * Field inheritance and method conformance remain part of class analysis.
   */
  void ensureClassInterfaces(DeclarationId declaration);
  /** Combines inherited members and validates matching child declarations. */
  void mergeInterfaceParent(sun::types::InterfaceType &interfaceType,
                            const sun::ast::InterfaceDefinitionAST &definition);

  /**
   * Copy the fields an implemented interface declares onto the class. Must run
   * before its methods are analyzed, since they may read those fields.
   */
  void inheritInterfaceFields(const sun::ast::ClassDefinitionAST &classDef,
                              std::shared_ptr<sun::types::ClassType> classType);

  /**
   * Check that a class implements every method its interfaces require, with
   * matching signatures and constness.
   */
  void validateInterfaceImplementation(
      const sun::ast::ClassDefinitionAST &classDef,
      std::shared_ptr<sun::types::ClassType> classType);

  /** Resolve the receiver member and enforce access rules. */
  sun::types::TypePtr resolveInterfaceMemberType(
      const sun::ast::MemberAccessAST &expr,
      const sun::types::TypePtr &objectType, const std::string &memberName);

 private:
  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;
  GenericSpecializer &generics_;
  type_analysis::TypeResolver &resolver_;

  // Preparation resolves interface shapes and class interface claims; class
  // fields, method conformance, and bodies are checked separately.
  std::set<DeclarationId> preparedTypes_;
  std::set<DeclarationId> preparingTypes_;
  std::set<DeclarationId> analyzedInterfaceBodies_;
  std::set<DeclarationId> resolvingInterfaceParents_;

  /** Resolves the local members and inherited contract of an interface. */
  void prepareInterfaceShape(sun::ast::InterfaceDefinitionAST &interfaceDef);
};

}  // namespace sun::semantic_analysis
