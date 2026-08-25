// type_checks.h — "this expression must be a class / a lambda / an array"
//
// Codegen constantly needs the concrete type behind a `sun::TypePtr`: check
// the kind, then cast. Written by hand that is four lines and a bespoke error
// message every time. TypeCheck<T> does it in one:
//
//   auto& fn = sun::requireType<sun::LambdaType>(lambdaExpr, "spawn argument");
//   if (auto* cls = sun::tryGetType<sun::ClassType>(targetType)) { ... }
//
// `require*` throws a compile error naming the context; `tryGet*` hands back
// null so the caller can take another path. The `*Ptr` forms return a shared
// pointer for the places that keep the type alive past the call.
//
// T is any Type subclass that stands for a single kind — it supplies its own
// `StaticKind`, so there is nothing to register here.
//
// These see a type exactly as it is: a `ref T` is a reference, not a T. Pass
// `sun::unwrapRef(type)` to look through one.

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "ast/expr_ast.h"
#include "semantic_analysis/types.h"
#include "support/error.h"

namespace sun {

// How a kind is named in an error message ("must be a class type").
inline const char* describeKind(Type::Kind kind) {
  switch (kind) {
    case Type::Kind::Function:
      return "a function type";
    case Type::Kind::Lambda:
      return "a lambda type";
    case Type::Kind::RawPointer:
      return "a raw_ptr type";
    case Type::Kind::StaticPointer:
      return "a static_ptr type";
    case Type::Kind::NullPointer:
      return "the null literal";
    case Type::Kind::Reference:
      return "a reference type";
    case Type::Kind::Class:
      return "a class type";
    case Type::Kind::Interface:
      return "an interface type";
    case Type::Kind::Enum:
      return "an enum type";
    case Type::Kind::ErrorUnion:
      return "an error union type";
    case Type::Kind::Array:
      return "an array type";
    case Type::Kind::Slice:
      return "a slice type";
    case Type::Kind::Module:
      return "a module reference";
    case Type::Kind::Thread:
      return "a Thread handle type";
    case Type::Kind::TypeParameter:
      return "a type parameter";
    default:
      return "a primitive type";
  }
}

// Everything codegen does with "is this type a T": the check, the cast, and
// the error when it has to be one.
template <typename T>
struct TypeCheck {
  static bool matches(const TypePtr& type) {
    return type && type->getKind() == T::StaticKind;
  }

  // The type as a T, or null if it is absent or of another kind.
  static T* tryGet(const TypePtr& type) {
    return matches(type) ? static_cast<T*>(type.get()) : nullptr;
  }

  // Same, keeping the type alive alongside the caller.
  static std::shared_ptr<T> tryGetPtr(const TypePtr& type) {
    return matches(type) ? std::static_pointer_cast<T>(type) : nullptr;
  }

  // The type as a T. Anything else is a compile error naming `what`, which
  // reads as a noun phrase: "spawn argument", "struct literal".
  static T& require(const TypePtr& type, std::string_view what,
                    std::optional<Position> loc) {
    if (T* concrete = tryGet(type)) return *concrete;
    throwMismatch(type, what, loc);
  }

  static std::shared_ptr<T> requirePtr(const TypePtr& type,
                                       std::string_view what,
                                       std::optional<Position> loc) {
    if (auto concrete = tryGetPtr(type)) return concrete;
    throwMismatch(type, what, loc);
  }

  [[noreturn]] static void throwMismatch(const TypePtr& actual,
                                         std::string_view what,
                                         std::optional<Position> loc) {
    const char* expected = describeKind(T::StaticKind);
    if (!actual) {
      logAndThrowError(std::string(what) +
                           " has no type from semantic analysis; expected " +
                           expected,
                       loc);
    }
    logAndThrowError(std::string(what) + " must be " + expected +
                         ", but has type " + actual->toDisplayString(),
                     loc);
  }
};

// Call-site spellings. The expression forms read the resolved type and report
// at the expression's own source location.

template <typename T>
T* tryGetType(const TypePtr& type) {
  return TypeCheck<T>::tryGet(type);
}

template <typename T>
T* tryGetType(const ExprAST& expr) {
  return TypeCheck<T>::tryGet(expr.getResolvedType());
}

template <typename T>
std::shared_ptr<T> tryGetTypePtr(const TypePtr& type) {
  return TypeCheck<T>::tryGetPtr(type);
}

template <typename T>
std::shared_ptr<T> tryGetTypePtr(const ExprAST& expr) {
  return TypeCheck<T>::tryGetPtr(expr.getResolvedType());
}

template <typename T>
T& requireType(const TypePtr& type, std::string_view what,
               std::optional<Position> loc = std::nullopt) {
  return TypeCheck<T>::require(type, what, loc);
}

template <typename T>
T& requireType(const ExprAST& expr, std::string_view what) {
  return TypeCheck<T>::require(expr.getResolvedType(), what,
                               expr.getLocation());
}

template <typename T>
std::shared_ptr<T> requireTypePtr(const TypePtr& type, std::string_view what,
                                  std::optional<Position> loc = std::nullopt) {
  return TypeCheck<T>::requirePtr(type, what, loc);
}

template <typename T>
std::shared_ptr<T> requireTypePtr(const ExprAST& expr, std::string_view what) {
  return TypeCheck<T>::requirePtr(expr.getResolvedType(), what,
                                  expr.getLocation());
}

// What a raw_ptr<T> or static_ptr<T> points at; null for every other type.
// Both spellings are just an address at a call site, so the code that looks
// through one rarely cares which it had.
inline TypePtr getPointeeType(const TypePtr& type) {
  if (auto* raw = tryGetType<RawPointerType>(type))
    return raw->getPointeeType();
  if (auto* stat = tryGetType<StaticPointerType>(type)) {
    return stat->getPointeeType();
  }
  return nullptr;
}

}  // namespace sun
