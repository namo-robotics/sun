// ast_common.h — Common types shared by all AST nodes

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast_fwd.h"
#include "ast/type_annotation.h"
#include "ast/type_constraint.h"
#include "semantic_analysis/types.h"

enum class ASTNodeType {
  NUMBER,
  STRING_LITERAL,
  CHAR_LITERAL,  // 'a' (a char) or b'a' (a u8)
  NULL_LITERAL,
  BOOL_LITERAL,
  ARRAY_LITERAL,   // [1, 2, 3] or [[1, 2], [3, 4]]
  STRUCT_LITERAL,  // { color: "red", speed: 120 }
  ARRAY_INDEX,     // x[i] or x[i, j] for n-dimensional (legacy)
  INDEX,           // x[i] or x[i, j] or x[1:10, 3:5] for indexing/slicing
  SLICE,           // Slice expression: start:end or single index
  VARIABLE_REFERENCE,
  VARIABLE_CREATION,
  VARIABLE_ASSIGNMENT,
  REFERENCE_CREATION,  // ref x = y - creates a reference to y
  BINARY,
  UNARY,
  CALL,  // Unified call - callee is an expression (can be var ref, function
         // literal, etc.)
  PROTOTYPE,
  FUNCTION,
  LAMBDA,  // Anonymous function (lambda expression)
  IF,
  MATCH,  // match value { pattern => expr, ... }
  FOR_LOOP,
  FOR_IN_LOOP,  // for (var x: T in iterable) { ... }
  WHILE_LOOP,
  BLOCK,
  INDEXED_ASSIGNMENT,
  RETURN,
  IMPORT,        // import "file.sun";
  IMPORT_SCOPE,  // Expanded import scope (contains imported file's AST)
  MANIFEST,      // manifest { ... } - defines package metadata and entry point
  MODULE,        // module Name { ... }
  USING,         // using Module::name; or using Module::*;
  QUALIFIED_NAME,        // Namespace::name
  CLASS_DEFINITION,      // class Name { ... }
  INTERFACE_DEFINITION,  // interface Name { ... }
  ENUM_DEFINITION,       // enum Name { Variant1, Variant2, ... }
  MEMBER_ACCESS,         // object.field or object.method(...)
  THIS,                  // this keyword
  MEMBER_ASSIGNMENT,     // object.field = value
  TRY_CATCH,             // try { ... } catch (e: IError) { ... }
  THROW,                 // throw <expr>
  BREAK_STMT,            // break statement
  CONTINUE_STMT,         // continue statement
  GENERIC_CALL,          // Generic function call: create<Type>(args...)
  PACK_EXPANSION,        // args... - expand a variadic parameter pack
  DECLARE_TYPE,          // declare [Alias =] Type<Args>;
  SPAWN,                 // spawn(lambda) - create OS thread
  UNSAFE_BLOCK,          // unsafe { ... } - unsafe operations block
  MOON_SCOPE,            // Wrapper for moon import stubs with content hash
  COMPOUND_ASSIGNMENT,   // target op= value (+=, -=, ...)
  TERNARY,               // cond ? thenExpr : elseExpr
  INTERPOLATED_STRING,   // `text ${expr}` (parse tree only; lowered away)
  PAREN_EXPR             // (expr) grouping (parse tree only; lowered away)
};

// One generic type parameter, as written between the angle brackets: a name,
// and optionally a constraint the type argument must satisfy.
//
//   <T>            name "T", no constraint — any type
//   <T: _Numeric>  see TypeConstraint for the forms a constraint can take
//
// The constraint is checked when the generic is instantiated with a concrete
// type argument, by the same predicate `_is<T>` uses in a function body.
struct TypeParameter {
  std::string name;
  std::optional<TypeConstraint> constraint;

  TypeParameter() = default;
  explicit TypeParameter(std::string n,
                         std::optional<TypeConstraint> c = std::nullopt)
      : name(std::move(n)), constraint(std::move(c)) {}

  bool operator==(const TypeParameter& other) const {
    return name == other.name && constraint == other.constraint;
  }

  // `T`, or `T: _Numeric` — how the parameter reads in source.
  std::string toString() const {
    return constraint ? name + ": " + constraint->toString() : name;
  }
};

// The names alone, for the many places that only care what a parameter is
// called (substitution, mangling, scope registration).
inline std::vector<std::string> typeParameterNames(
    const std::vector<TypeParameter>& params) {
  std::vector<std::string> names;
  names.reserve(params.size());
  for (const auto& p : params) names.push_back(p.name);
  return names;
}

// The trailing value pack in a parameter list, as written at the end of the
// parentheses: a name, and the type annotation after its colon.
//
//   args...                  name "args", anything the call supplies
//   args...: _params_of<T>   the parameters T's `init` takes, or, when T is a
//                            lambda, the parameters that lambda takes
//
// The annotation sits where an ordinary parameter's type sits, but it stands
// for a whole parameter list rather than one type, and the call's arguments
// are checked against it. It is not a constraint in the `<T: _Numeric>` sense:
// it does not narrow which types are allowed, it says where the pack's
// parameters come from.
//
// A pack is not a type. It stands for however many arguments the call passes,
// so a declaration holding one is a template: it is monomorphized once per
// argument tuple, and the pack's elements become ordinary positional
// parameters named `args.0`, `args.1`, … in that specialization.
struct VariadicParam {
  std::string name;
  std::optional<TypeAnnotation> typeAnnotation;

  VariadicParam() = default;
  explicit VariadicParam(std::string n,
                         std::optional<TypeAnnotation> annot = std::nullopt)
      : name(std::move(n)), typeAnnotation(std::move(annot)) {}

  bool hasTypeAnnotation() const { return typeAnnotation.has_value(); }

  // The element the pack materializes as at index i, e.g. `args.0`. Codegen
  // names the specialization's parameters this way and semantic analysis
  // rewrites `args...` into references to exactly these names, so the two
  // sides must agree here and nowhere else.
  std::string elementName(size_t i) const {
    return name + "." + std::to_string(i);
  }
};

// How a lambda takes hold of a variable from the enclosing scope. These are
// the three mutually exclusive ways; whether the binding is writable inside
// the lambda is a separate question (Capture::isConst).
enum class CaptureKind {
  // Not named in the capture list: the closure gets a copy it may only read.
  // A compound value cannot be captured this way — the copy would alias.
  ByValue,
  // `[x]`: the closure owns the value. A compound moves in and the scope
  // that built the closure drops it; a scalar is copied. Either way it is
  // the closure's to change.
  Owned,
  // `[ref x]`, or `[const ref x]` when isConst: a borrow of the original.
  // The env slot holds the referent's address, and the borrow checker holds
  // a loan on it — mutable for `ref`, shared for `const ref`.
  Borrow,
};

struct Capture {
  std::string name;
  sun::TypePtr type;
  CaptureKind kind = CaptureKind::ByValue;
  // The binding cannot be written inside the lambda. Always true for
  // `[const ref x]`; also true when a by-value capture picked up a `const`
  // variable, which stays constant however it was captured.
  bool isConst = false;
};
