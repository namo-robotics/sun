// type_inferer.h — What type does this expression have, and what type does
// this written annotation name?
//
// Two jobs that are really one. Inference reads an expression and works out
// its type without changing it; resolution reads a `TypeAnnotation` — the
// syntax the parser produced for `ref Vec<i32>` — and produces the type it
// names, instantiating any generic along the way. Both are asked for
// constantly by the analyzer, and neither records anything: the analyzer is
// what stamps a resolved type onto a node.
//
// Reading a type can build one, so this holds the specializer; and a few
// paths (a lambda body, an unsafe block) need the analyzer, so it holds that
// too.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "semantic_analysis/generic_specializer.h"
#include "semantic_analysis/semantic_context.h"

class SemanticAnalyzer;

/**
 * Answers "what type is this?" for expressions and for written type
 * annotations. Never modifies the AST.
 */
class TypeInferer {
 public:
  TypeInferer(SemanticContext &ctx, SemanticAnalyzer &sema,
              GenericSpecializer &generics)
      : ctx_(ctx), sema_(sema), generics_(generics) {}

  // ---- Expressions -------------------------------------------------------

  /**
   * Infer the type of an expression without modifying it.
   * Recursively traverses the AST to compute types based on:
   * - Literals: number format determines i32 vs f64, strings are String
   * - Variables: looked up in symbol table (local scope, then global)
   * - Binary ops: comparison returns bool, arithmetic returns LHS type
   * - Calls: return type from function/lambda signature
   * - References: wraps target type in ref(T)
   * Returns f64 as fallback for unknown expressions.
   */
  sun::TypePtr inferType(const ExprAST &expr);

  /**
   * The type of `object.member`, where the object may be a value, a class or
   * enum name, or a module. Also stamps the resolved symbol name onto the
   * node for codegen. Resolves the object's type — through a borrow, an
   * `_is<T>` narrowing and a pointer — then dispatches on what it turned out
   * to be.
   */
  sun::TypePtr inferType(const MemberAccessAST &expr);

  /**
   * The type a `f<T>(...)` call produces, dispatching on whether the name is
   * an intrinsic, a generic function, or a generic class.
   */
  sun::TypePtr inferGenericCallType(const GenericCallAST &call);

  /** The result type of an intrinsic call (_sizeof, _load, _to_ref, ...). */
  sun::TypePtr inferIntrinsicCallType(const GenericCallAST &call);

  /**
   * The return type of a call to a generic function, instantiating the
   * specialization if it does not exist yet.
   */
  sun::TypePtr inferGenericFunctionCallType(const GenericCallAST &call);

  /**
   * The type `C<T>(...)` constructs, instantiating the generic class if that
   * specialization does not exist yet.
   */
  sun::TypePtr inferGenericClassConstructionType(const GenericCallAST &call);

  /**
   * The static_ptr<T> type when `type` is a static_ptr to a non-class, else
   * null. A static_ptr<Class> dispatches to the class's own methods instead
   * of the builtin ones.
   */
  static sun::StaticPointerType *asNonClassStaticPtr(const sun::TypePtr &type);

  /** True for a builtin static_ptr<T> method name: length() or raw(). */
  static bool isStaticPtrMethod(const std::string &name);

  /**
   * The result type of a static_ptr<T> builtin method call, checking the
   * argument count.
   */
  sun::TypePtr inferStaticPtrMethodType(const sun::StaticPointerType &ptrType,
                                        const std::string &name,
                                        size_t argCount, const Position &loc);

  // ---- Written type annotations ------------------------------------------

  /**
   * Resolve a written type annotation to a type, instantiating any generic
   * it names. The bindings in scope are already applied, so the result must
   * not be handed to substituteTypeParameters as well — see there.
   */
  sun::TypePtr typeAnnotationToType(const TypeAnnotation &annot);

  /**
   * Resolve a list of written type arguments, reporting `context` in the
   * error when one of them is not a type.
   */
  std::vector<sun::TypePtr> resolveTypeArguments(
      const std::vector<std::unique_ptr<TypeAnnotation>> &typeAnnotations,
      const std::optional<Position> &location, const std::string &context);

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
  sun::TypePtr substituteTypeParameters(sun::TypePtr type);

  /**
   * The const view of a type: every `ref T` in it, including inside a payload
   * enum (Option<ref T> -> Option<const ref T>), becomes `const ref T`. It is
   * what a const method's result looks like through a constant receiver, and
   * what its body returns against.
   */
  sun::TypePtr createConstView(sun::TypePtr type);

  /**
   * The four member-access receivers with rules of their own. Each takes the
   * already-resolved object type, so `mod.x`, `obj.f`, `iface.m` and `T.m`
   * are read one at a time rather than as one switch.
   */
  sun::TypePtr inferModuleMemberType(const MemberAccessAST &memberAccess,
                                     const sun::TypePtr &objectType,
                                     const std::string &memberName);
  sun::TypePtr inferClassMemberType(const MemberAccessAST &memberAccess,
                                    const sun::TypePtr &objectType,
                                    const std::string &memberName);
  sun::TypePtr inferInterfaceMemberType(const MemberAccessAST &memberAccess,
                                        const sun::TypePtr &objectType,
                                        const std::string &memberName);
  sun::TypePtr inferTypeParameterMemberType(const MemberAccessAST &memberAccess,
                                            const sun::TypePtr &objectType,
                                            const std::string &memberName);

  /**
   * The four expression kinds whose inference is more than a line: a call's
   * return type, a variable's declared or narrowed type, an index's element
   * type, and an array literal's element type and dimensions.
   */
  sun::TypePtr inferCallType(const CallExprAST &callExpr);
  sun::TypePtr inferVariableReferenceType(const VariableReferenceAST &varRef);
  sun::TypePtr inferIndexType(const IndexAST &arrIdx);
  sun::TypePtr inferArrayLiteralType(const ArrayLiteralAST &arrLit);

 private:
  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;
  GenericSpecializer &generics_;
};
