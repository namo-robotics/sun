// type_annotation.h — TypeAnnotation struct for parsed type info

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "semantic_analysis/declaration_id.h"
#include "support/position.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * One size of an array type: `5` in `array<T, 5>` or `N` in `array<T, N>`.
 * A size is written as a number or as the name of a constant, possibly through
 * its module (`limits.N`), never as an expression.
 */
struct ArrayDimension {
  // The number of elements. For a named size it is filled in when the type
  // is resolved, which is why resolving a const annotation may set it.
  mutable std::optional<size_t> size;
  // The constant as written, with dots; empty when a number was written.
  std::string constantName;
  // Where the size is written; not serialized
  sun::support::Position position{};

  /** True when the size was written as the name of a constant. */
  bool isNamed() const { return !constantName.empty(); }

  /** Two sizes are the same when they were written the same way. */
  bool operator==(const ArrayDimension& other) const {
    return constantName == other.constantName &&
           (isNamed() || size == other.size);
  }

  /** The size as written: the constant's name, or the number. */
  std::string toString() const {
    return isNamed() ? constantName : std::to_string(size.value_or(0));
  }
};

/**
 * Type annotation structure for parsed type info
 * Supports: i32, f64, bool, void, ptr&lt;T&gt;, ref T, function, lambda
 * Generic types: ClassName<T, U> for class instantiation
 * Array types: array<T, N> or array<T, M, N> for fixed-size arrays
 * Error union types: T, error (value or error)
 */
struct TypeAnnotation {
  std::optional<sun::semantic_analysis::DeclarationId> declarationKey;
  std::string baseName;  // "i32", "f64", "ptr", "fn", "lambda", "array", etc.
  std::unique_ptr<TypeAnnotation>
      elementType;  // For ptr/ref/array: element type

  // Callable types: function (param1) returnType or (param1) => returnType
  std::vector<std::unique_ptr<TypeAnnotation>>
      paramTypes;                              // Parameter types for fn type
  std::unique_ptr<TypeAnnotation> returnType;  // Return type for fn type

  // For generic types: List<i32>, Map<string, i32>
  std::vector<std::unique_ptr<TypeAnnotation>> typeArguments;

  // For array types: array<T, 5>, array<T, 3, 2> or array<T, N>
  std::vector<ArrayDimension> arrayDimensions;

  // For error union types: indicates this type can also be an error
  bool canError = false;
  bool requiresUnsafe = false;  // Calling this value needs an unsafe block.

  // For reference types: `const ref T` (the referent cannot be changed)
  bool constRef = false;

  // For lambda types: `<'_>() => T` admits lambdas that carry a captured
  // environment living in a stack frame; a plain `() => T` is reserved for
  // environment-free lambdas
  bool refEnv = false;

  // Lifetime on a `<'a>` lambda type or a `ref 'a T` reference. The name
  // "_" is a fresh anonymous lambda lifetime; empty means an elided `ref T`.
  std::string lifetimeName;

  // Lifetime arguments applied to a class type: Bus<'a> (written before any
  // type arguments); empty for an unannotated application
  std::vector<std::string> lifetimeArguments;

  // Source span (includes the "throws IError" suffix when present); not
  // serialized
  sun::support::Position span{};

  /** Creates a type annotation from a name or an existing annotation. */
  TypeAnnotation() = default;
  /** Creates a type annotation from a name or an existing annotation. */
  TypeAnnotation(std::string name) : baseName(std::move(name)) {}
  /** Creates a type annotation from a name or an existing annotation. */
  TypeAnnotation(const TypeAnnotation& other)
      : declarationKey(other.declarationKey),
        baseName(other.baseName),
        arrayDimensions(other.arrayDimensions),
        canError(other.canError),
        requiresUnsafe(other.requiresUnsafe),
        constRef(other.constRef),
        refEnv(other.refEnv),
        lifetimeName(other.lifetimeName),
        lifetimeArguments(other.lifetimeArguments),
        span(other.span) {
    if (other.elementType) {
      elementType = std::make_unique<TypeAnnotation>(*other.elementType);
    }
    for (const auto& param : other.paramTypes) {
      paramTypes.push_back(std::make_unique<TypeAnnotation>(*param));
    }
    if (other.returnType) {
      returnType = std::make_unique<TypeAnnotation>(*other.returnType);
    }
    for (const auto& typeArg : other.typeArguments) {
      typeArguments.push_back(std::make_unique<TypeAnnotation>(*typeArg));
    }
  }
  /** Replaces the stored state with a copy of another instance. */
  TypeAnnotation& operator=(const TypeAnnotation& other) {
    if (this != &other) {
      declarationKey = other.declarationKey;
      baseName = other.baseName;
      arrayDimensions = other.arrayDimensions;
      canError = other.canError;
      requiresUnsafe = other.requiresUnsafe;
      constRef = other.constRef;
      refEnv = other.refEnv;
      lifetimeName = other.lifetimeName;
      lifetimeArguments = other.lifetimeArguments;
      span = other.span;
      if (other.elementType) {
        elementType = std::make_unique<TypeAnnotation>(*other.elementType);
      } else {
        elementType = nullptr;
      }
      paramTypes.clear();
      for (const auto& param : other.paramTypes) {
        paramTypes.push_back(std::make_unique<TypeAnnotation>(*param));
      }
      if (other.returnType) {
        returnType = std::make_unique<TypeAnnotation>(*other.returnType);
      } else {
        returnType = nullptr;
      }
      typeArguments.clear();
      for (const auto& typeArg : other.typeArguments) {
        typeArguments.push_back(std::make_unique<TypeAnnotation>(*typeArg));
      }
    }
    return *this;
  }
  /** Creates a type annotation from a name or an existing annotation. */
  TypeAnnotation(TypeAnnotation&&) = default;
  /** Transfers the stored state from another instance during move assignment.
   */
  TypeAnnotation& operator=(TypeAnnotation&&) = default;

