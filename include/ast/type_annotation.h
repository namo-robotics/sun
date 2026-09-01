// type_annotation.h — TypeAnnotation struct for parsed type info

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "support/position.h"

// Type annotation structure for parsed type info
// Supports: i32, f64, bool, void, ptr<T>, ref T, function, lambda
// Generic types: ClassName<T, U> for class instantiation
// Array types: array<T, N> or array<T, M, N> for fixed-size arrays
// Error union types: T, error (value or error)
struct TypeAnnotation {
  std::string baseName;  // "i32", "f64", "ptr", "fn", "lambda", "array", etc.
  std::unique_ptr<TypeAnnotation>
      elementType;  // For ptr/ref/array: element type

  // Callable types: function (param1) returnType or (param1) => returnType
  std::vector<std::unique_ptr<TypeAnnotation>>
      paramTypes;                              // Parameter types for fn type
  std::unique_ptr<TypeAnnotation> returnType;  // Return type for fn type

  // For generic types: List<i32>, Map<string, i32>
  std::vector<std::unique_ptr<TypeAnnotation>> typeArguments;

  // For array types: array<T, 5> or array<T, 3, 2>
  std::vector<size_t> arrayDimensions;

  // For error union types: indicates this type can also be an error
  bool canError = false;

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
  Position span{};

  TypeAnnotation() = default;
  TypeAnnotation(std::string name) : baseName(std::move(name)) {}
  TypeAnnotation(const TypeAnnotation& other)
      : baseName(other.baseName),
        arrayDimensions(other.arrayDimensions),
        canError(other.canError),
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
  TypeAnnotation& operator=(const TypeAnnotation& other) {
    if (this != &other) {
      baseName = other.baseName;
      arrayDimensions = other.arrayDimensions;
      canError = other.canError;
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
  TypeAnnotation(TypeAnnotation&&) = default;
  TypeAnnotation& operator=(TypeAnnotation&&) = default;

  bool isRawPointer() const {
    return baseName == "raw_ptr";
  }  // raw_ptr<T> non-owning pointer for C interop
  bool isStaticPointer() const {
    return baseName == "static_ptr";
  }  // static_ptr<T> pointer to immortal static data
  bool isReference() const {
    return baseName == "ref";
  }  // ref(T) reference type
  bool isConstReference() const { return isReference() && constRef; }
  bool isFunction() const {
    return baseName == "fn";
  }  // function () T thin function-pointer type
  bool isLambda() const {
    return baseName == "lambda";
  }  // () => {} anonymous function type
  bool isArray() const {
    return baseName == "array";
  }  // array<T, N> fixed-size array
  bool isCallable() const { return isFunction() || isLambda(); }
  bool isGeneric() const { return !typeArguments.empty(); }
  bool isErrorUnion() const { return canError; }

  std::string toString() const {
    if (isArray() && elementType) {
      std::string result = "array<" + elementType->toString();
      for (size_t dim : arrayDimensions) {
        result += ", " + std::to_string(dim);
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
