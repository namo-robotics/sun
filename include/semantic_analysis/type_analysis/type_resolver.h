/** Resolves written types and scoped substitutions during semantic analysis. */
#pragma once
#include "semantic_analysis/generic_specializer.h"
#include "semantic_analysis/semantic_context.h"

/** Resolves declarations and checks Sun programs. */
namespace sun::semantic_analysis::type_analysis {
/** Resolves annotations and may request nominal specializations. */
class TypeResolver {
 public:
  /** Borrow the semantic session and its specialization service. */
  TypeResolver(SemanticContext &ctx, GenericSpecializer &generics)
      : ctx_(ctx), generics_(generics) {}
  /** Resolve an interface requirement with the current parameter bindings. */
  std::shared_ptr<sun::types::InterfaceType> resolveConstraintInterface(
      const sun::ast::TypeConstraint &constraint);

  // ---- Written type annotations ------------------------------------------

  /**
   * Resolve a written type annotation to a type, instantiating any generic
   * it names. The bindings in scope are already applied, so the result must
   * not be handed to substituteTypeParameters as well — see there.
   */
  sun::types::TypePtr typeAnnotationToType(
      const sun::ast::TypeAnnotation &annot);

  /**
   * Resolve a list of written type arguments, reporting `context` in the
   * error when one of them is not a type.
   */
  std::vector<sun::types::TypePtr> resolveTypeArguments(
      const std::vector<std::unique_ptr<sun::ast::TypeAnnotation>>
          &typeAnnotations,
      const std::optional<sun::support::Position> &location,
      const std::string &context);

  /**
   * Replace the type parameters in a type with what they are bound to in
   * scope, recursing through references, arrays, and generic arguments.
   *
   * Apply it once, and only to a type that was resolved somewhere else — a
   * variable's recorded type, a method signature stored on a class. Applying
   * it to what typeAnnotationToType just returned applies the same bindings
   * twice, and a second pass is not a no-op: a type parameter reaching this
   * function is read in whatever bindings are in scope here, which for a type
   * carried out of another template is a different template's parameter that
   * happens to share its name. That is how `IIterator<T, Container>` used to
   * capture the `T` of `Vec<T>` and recurse forever (issue #144).
   */
  sun::types::TypePtr substituteTypeParameters(sun::types::TypePtr type);

  /**
   * The const view of a type: every `ref T` in it, including inside a payload
   * enum (Option<ref T> -> Option<const ref T>), becomes `const ref T`. It is
   * what a const method's result looks like through a constant receiver, and
   * what its body returns against.
   */
  sun::types::TypePtr createConstView(sun::types::TypePtr type);

 private:
  SemanticContext &ctx_;
  GenericSpecializer &generics_;
};
}  // namespace sun::semantic_analysis::type_analysis