  /** Reports whether this syntax node represents a raw-pointer annotation. */
  bool isRawPointer() const {
    return baseName == "raw_ptr";
  }  // raw_ptr<T> non-owning pointer for C interop
  /** Reports whether this syntax node represents a static-pointer annotation.
   */
  bool isStaticPointer() const {
    return baseName == "static_ptr";
  }  // static_ptr<T> pointer to immortal static data
  /** Reports whether this syntax node represents a borrowed-reference
   * annotation. */
  bool isReference() const {
    return baseName == "ref";
  }  // ref(T) reference type
  /** Reports whether this syntax node represents a reference without write
   * access. */
  bool isConstReference() const { return isReference() && constRef; }
  /** Reports whether this syntax node represents a function definition. */
  bool isFunction() const {
    return baseName == "fn";
  }  // function () T thin function-pointer type
  /** Reports whether this syntax node represents a lambda expression. */
  bool isLambda() const {
    return baseName == "lambda";
  }  // () => {} anonymous function type
  /** Reports whether this syntax node represents an array annotation. */
  bool isArray() const {
    return baseName == "array";
  }  // array<T, N> fixed-size array
  /** Reports whether this value can be invoked as a function. */
  bool isCallable() const { return isFunction() || isLambda(); }
  /** Reports whether this declaration still has unbound type parameters. */
  bool isGeneric() const { return !typeArguments.empty(); }
  /** Reports whether this syntax node represents a value-or-error type. */
  bool isErrorUnion() const { return canError; }

  /** Compare type structure and resolved names, ignoring source locations. */
  bool operator==(const TypeAnnotation& other) const {
    auto equalOptional = [](const auto& left, const auto& right) {
      return (!left && !right) || (left && right && *left == *right);
    };
    auto equalList = [&](const auto& left, const auto& right) {
      if (left.size() != right.size()) return false;
      for (size_t i = 0; i < left.size(); ++i)
        if (!equalOptional(left[i], right[i])) return false;
      return true;
    };
    return (declarationKey || other.declarationKey
                ? declarationKey == other.declarationKey
                : baseName == other.baseName) &&
           equalOptional(elementType, other.elementType) &&
           equalOptional(returnType, other.returnType) &&
           equalList(paramTypes, other.paramTypes) &&
           equalList(typeArguments, other.typeArguments) &&
           arrayDimensions == other.arrayDimensions &&
           canError == other.canError &&
           requiresUnsafe == other.requiresUnsafe &&
           constRef == other.constRef && refEnv == other.refEnv &&
           lifetimeName == other.lifetimeName &&
           lifetimeArguments == other.lifetimeArguments;
  }

  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const {
    if (isArray() && elementType) {
      std::string result = "array<" + elementType->toString();
      for (const ArrayDimension& dim : arrayDimensions) {
        result += ", " + dim.toString();
      }
      result += ">";
      if (canError) result += " throws IError";
      return result;
    }
    if (isRawPointer() && elementType) {
      return "raw_ptr(" + elementType->toString() + ")";
    }
    if (isStaticPointer() && elementType) {
      return "static_ptr(" + elementType->toString() + ")";
    }
    if (isReference() && elementType) {
      std::string result = constRef ? "const ref" : "ref";
      if (!lifetimeName.empty()) result += " '" + lifetimeName;
      return result + "(" + elementType->toString() + ")";
    }
    if (isFunction()) {
      std::string result = "function (";
      for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (i > 0) result += ", ";
        result += paramTypes[i]->toString();
      }
      result += ") ";
      result += returnType ? returnType->toString() : "void";
      if (requiresUnsafe) result = "unsafe " + result;
      if (canError) result += " throws IError";
      return result;
    }
    if (isLambda()) {
      std::string result =
          refEnv ? "<'" + (lifetimeName.empty() ? "_" : lifetimeName) + ">("
                 : "(";
      for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (i > 0) result += ", ";
        result += paramTypes[i]->toString();
      }
      result += ") => ";
      result += returnType ? returnType->toString() : "void";
      if (requiresUnsafe) result = "unsafe " + result;
      if (canError) result += " throws IError";
      return result;
    }
    // Generic types: ClassName<'a, T> (lifetime arguments come first)
    if (!typeArguments.empty() || !lifetimeArguments.empty()) {
      std::string result = baseName + "<";
      bool first = true;
      for (const auto& lifetime : lifetimeArguments) {
        if (!first) result += ", ";
        result += "'" + lifetime;
        first = false;
      }
      for (const auto& typeArg : typeArguments) {
        if (!first) result += ", ";
        result += typeArg->toString();
        first = false;
      }
      result += ">";
      if (canError) result += " throws IError";
      return result;
    }
    std::string result = baseName;
    if (canError) result += " throws IError";
    return result;
  }
};

}  // namespace sun::ast
