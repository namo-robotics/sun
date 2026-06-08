// type_annotation.h — TypeAnnotation struct for parsed type info

#pragma once

#include <memory>
#include <string>
#include <vector>

// Type annotation structure for parsed type info
// Supports: i32, f64, bool, void, ptr<T>, ref T, fn, lambda
// Generic types: ClassName<T, U> for class instantiation
// Array types: array<T, N> or array<T, M, N> for fixed-size arrays
// Error union types: T, error (value or error)
struct TypeAnnotation {
  std::string baseName;  // "i32", "f64", "ptr", "fn", "lambda", "array", etc.
  std::unique_ptr<TypeAnnotation>
      elementType;  // For ptr/ref/array: element type

  // For function/lambda types: (param1, param2) -> returnType
  std::vector<std::unique_ptr<TypeAnnotation>>
      paramTypes;                              // Parameter types for fn type
  std::unique_ptr<TypeAnnotation> returnType;  // Return type for fn type

  // For generic types: List<i32>, Map<string, i32>
  std::vector<std::unique_ptr<TypeAnnotation>> typeArguments;

  // For array types: array<T, 5> or array<T, 3, 2>
  std::vector<size_t> arrayDimensions;

  // For error union types: indicates this type can also be an error
  bool canError = false;

  TypeAnnotation() = default;
  TypeAnnotation(std::string name) : baseName(std::move(name)) {}
  TypeAnnotation(const TypeAnnotation& other)
      : baseName(other.baseName),
        arrayDimensions(other.arrayDimensions),
        canError(other.canError) {
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
  bool isFunction() const {
    return baseName == "fn";
  }  // _() -> {} named function type
  bool isLambda() const {
    return baseName == "lambda";
  }  // () -> {} anonymous function type
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
      if (canError) result += ", error";
      return result;
    }
    if (isRawPointer() && elementType) {
      return "raw_ptr(" + elementType->toString() + ")";
    }
    if (isStaticPointer() && elementType) {
      return "static_ptr(" + elementType->toString() + ")";
    }
    if (isReference() && elementType) {
      return "ref(" + elementType->toString() + ")";
    }
    if (isFunction()) {
      std::string result = "_(";
      for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (i > 0) result += ", ";
        result += paramTypes[i]->toString();
      }
      result += ") -> ";
      result += returnType ? returnType->toString() : "void";
      return result;
    }
    if (isLambda()) {
      std::string result = "(";
      for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (i > 0) result += ", ";
        result += paramTypes[i]->toString();
      }
      result += ") -> ";
      result += returnType ? returnType->toString() : "void";
      return result;
    }
    // Generic types: ClassName<T, U>
    if (!typeArguments.empty()) {
      std::string result = baseName + "<";
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toString();
      }
      result += ">";
      if (canError) result += ", error";
      return result;
    }
    std::string result = baseName;
    if (canError) result += ", error";
    return result;
  }
};
