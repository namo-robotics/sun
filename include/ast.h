// ast.h — pure AST, no LLVM dependencies

#pragma once

#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lexer.h"
#include "qualified_name.h"
#include "types.h"

// Forward declaration for type annotation
struct TypeAnnotation;

enum class ASTNodeType {
  NUMBER,
  STRING_LITERAL,
  NULL_LITERAL,
  BOOL_LITERAL,
  ARRAY_LITERAL,  // [1, 2, 3] or [[1, 2], [3, 4]]
  ARRAY_INDEX,    // x[i] or x[i, j] for n-dimensional (legacy)
  INDEX,          // x[i] or x[i, j] or x[1:10, 3:5] for indexing/slicing
  SLICE,          // Slice expression: start:end or single index
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
  IMPORT,                // import "file.sun";
  NAMESPACE,             // namespace Name { ... }
  USING,                 // using Namespace::name; or using Namespace::*;
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
  DECLARE_TYPE           // declare [Alias =] Type<Args>;
};

struct Capture {
  std::string name;
  sun::TypePtr type;
};

class ExprAST {
 protected:
  mutable sun::TypePtr resolvedType;  // Populated by semantic analyzer
  Position location_;                 // Original source location
  bool precompiled_ = false;          // True if from precompiled library
  std::string symbolPrefix_;          // Hash prefix for moon symbol isolation

 public:
  virtual ~ExprAST() = default;
  virtual ASTNodeType getType() const = 0;

  // Debug representation of this AST node
  virtual std::string toString() const = 0;

  // Print to stderr (easier to call from debugger than toString())
  void dump() const { std::cerr << toString() << "\n"; }

  // Clone this AST node (deep copy)
  virtual std::unique_ptr<ExprAST> clone() const = 0;

  // Type annotation set by semantic analyzer
  void setResolvedType(sun::TypePtr type) const { resolvedType = type; }
  sun::TypePtr getResolvedType() const { return resolvedType; }
  bool hasResolvedType() const { return resolvedType != nullptr; }
  void clearResolvedType() const { resolvedType = nullptr; }

  // Source location tracking
  void setLocation(Position loc) { location_ = loc; }
  void setLocation(int line, int column) {
    location_.line = line;
    location_.column = column;
  }
  const Position& getLocation() const { return location_; }
  int getLine() const { return location_.line; }
  int getColumn() const { return location_.column; }

  // Convenience type checks
  bool isFunction() const { return getType() == ASTNodeType::FUNCTION; }
  bool isLambda() const { return getType() == ASTNodeType::LAMBDA; }
  bool isBlock() const { return getType() == ASTNodeType::BLOCK; }
  bool isReturn() const { return getType() == ASTNodeType::RETURN; }
  bool isImport() const { return getType() == ASTNodeType::IMPORT; }

  // Precompiled flag (for definitions loaded from .moon files)
  bool isPrecompiled() const { return precompiled_; }
  void setPrecompiled(bool value) { precompiled_ = value; }

  // Symbol prefix for moon library isolation (content hash)
  const std::string& getSymbolPrefix() const { return symbolPrefix_; }
  void setSymbolPrefix(const std::string& prefix) { symbolPrefix_ = prefix; }

 protected:
  ExprAST() = default;
  explicit ExprAST(Position loc) : location_(loc) {}
};

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

class NumberExprAST : public ExprAST {
  std::variant<int64_t, double> value_;

 public:
  explicit NumberExprAST(int64_t intVal) : value_(intVal) {}
  explicit NumberExprAST(double floatVal) : value_(floatVal) {}
  ASTNodeType getType() const override { return ASTNodeType::NUMBER; }
  std::string toString() const override {
    if (isInteger()) return std::to_string(getIntVal());
    return std::to_string(getFloatVal());
  }
  std::unique_ptr<ExprAST> clone() const override;

  bool isInteger() const { return std::holds_alternative<int64_t>(value_); }
  bool isFloat() const { return std::holds_alternative<double>(value_); }

  int64_t getIntVal() const { return std::get<int64_t>(value_); }
  double getFloatVal() const { return std::get<double>(value_); }

  // For backward compatibility, get value as double
  double getVal() const {
    if (isInteger()) return static_cast<double>(std::get<int64_t>(value_));
    return std::get<double>(value_);
  }
};

class StringLiteralAST : public ExprAST {
  std::string Value;

 public:
  explicit StringLiteralAST(std::string Value) : Value(std::move(Value)) {}
  ASTNodeType getType() const override { return ASTNodeType::STRING_LITERAL; }
  std::string toString() const override { return "\"" + Value + "\""; }
  std::unique_ptr<ExprAST> clone() const override;
  const std::string& getValue() const { return Value; }
};

class NullLiteralAST : public ExprAST {
 public:
  NullLiteralAST() = default;
  ASTNodeType getType() const override { return ASTNodeType::NULL_LITERAL; }
  std::string toString() const override { return "null"; }
  std::unique_ptr<ExprAST> clone() const override;
};

class BoolLiteralAST : public ExprAST {
  bool Value;

 public:
  explicit BoolLiteralAST(bool Value) : Value(Value) {}
  ASTNodeType getType() const override { return ASTNodeType::BOOL_LITERAL; }
  std::string toString() const override { return Value ? "true" : "false"; }
  std::unique_ptr<ExprAST> clone() const override;
  bool getValue() const { return Value; }
};

// Array literal: [1, 2, 3] or [[1, 2], [3, 4]] for nested arrays
class ArrayLiteralAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> elements;

 public:
  explicit ArrayLiteralAST(std::vector<std::unique_ptr<ExprAST>> elems)
      : elements(std::move(elems)) {}
  ASTNodeType getType() const override { return ASTNodeType::ARRAY_LITERAL; }
  std::string toString() const override {
    std::string result = "[";
    for (size_t i = 0; i < elements.size(); ++i) {
      if (i > 0) result += ", ";
      result += elements[i]->toString();
    }
    return result + "]";
  }
  const std::vector<std::unique_ptr<ExprAST>>& getElements() const {
    return elements;
  }
  size_t size() const { return elements.size(); }
  std::unique_ptr<ExprAST> clone() const override;
};

// Array indexing: x[i] or x[i, j, k] for n-dimensional arrays (legacy)
// Uses comma-separated indices: x[0, 1] instead of x[0][1]
class ArrayIndexAST : public ExprAST {
  std::unique_ptr<ExprAST> array;                 // The array being indexed
  std::vector<std::unique_ptr<ExprAST>> indices;  // One or more indices

 public:
  ArrayIndexAST(std::unique_ptr<ExprAST> arr,
                std::vector<std::unique_ptr<ExprAST>> idxs)
      : array(std::move(arr)), indices(std::move(idxs)) {}
  ASTNodeType getType() const override { return ASTNodeType::ARRAY_INDEX; }
  std::string toString() const override {
    std::string result = array->toString() + "[";
    for (size_t i = 0; i < indices.size(); ++i) {
      if (i > 0) result += ", ";
      result += indices[i]->toString();
    }
    return result + "]";
  }
  const ExprAST* getArray() const { return array.get(); }
  const std::vector<std::unique_ptr<ExprAST>>& getIndices() const {
    return indices;
  }
  size_t numIndices() const { return indices.size(); }
  std::unique_ptr<ExprAST> clone() const override;
};

// Slice expression: represents either a single index or a range slice
// Single index: x[5] -> start=5, end=nullptr, isRange=false
// Range slice: x[5:10] -> start=5, end=10, isRange=true
// Partial slices: x[:10], x[5:], x[:] -> isRange=true with nullptr bounds
class SliceExprAST : public ExprAST {
  std::unique_ptr<ExprAST> start_;  // Start index (nullptr = from beginning)
  std::unique_ptr<ExprAST> end_;    // End index (nullptr = to end)
  bool isRange_;                    // true for slice (a:b), false for index (a)

 public:
  // Constructor for single index (isRange=false)
  explicit SliceExprAST(std::unique_ptr<ExprAST> index)
      : start_(std::move(index)), end_(nullptr), isRange_(false) {}

  // Constructor for range slice (isRange=true)
  SliceExprAST(std::unique_ptr<ExprAST> start, std::unique_ptr<ExprAST> end,
               bool isRange)
      : start_(std::move(start)), end_(std::move(end)), isRange_(isRange) {}

  ASTNodeType getType() const override { return ASTNodeType::SLICE; }
  std::string toString() const override {
    if (!isRange_) return start_ ? start_->toString() : "";
    std::string result = start_ ? start_->toString() : "";
    result += ":";
    if (end_) result += end_->toString();
    return result;
  }

  const ExprAST* getStart() const { return start_.get(); }
  const ExprAST* getEnd() const { return end_.get(); }
  bool isRange() const { return isRange_; }
  bool hasStart() const { return start_ != nullptr; }
  bool hasEnd() const { return end_ != nullptr; }
  std::unique_ptr<ExprAST> clone() const override;
};

// Index expression: x[i] or x[i, j, k] for n-dimensional indexing
// Applies to arrays or any type implementing IIndexable
// Each index can be a single value or a slice range
// Uses comma-separated indices: x[0, 1] instead of x[0][1]
// Supports slicing: x[1:10, 3:5] or mixed x[0, 1:5]
class IndexAST : public ExprAST {
  std::unique_ptr<ExprAST>
      target;  // The target being indexed (array or IIndexable)
  std::vector<std::unique_ptr<SliceExprAST>>
      indices;  // One or more index/slice components

 public:
  IndexAST(std::unique_ptr<ExprAST> target,
           std::vector<std::unique_ptr<SliceExprAST>> idxs)
      : target(std::move(target)), indices(std::move(idxs)) {}
  ASTNodeType getType() const override { return ASTNodeType::INDEX; }
  std::string toString() const override {
    std::string result = target->toString() + "[";
    for (size_t i = 0; i < indices.size(); ++i) {
      if (i > 0) result += ", ";
      result += indices[i]->toString();
    }
    return result + "]";
  }
  const ExprAST* getTarget() const { return target.get(); }
  const std::vector<std::unique_ptr<SliceExprAST>>& getIndices() const {
    return indices;
  }
  size_t numIndices() const { return indices.size(); }

  // Check if any index component is a range slice
  bool hasSlices() const {
    for (const auto& idx : indices) {
      if (idx->isRange()) return true;
    }
    return false;
  }
  std::unique_ptr<ExprAST> clone() const override;
};

class VariableReferenceAST : public ExprAST {
  std::string Name;
  sun::QualifiedName qualifiedName_;  // Qualified name after semantic analysis

 public:
  explicit VariableReferenceAST(std::string Name) : Name(std::move(Name)) {}
  ASTNodeType getType() const override {
    return ASTNodeType::VARIABLE_REFERENCE;
  }
  std::string toString() const override { return Name; }
  std::unique_ptr<ExprAST> clone() const override;
  const std::string& getName() const { return Name; }

  // Qualified name (after semantic analysis qualifies it)
  // Returns mangled form for backward compatibility with codegen
  std::string getQualifiedName() const {
    return qualifiedName_.empty() ? Name : qualifiedName_.mangled();
  }
  // Full qualified name info for display/error messages
  const sun::QualifiedName& getQualifiedNameInfo() const {
    return qualifiedName_;
  }
  void setQualifiedName(sun::QualifiedName qname) {
    qualifiedName_ = std::move(qname);
  }
  // Backward compat: set from mangled string (loses display info)
  void setQualifiedName(std::string name) {
    qualifiedName_ = sun::QualifiedName::fromMangled(std::move(name));
  }
  bool hasQualifiedName() const { return !qualifiedName_.empty(); }
};

class VariableCreationAST : public ExprAST {
  std::string name;
  std::unique_ptr<ExprAST> value;
  std::optional<TypeAnnotation> typeAnnotation;

 public:
  explicit VariableCreationAST(
      std::string name, std::unique_ptr<ExprAST> value,
      std::optional<TypeAnnotation> type = std::nullopt)
      : name(std::move(name)),
        value(std::move(value)),
        typeAnnotation(std::move(type)) {}
  ASTNodeType getType() const override {
    return ASTNodeType::VARIABLE_CREATION;
  }
  std::string toString() const override {
    std::string result = "var " + name;
    if (typeAnnotation) result += ": " + typeAnnotation->toString();
    result += " = " + value->toString();
    return result;
  }
  const std::string& getName() const { return name; }
  const ExprAST* getValue() const { return value.get(); }
  const std::optional<TypeAnnotation>& getTypeAnnotation() const {
    return typeAnnotation;
  }
  bool hasTypeAnnotation() const { return typeAnnotation.has_value(); }
  std::unique_ptr<ExprAST> clone() const override;

  // Qualified name (after semantic analysis qualifies it)
  std::string getQualifiedName() const {
    return qualifiedName_.empty() ? name : qualifiedName_.mangled();
  }
  void setQualifiedName(sun::QualifiedName qname) {
    qualifiedName_ = std::move(qname);
  }
  void setQualifiedName(std::string n) {
    qualifiedName_ = sun::QualifiedName::fromMangled(std::move(n));
  }
  bool hasQualifiedName() const { return !qualifiedName_.empty(); }

 private:
  sun::QualifiedName qualifiedName_;
};

class VariableAssignmentAST : public ExprAST {
  std::string name;
  std::unique_ptr<ExprAST> value;

 public:
  explicit VariableAssignmentAST(std::string name,
                                 std::unique_ptr<ExprAST> value)
      : name(std::move(name)), value(std::move(value)) {}
  ASTNodeType getType() const override {
    return ASTNodeType::VARIABLE_ASSIGNMENT;
  }
  std::string toString() const override {
    return name + " = " + value->toString();
  }
  const std::string& getName() const { return name; }
  const ExprAST* getValue() const { return value.get(); }
  std::unique_ptr<ExprAST> clone() const override;
};

// Reference creation: ref x = y (mutable) or ref const x = y (immutable)
// Creates a reference variable x that points to the address of y
class ReferenceCreationAST : public ExprAST {
  std::string name;
  std::unique_ptr<ExprAST> target;  // The expression being referenced
  bool mutable_;                    // true = mutable ref, false = immutable ref

 public:
  explicit ReferenceCreationAST(std::string name,
                                std::unique_ptr<ExprAST> target,
                                bool isMutable = true, Position loc = {})
      : ExprAST(loc),
        name(std::move(name)),
        target(std::move(target)),
        mutable_(isMutable) {}
  ASTNodeType getType() const override {
    return ASTNodeType::REFERENCE_CREATION;
  }
  std::string toString() const override {
    return std::string("ref ") + (mutable_ ? "" : "const ") + name + " = " +
           target->toString();
  }
  const std::string& getName() const { return name; }
  const ExprAST* getTarget() const { return target.get(); }
  bool isMutable() const { return mutable_; }
  std::unique_ptr<ExprAST> clone() const override;

  // Qualified name (after semantic analysis qualifies it)
  std::string getQualifiedName() const {
    return qualifiedName_.empty() ? name : qualifiedName_.mangled();
  }
  void setQualifiedName(sun::QualifiedName qname) {
    qualifiedName_ = std::move(qname);
  }
  void setQualifiedName(std::string n) {
    qualifiedName_ = sun::QualifiedName::fromMangled(std::move(n));
  }
  bool hasQualifiedName() const { return !qualifiedName_.empty(); }

 private:
  sun::QualifiedName qualifiedName_;
};

class BinaryExprAST : public ExprAST {
  Token op;
  std::unique_ptr<ExprAST> LHS, RHS;

 public:
  BinaryExprAST(Token Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : ExprAST(Op.start), op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
  ASTNodeType getType() const override { return ASTNodeType::BINARY; }
  std::string toString() const override {
    return "(" + LHS->toString() + " " + op.text + " " + RHS->toString() + ")";
  }
  Token getOp() const { return op; }
  const ExprAST* getLHS() const { return LHS.get(); }
  const ExprAST* getRHS() const { return RHS.get(); }
  std::unique_ptr<ExprAST> clone() const override;
};

class UnaryExprAST : public ExprAST {
  Token op;
  std::unique_ptr<ExprAST> Operand;

 public:
  UnaryExprAST(Token op, std::unique_ptr<ExprAST> operand)
      : ExprAST(op.start), op(op), Operand(std::move(operand)) {}
  ASTNodeType getType() const override { return ASTNodeType::UNARY; }
  std::string toString() const override {
    return op.text + Operand->toString();
  }
  Token getOp() const { return op; }
  const ExprAST* getOperand() const { return Operand.get(); }
  std::unique_ptr<ExprAST> clone() const override;
};

// Unified call expression - callee is any expression that evaluates to a
// function This handles both direct calls (foo(x)) and indirect calls
// (myFuncVar(x))
class CallExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Callee;  // Expression that evaluates to a function
  std::vector<std::unique_ptr<ExprAST>> Args;

 public:
  CallExprAST(std::unique_ptr<ExprAST> Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(std::move(Callee)), Args(std::move(Args)) {}
  ASTNodeType getType() const override { return ASTNodeType::CALL; }
  std::string toString() const override {
    std::string result = Callee->toString() + "(";
    for (size_t i = 0; i < Args.size(); ++i) {
      if (i > 0) result += ", ";
      result += Args[i]->toString();
    }
    return result + ")";
  }
  const ExprAST* getCallee() const { return Callee.get(); }
  const std::vector<std::unique_ptr<ExprAST>>& getArgs() const { return Args; }
  std::unique_ptr<ExprAST> clone() const override;
};

// Pack expansion expression: args...
// Expands a variadic parameter pack in a call expression
class PackExpansionAST : public ExprAST {
  std::string packName;  // Name of the variadic parameter to expand

 public:
  explicit PackExpansionAST(std::string name) : packName(std::move(name)) {}
  ASTNodeType getType() const override { return ASTNodeType::PACK_EXPANSION; }
  std::string toString() const override { return packName + "..."; }
  std::unique_ptr<ExprAST> clone() const override;
  const std::string& getPackName() const { return packName; }
};

// Top-level nodes (not derived from ExprAST)
class PrototypeAST {
  std::string Name;  // Source name as written by user (for error messages)
  sun::QualifiedName
      qualifiedName_;  // Fully qualified name set by semantic analyzer
  std::vector<std::string> typeParameters;  // Generic type parameters: <T, U>
  std::vector<std::pair<std::string, TypeAnnotation>> args;
  std::optional<TypeAnnotation> returnType;
  std::vector<Capture> captures;
  std::optional<std::string>
      variadicParamName_;  // Name of variadic param if present
  std::optional<TypeAnnotation> variadicConstraint_;  // e.g., _init_args<T>

  // Resolved types for specialized generic functions (set during instantiation)
  // When set, codegen uses these instead of converting type annotations
  std::vector<sun::TypePtr> resolvedParamTypes_;
  bool resolvedParamTypesSet_ = false;  // Track if setResolvedParamTypes called
  sun::TypePtr resolvedReturnType_;
  // Resolved variadic param types (for methods with _init_args<T> constraint)
  // When set, these are the concrete types for the variadic pack
  std::vector<sun::TypePtr> resolvedVariadicTypes_;
  // Type parameter bindings for specialized functions (T -> i32, U -> f64)
  // Used by codegen to resolve type parameters in nested generic calls
  std::vector<std::pair<std::string, sun::TypePtr>> typeBindings_;

 public:
  PrototypeAST(std::string Name,
               std::vector<std::pair<std::string, TypeAnnotation>> args,
               std::optional<TypeAnnotation> retType = std::nullopt,
               std::vector<std::string> typeParams = {},
               std::optional<std::string> variadicParam = std::nullopt,
               std::optional<TypeAnnotation> variadicConstraint = std::nullopt)
      : Name(std::move(Name)),
        typeParameters(std::move(typeParams)),
        args(std::move(args)),
        returnType(std::move(retType)),
        variadicParamName_(std::move(variadicParam)),
        variadicConstraint_(std::move(variadicConstraint)) {}

  void setCaptures(const std::vector<Capture>& caps) { captures = caps; }
  const std::vector<Capture>& getCaptures() const { return captures; }
  bool hasClosure() const { return !captures.empty(); }
  ASTNodeType getType() const { return ASTNodeType::PROTOTYPE; }
  const std::string& getName() const { return Name; }
  void setName(std::string name) { Name = std::move(name); }
  // Get fully qualified name (for codegen/symbol lookup)
  // Returns mangled form for backward compatibility with codegen
  std::string getQualifiedName() const {
    return qualifiedName_.empty() ? Name : qualifiedName_.mangled();
  }
  // Full qualified name info for display/error messages
  const sun::QualifiedName& getQualifiedNameInfo() const {
    return qualifiedName_;
  }
  void setQualifiedName(sun::QualifiedName qname) {
    qualifiedName_ = std::move(qname);
  }
  // Backward compat: set from mangled string (loses display info)
  void setQualifiedName(std::string name) {
    qualifiedName_ = sun::QualifiedName::fromMangled(std::move(name));
  }
  bool hasQualifiedName() const { return !qualifiedName_.empty(); }

  // Generic method support
  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  bool isGeneric() const { return !typeParameters.empty(); }
  void clearTypeParameters() { typeParameters.clear(); }

  const std::vector<std::pair<std::string, TypeAnnotation>>& getArgs() const {
    return args;
  }

  std::vector<std::pair<std::string, TypeAnnotation>>& getMutableArgs() {
    return args;
  }

  std::vector<std::string> getArgNames() const {
    std::vector<std::string> names;
    for (const auto& [name, type] : args) {
      names.push_back(name);
    }
    return names;
  }

  const std::optional<TypeAnnotation>& getReturnType() const {
    return returnType;
  }
  bool hasReturnType() const { return returnType.has_value(); }

  // Set the return type (used by semantic analyzer for type inference)
  void setReturnType(TypeAnnotation type) { returnType = std::move(type); }

  // Variadic parameter support
  bool hasVariadicParam() const { return variadicParamName_.has_value(); }
  const std::optional<std::string>& getVariadicParamName() const {
    return variadicParamName_;
  }
  bool hasVariadicConstraint() const { return variadicConstraint_.has_value(); }
  const std::optional<TypeAnnotation>& getVariadicConstraint() const {
    return variadicConstraint_;
  }

  // Resolved types for specialized generic functions
  // Set during instantiation, used by codegen to skip type annotation
  // conversion
  void setResolvedParamTypes(std::vector<sun::TypePtr> types) {
    resolvedParamTypes_ = std::move(types);
    resolvedParamTypesSet_ = true;
  }
  const std::vector<sun::TypePtr>& getResolvedParamTypes() const {
    return resolvedParamTypes_;
  }
  bool hasResolvedParamTypes() const { return resolvedParamTypesSet_; }

  void setResolvedReturnType(sun::TypePtr type) {
    resolvedReturnType_ = std::move(type);
  }
  sun::TypePtr getResolvedReturnType() const { return resolvedReturnType_; }
  bool hasResolvedReturnType() const { return resolvedReturnType_ != nullptr; }

  // Resolved variadic param types for _init_args<T> expansion
  void setResolvedVariadicTypes(std::vector<sun::TypePtr> types) {
    resolvedVariadicTypes_ = std::move(types);
  }
  const std::vector<sun::TypePtr>& getResolvedVariadicTypes() const {
    return resolvedVariadicTypes_;
  }
  bool hasResolvedVariadicTypes() const {
    return !resolvedVariadicTypes_.empty();
  }

  // Type parameter bindings for specialized generic functions
  void setTypeBindings(
      std::vector<std::pair<std::string, sun::TypePtr>> bindings) {
    typeBindings_ = std::move(bindings);
  }
  const std::vector<std::pair<std::string, sun::TypePtr>>& getTypeBindings()
      const {
    return typeBindings_;
  }
  bool hasTypeBindings() const { return !typeBindings_.empty(); }

  // Clone the prototype (deep copy)
  std::unique_ptr<PrototypeAST> clone() const;
};

class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Then, Else;

 public:
  IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Then,
            std::unique_ptr<ExprAST> Else)
      : Cond(std::move(Cond)), Then(std::move(Then)), Else(std::move(Else)) {}
  ASTNodeType getType() const override { return ASTNodeType::IF; }
  std::string toString() const override {
    std::string result = "if (" + Cond->toString() + ") " + Then->toString();
    if (Else) result += " else " + Else->toString();
    return result;
  }
  ExprAST* getCond() const { return Cond.get(); }
  ExprAST* getThen() const { return Then.get(); }
  ExprAST* getElse() const { return Else.get(); }
  std::unique_ptr<ExprAST> clone() const override;
};

// A single arm in a match expression: pattern => body
struct MatchArm {
  std::unique_ptr<ExprAST> pattern;  // nullptr for wildcard _
  bool isWildcard;                   // true if this arm is _
  std::unique_ptr<ExprAST> body;

  MatchArm(std::unique_ptr<ExprAST> pattern, bool isWildcard,
           std::unique_ptr<ExprAST> body)
      : pattern(std::move(pattern)),
        isWildcard(isWildcard),
        body(std::move(body)) {}

  // Move constructor
  MatchArm(MatchArm&& other) = default;
  MatchArm& operator=(MatchArm&& other) = default;

  // No copy
  MatchArm(const MatchArm&) = delete;
  MatchArm& operator=(const MatchArm&) = delete;
};

class MatchExprAST : public ExprAST {
  std::unique_ptr<ExprAST> discriminant;  // The value being matched
  std::vector<MatchArm> arms;             // Match arms

 public:
  MatchExprAST(std::unique_ptr<ExprAST> discriminant,
               std::vector<MatchArm> arms)
      : discriminant(std::move(discriminant)), arms(std::move(arms)) {}

  ASTNodeType getType() const override { return ASTNodeType::MATCH; }

  std::string toString() const override {
    std::string result = "match " + discriminant->toString() + " {";
    for (size_t i = 0; i < arms.size(); ++i) {
      if (i > 0) result += ", ";
      if (arms[i].isWildcard) {
        result += "_";
      } else {
        result += arms[i].pattern->toString();
      }
      result += " => " + arms[i].body->toString();
    }
    result += "}";
    return result;
  }

  const ExprAST* getDiscriminant() const { return discriminant.get(); }
  const std::vector<MatchArm>& getArms() const { return arms; }
  std::unique_ptr<ExprAST> clone() const override;
};

class ForExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Init;       // Initialization (can be null)
  std::unique_ptr<ExprAST> Condition;  // Condition (can be null for infinite)
  std::unique_ptr<ExprAST> Increment;  // Increment (can be null)
  std::unique_ptr<ExprAST> Body;

 public:
  ForExprAST(std::unique_ptr<ExprAST> Init, std::unique_ptr<ExprAST> Condition,
             std::unique_ptr<ExprAST> Increment, std::unique_ptr<ExprAST> Body)
      : Init(std::move(Init)),
        Condition(std::move(Condition)),
        Increment(std::move(Increment)),
        Body(std::move(Body)) {}

  ASTNodeType getType() const override { return ASTNodeType::FOR_LOOP; }
  std::string toString() const override {
    std::string result = "for (";
    if (Init) result += Init->toString();
    result += "; ";
    if (Condition) result += Condition->toString();
    result += "; ";
    if (Increment) result += Increment->toString();
    result += ") " + Body->toString();
    return result;
  }

  const ExprAST* getInit() const { return Init.get(); }
  const ExprAST* getCondition() const { return Condition.get(); }
  const ExprAST* getIncrement() const { return Increment.get(); }
  const ExprAST* getBody() const { return Body.get(); }
  std::unique_ptr<ExprAST> clone() const override;
};

// for (var x: T in iterable) { ... }
// Iterates over iterable by calling iter() -> IIterator<T>,
// then hasNext() -> bool, next() -> T in a loop
class ForInExprAST : public ExprAST {
  std::string LoopVar;                // Variable name (x)
  TypeAnnotation LoopVarType;         // Type annotation (T)
  std::unique_ptr<ExprAST> Iterable;  // Expression that yields an iterable
  std::unique_ptr<ExprAST> Body;

  // Resolved loop variable type (set by semantic analyzer)
  mutable sun::TypePtr resolvedLoopVarType_;

 public:
  ForInExprAST(std::string LoopVar, TypeAnnotation LoopVarType,
               std::unique_ptr<ExprAST> Iterable, std::unique_ptr<ExprAST> Body)
      : LoopVar(std::move(LoopVar)),
        LoopVarType(std::move(LoopVarType)),
        Iterable(std::move(Iterable)),
        Body(std::move(Body)) {}

  ASTNodeType getType() const override { return ASTNodeType::FOR_IN_LOOP; }
  std::string toString() const override {
    return "for (var " + LoopVar + ": " + LoopVarType.toString() + " in " +
           Iterable->toString() + ") " + Body->toString();
  }

  const std::string& getLoopVar() const { return LoopVar; }
  const TypeAnnotation& getLoopVarType() const { return LoopVarType; }
  const ExprAST* getIterable() const { return Iterable.get(); }
  const ExprAST* getBody() const { return Body.get(); }

  // Resolved loop variable type (set by semantic analyzer)
  void setResolvedLoopVarType(sun::TypePtr type) const {
    resolvedLoopVarType_ = std::move(type);
  }
  sun::TypePtr getResolvedLoopVarType() const { return resolvedLoopVarType_; }
  bool hasResolvedLoopVarType() const {
    return resolvedLoopVarType_ != nullptr;
  }

  std::unique_ptr<ExprAST> clone() const override;
};

class WhileExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Condition, Body;

 public:
  WhileExprAST(std::unique_ptr<ExprAST> Condition,
               std::unique_ptr<ExprAST> Body)
      : Condition(std::move(Condition)), Body(std::move(Body)) {}

  ASTNodeType getType() const override { return ASTNodeType::WHILE_LOOP; }
  std::string toString() const override {
    return "while (" + Condition->toString() + ") " + Body->toString();
  }

  const ExprAST* getCondition() const { return Condition.get(); }
  const ExprAST* getBody() const { return Body.get(); }
  std::unique_ptr<ExprAST> clone() const override;
};

class BlockExprAST : public ExprAST {
  std::vector<std::unique_ptr<ExprAST>> Body;

 public:
  BlockExprAST() = default;

  explicit BlockExprAST(std::vector<std::unique_ptr<ExprAST>> body)
      : Body(std::move(body)) {}

  void addExpression(std::unique_ptr<ExprAST> expr) {
    Body.push_back(std::move(expr));
  }

  ASTNodeType getType() const override { return ASTNodeType::BLOCK; }
  std::string toString() const override { return "{ ... }"; }

  const std::vector<std::unique_ptr<ExprAST>>& getBody() const { return Body; }

  // Optional: convenience method to check if block is empty
  bool isEmpty() const { return Body.empty(); }

  // Optional: get the last expression (common when evaluating blocks)
  const ExprAST* getLastExpr() const {
    return Body.empty() ? nullptr : Body.back().get();
  }
  std::unique_ptr<ExprAST> clone() const override;
};

// Indexed assignment: arr[i] = value
class IndexedAssignmentAST : public ExprAST {
  std::unique_ptr<ExprAST> target;  // The indexed expression (e.g., x[0])
  std::unique_ptr<ExprAST> value;   // The value to assign

 public:
  IndexedAssignmentAST(std::unique_ptr<ExprAST> target,
                       std::unique_ptr<ExprAST> value)
      : target(std::move(target)), value(std::move(value)) {}

  ASTNodeType getType() const override {
    return ASTNodeType::INDEXED_ASSIGNMENT;
  }
  std::string toString() const override {
    return target->toString() + " = " + value->toString();
  }
  const ExprAST* getTarget() const { return target.get(); }
  const ExprAST* getValue() const { return value.get(); }
  std::unique_ptr<ExprAST> clone() const override;
};

class FunctionAST : public ExprAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<BlockExprAST> Body;
  std::string sourceText_;  // Original source code (for generic method storage)
  // Specialized versions of this generic function, keyed by mangled type args
  // Mutable because specializations are added during semantic analysis of calls
  mutable std::map<std::string, std::shared_ptr<FunctionAST>> specializations_;

 public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<BlockExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}

  ASTNodeType getType() const override { return ASTNodeType::FUNCTION; }
  std::string toString() const override {
    std::string result = "function " + Proto->getName() + "(";
    const auto& args = Proto->getArgs();
    for (size_t i = 0; i < args.size(); ++i) {
      if (i > 0) result += ", ";
      result += args[i].first + ": " + args[i].second.toString();
    }
    result += ")";
    if (Proto->hasReturnType())
      result += " " + Proto->getReturnType()->toString();
    if (Body) result += " " + Body->toString();
    return result;
  }

  // Add this method to allow moving the prototype out
  std::unique_ptr<PrototypeAST> releaseProto() { return std::move(Proto); }

  const PrototypeAST& getProto() const { return *Proto; }
  const BlockExprAST& getBody() const { return *Body; }

  // Set body (for lazy parsing - replace empty stub with parsed body)
  void setBody(std::unique_ptr<BlockExprAST> newBody) {
    Body = std::move(newBody);
  }

  // Source text for serialization (generic methods in moon)
  void setSourceText(std::string src) { sourceText_ = std::move(src); }
  const std::string& getSourceText() const { return sourceText_; }
  bool hasSourceText() const { return !sourceText_.empty(); }

  // Check if function is an extern declaration (no body)
  bool isExtern() const { return Body == nullptr; }
  bool hasBody() const { return Body != nullptr; }

  // Specialization storage for generic functions
  // Called by semantic analyzer when a generic function is instantiated
  void addSpecialization(const std::string& mangledName,
                         std::shared_ptr<FunctionAST> specializedAST) const {
    specializations_[mangledName] = std::move(specializedAST);
  }
  const std::map<std::string, std::shared_ptr<FunctionAST>>&
  getSpecializations() const {
    return specializations_;
  }
  bool hasSpecialization(const std::string& mangledName) const {
    return specializations_.find(mangledName) != specializations_.end();
  }
  std::shared_ptr<FunctionAST> getSpecialization(
      const std::string& mangledName) const {
    auto it = specializations_.find(mangledName);
    return it != specializations_.end() ? it->second : nullptr;
  }

  std::unique_ptr<ExprAST> clone() const override;
};

// Lambda expression (anonymous function)
class LambdaAST : public ExprAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<BlockExprAST> Body;

 public:
  LambdaAST(std::unique_ptr<PrototypeAST> Proto,
            std::unique_ptr<BlockExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}

  ASTNodeType getType() const override { return ASTNodeType::LAMBDA; }
  std::string toString() const override {
    std::string result = "lambda(";
    const auto& args = Proto->getArgs();
    for (size_t i = 0; i < args.size(); ++i) {
      if (i > 0) result += ", ";
      result += args[i].first + ": " + args[i].second.toString();
    }
    result += ")";
    if (Proto->hasReturnType())
      result += " " + Proto->getReturnType()->toString();
    if (Body) result += " " + Body->toString();
    return result;
  }

  const PrototypeAST& getProto() const { return *Proto; }
  const BlockExprAST& getBody() const { return *Body; }
  bool hasBody() const { return Body != nullptr; }

  std::unique_ptr<ExprAST> clone() const override;
};

// Return statement: return <expr>;
class ReturnExprAST : public ExprAST {
  std::unique_ptr<ExprAST>
      Value;  // The expression to return (may be nullptr for void)

 public:
  explicit ReturnExprAST(std::unique_ptr<ExprAST> value = nullptr)
      : Value(std::move(value)) {}

  ASTNodeType getType() const override { return ASTNodeType::RETURN; }
  std::string toString() const override {
    if (Value) return "return " + Value->toString();
    return "return";
  }

  const ExprAST* getValue() const { return Value.get(); }
  bool hasValue() const { return Value != nullptr; }
  std::unique_ptr<ExprAST> clone() const override;
};

// Break statement: break;
// Exits the innermost enclosing loop
class BreakAST : public ExprAST {
 public:
  BreakAST() = default;

  ASTNodeType getType() const override { return ASTNodeType::BREAK_STMT; }
  std::string toString() const override { return "break"; }
  std::unique_ptr<ExprAST> clone() const override;
};

// Continue statement: continue;
// Jumps to the next iteration of the innermost enclosing loop
class ContinueAST : public ExprAST {
 public:
  ContinueAST() = default;

  ASTNodeType getType() const override { return ASTNodeType::CONTINUE_STMT; }
  std::string toString() const override { return "continue"; }
  std::unique_ptr<ExprAST> clone() const override;
};

// Throw expression: throw <expr>
// Used to throw an error from a function declared with ", IError"
class ThrowExprAST : public ExprAST {
  std::unique_ptr<ExprAST> errorExpr;  // The error expression to throw

 public:
  explicit ThrowExprAST(std::unique_ptr<ExprAST> expr)
      : errorExpr(std::move(expr)) {}

  ASTNodeType getType() const override { return ASTNodeType::THROW; }
  std::string toString() const override {
    return "throw " + errorExpr->toString();
  }

  const ExprAST& getErrorExpr() const { return *errorExpr; }
  bool hasErrorExpr() const { return errorExpr != nullptr; }
  std::unique_ptr<ExprAST> clone() const override;
};

// Catch clause for try-catch expression
// Represents: catch (name: Type) { body }
struct CatchClause {
  std::string bindingName;                    // variable name for error binding
  std::optional<TypeAnnotation> bindingType;  // type annotation (e.g., IError)
  std::unique_ptr<BlockExprAST> body;         // the catch body

  CatchClause() = default;
  CatchClause(CatchClause&&) = default;
  CatchClause& operator=(CatchClause&&) = default;
};

// Try-catch expression: try { ... } catch (e: IError) { ... }
// Modern exception handling syntax
class TryCatchExprAST : public ExprAST {
  std::unique_ptr<BlockExprAST> tryBlock;  // The try block
  CatchClause catchClause;                 // The catch handler

 public:
  TryCatchExprAST(std::unique_ptr<BlockExprAST> tryBlk, CatchClause catchCls)
      : tryBlock(std::move(tryBlk)), catchClause(std::move(catchCls)) {}

  ASTNodeType getType() const override { return ASTNodeType::TRY_CATCH; }
  std::string toString() const override {
    std::string result =
        "try " + tryBlock->toString() + " catch (" + catchClause.bindingName;
    if (catchClause.bindingType)
      result += ": " + catchClause.bindingType->toString();
    result += ") " + catchClause.body->toString();
    return result;
  }

  const BlockExprAST& getTryBlock() const { return *tryBlock; }
  const CatchClause& getCatchClause() const { return catchClause; }
  std::unique_ptr<ExprAST> clone() const override;
};

// Import declaration: import "path/to/module.sun";
class ImportAST : public ExprAST {
  std::string path;  // The file path to import

 public:
  explicit ImportAST(std::string path) : path(std::move(path)) {}

  ASTNodeType getType() const override { return ASTNodeType::IMPORT; }
  std::string toString() const override { return "import \"" + path + "\""; }

  std::unique_ptr<ExprAST> clone() const override;
  const std::string& getPath() const { return path; }
};

// Qualified name expression: Module.name or Namespace::name
class QualifiedNameAST : public ExprAST {
  std::vector<std::string> parts;  // ["sun", "Vec"] for sun.Vec

  // Resolved mangled name (set by semantic analyzer) including library hash
  mutable std::string resolvedMangledName_;

 public:
  explicit QualifiedNameAST(std::vector<std::string> parts)
      : parts(std::move(parts)) {}

  ASTNodeType getType() const override { return ASTNodeType::QUALIFIED_NAME; }
  std::string toString() const override { return getFullName(); }

  const std::vector<std::string>& getParts() const { return parts; }

  // Get the namespace/module path (all parts except the last)
  std::vector<std::string> getNamespacePath() const {
    if (parts.size() <= 1) return {};
    return std::vector<std::string>(parts.begin(), parts.end() - 1);
  }

  // Get the final name (last part)
  const std::string& getName() const { return parts.back(); }

  // Get fully qualified name as string with dot separator (e.g., "sun.Vec")
  std::string getFullName() const {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) result += ".";
      result += parts[i];
    }
    return result;
  }

  // Get mangled name for LLVM symbols (e.g., "sun_Vec")
  // Uses resolved name if set by semantic analyzer, otherwise computes from
  // parts
  std::string getMangledName() const {
    if (!resolvedMangledName_.empty()) return resolvedMangledName_;
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) result += "_";
      result += parts[i];
    }
    return result;
  }

  void setResolvedMangledName(std::string name) const {
    resolvedMangledName_ = std::move(name);
  }

  std::unique_ptr<ExprAST> clone() const override;
};

// Module/Namespace declaration: module Name { declarations... }
// Also supports legacy 'namespace' keyword
class NamespaceAST : public ExprAST {
  std::string name;
  std::unique_ptr<BlockExprAST> body;

 public:
  NamespaceAST(std::string name, std::unique_ptr<BlockExprAST> body)
      : name(std::move(name)), body(std::move(body)) {}

  ASTNodeType getType() const override { return ASTNodeType::NAMESPACE; }
  std::string toString() const override {
    return "namespace " + name + " " + body->toString();
  }

  const std::string& getName() const { return name; }
  const BlockExprAST& getBody() const { return *body; }
  std::unique_ptr<ExprAST> clone() const override;
};

// Using declaration: using Namespace::name; or using Namespace::*;
// Also supports: using Module; (imports all), using Module.prefix*; (prefix)
class UsingAST : public ExprAST {
  std::vector<std::string> namespacePath;  // The namespace path
  std::string target;  // The specific name, "*" for wildcard, or "prefix*" for
                       // prefix wildcard
  bool isWildcard;
  bool isPrefixWildcard;
  std::string prefix;  // For prefix wildcards, the prefix without '*'

 public:
  // For using Namespace::name; or using Module.prefix*;
  UsingAST(std::vector<std::string> nsPath, std::string targetName)
      : namespacePath(std::move(nsPath)),
        target(std::move(targetName)),
        isWildcard(target == "*"),
        isPrefixWildcard(false) {
    // Check for prefix wildcard (e.g., "Mat*")
    if (!isWildcard && target.size() > 1 && target.back() == '*') {
      isPrefixWildcard = true;
      prefix = target.substr(0, target.size() - 1);
    }
  }

  ASTNodeType getType() const override { return ASTNodeType::USING; }
  std::string toString() const override {
    return "using " + getNamespacePathString() + "." + target;
  }

  const std::vector<std::string>& getNamespacePath() const {
    return namespacePath;
  }
  const std::string& getTarget() const { return target; }
  bool isWildcardImport() const { return isWildcard; }
  bool isPrefixWildcardImport() const { return isPrefixWildcard; }
  const std::string& getPrefix() const { return prefix; }

  // Get the full path as string (e.g., "Math.Trig" or "Math::Trig")
  std::string getNamespacePathString() const {
    std::string result;
    for (size_t i = 0; i < namespacePath.size(); ++i) {
      if (i > 0) result += ".";
      result += namespacePath[i];
    }
    return result;
  }
  std::unique_ptr<ExprAST> clone() const override;
};

// ============================================================================
// Class-related AST nodes
// ============================================================================

// Field declaration in a class: var name: type;
struct ClassFieldDecl {
  std::string name;
  TypeAnnotation type;
  Position location;  // Source location of field declaration
};

// Method declaration in a class (uses FunctionAST internally)
struct ClassMethodDecl {
  std::unique_ptr<FunctionAST> function;
  bool isConstructor;  // true if method name is "init"
};

// Implemented interface with optional type arguments
// e.g., IIterator<T> or IComparable<i32>
struct ImplementedInterfaceAST {
  std::string name;                           // Interface name: "IIterator"
  std::vector<TypeAnnotation> typeArguments;  // Type args: [T] or [i32]
};

// Class definition: class Name<T, U> implements Interface1<T>, Interface2 {
// fields and methods }
class ClassDefinitionAST : public ExprAST {
  std::string name;  // Source name as written by user (for error messages)
  sun::QualifiedName
      qualifiedName_;  // Fully qualified name set by semantic analyzer
  std::vector<std::string>
      typeParameters;  // Generic type parameters: T, U, etc.
  std::vector<ImplementedInterfaceAST>
      implementedInterfaces;  // Interfaces with type args
  std::vector<ClassFieldDecl> fields;
  std::vector<ClassMethodDecl> methods;

  // Specialized versions of this generic class, keyed by mangled type args
  // (e.g., "Vec_i32"). Mutable because specializations are added during
  // semantic analysis of usages after the generic class AST is created.
  mutable std::map<std::string, std::shared_ptr<ClassDefinitionAST>>
      specializations_;

 public:
  ClassDefinitionAST(std::string name, std::vector<std::string> typeParams,
                     std::vector<ImplementedInterfaceAST> interfaces,
                     std::vector<ClassFieldDecl> fields,
                     std::vector<ClassMethodDecl> methods,
                     bool precompiled = false)
      : name(std::move(name)),
        typeParameters(std::move(typeParams)),
        implementedInterfaces(std::move(interfaces)),
        fields(std::move(fields)),
        methods(std::move(methods)) {
    precompiled_ = precompiled;
  }

  ASTNodeType getType() const override { return ASTNodeType::CLASS_DEFINITION; }
  std::string toString() const override {
    std::string result = "class " + name;
    if (!typeParameters.empty()) {
      result += "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i];
      }
      result += ">";
    }
    if (!implementedInterfaces.empty()) {
      result += " implements ";
      for (size_t i = 0; i < implementedInterfaces.size(); ++i) {
        if (i > 0) result += ", ";
        result += implementedInterfaces[i].name;
        if (!implementedInterfaces[i].typeArguments.empty()) {
          result += "<...>";
        }
      }
    }
    result += " { ... }";
    return result;
  }

  const std::string& getName() const { return name; }
  // Get fully qualified name (for codegen/symbol lookup)
  // Returns mangled form for backward compatibility with codegen
  std::string getQualifiedName() const {
    return qualifiedName_.empty() ? name : qualifiedName_.mangled();
  }
  // Full qualified name info for display/error messages
  const sun::QualifiedName& getQualifiedNameInfo() const {
    return qualifiedName_;
  }
  void setQualifiedName(sun::QualifiedName qname) {
    qualifiedName_ = std::move(qname);
  }
  // Backward compat: set from mangled string (loses display info)
  void setQualifiedName(std::string name) {
    qualifiedName_ = sun::QualifiedName::fromMangled(std::move(name));
  }
  bool hasQualifiedName() const { return !qualifiedName_.empty(); }
  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  bool isGeneric() const { return !typeParameters.empty(); }
  const std::vector<ImplementedInterfaceAST>& getImplementedInterfaces() const {
    return implementedInterfaces;
  }
  const std::vector<ClassFieldDecl>& getFields() const { return fields; }
  const std::vector<ClassMethodDecl>& getMethods() const { return methods; }

  // Find the constructor (init method)
  const ClassMethodDecl* getConstructor() const {
    for (const auto& method : methods) {
      if (method.isConstructor) return &method;
    }
    return nullptr;
  }

  // Specialization storage for generic classes
  // Called by semantic analyzer when a generic class is instantiated
  void addSpecialization(
      const std::string& mangledName,
      std::shared_ptr<ClassDefinitionAST> specializedAST) const {
    specializations_[mangledName] = std::move(specializedAST);
  }
  const std::map<std::string, std::shared_ptr<ClassDefinitionAST>>&
  getSpecializations() const {
    return specializations_;
  }
  bool hasSpecialization(const std::string& mangledName) const {
    return specializations_.find(mangledName) != specializations_.end();
  }
  std::shared_ptr<ClassDefinitionAST> getSpecialization(
      const std::string& mangledName) const {
    auto it = specializations_.find(mangledName);
    return it != specializations_.end() ? it->second : nullptr;
  }

  std::unique_ptr<ExprAST> clone() const override;
};

// ============================================================================
// Interface-related AST nodes
// ============================================================================

// Field declaration in an interface: var name: type;
struct InterfaceFieldDecl {
  std::string name;
  TypeAnnotation type;
  Position location;  // Source location of field declaration
};

// Method declaration in an interface (uses FunctionAST internally)
// Methods can have default implementations
struct InterfaceMethodDecl {
  std::unique_ptr<FunctionAST> function;
  bool hasDefaultImpl;  // true if method has a body (default implementation)
};

// Interface definition: interface Name<T, U> { fields and methods }
class InterfaceDefinitionAST : public ExprAST {
  std::string name;  // Source name as written by user (for error messages)
  sun::QualifiedName
      qualifiedName_;  // Fully qualified name set by semantic analyzer
  std::vector<std::string>
      typeParameters;  // Generic type parameters: T, U, etc.
  std::vector<InterfaceFieldDecl> fields;
  std::vector<InterfaceMethodDecl> methods;

 public:
  InterfaceDefinitionAST(std::string name, std::vector<std::string> typeParams,
                         std::vector<InterfaceFieldDecl> fields,
                         std::vector<InterfaceMethodDecl> methods,
                         bool precompiled = false)
      : name(std::move(name)),
        typeParameters(std::move(typeParams)),
        fields(std::move(fields)),
        methods(std::move(methods)) {
    precompiled_ = precompiled;
  }

  ASTNodeType getType() const override {
    return ASTNodeType::INTERFACE_DEFINITION;
  }
  std::string toString() const override {
    std::string result = "interface " + name;
    if (!typeParameters.empty()) {
      result += "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i];
      }
      result += ">";
    }
    result += " { ... }";
    return result;
  }

  const std::string& getName() const { return name; }
  // Get fully qualified name (for codegen/symbol lookup)
  // Returns mangled form for backward compatibility with codegen
  std::string getQualifiedName() const {
    return qualifiedName_.empty() ? name : qualifiedName_.mangled();
  }
  // Full qualified name info for display/error messages
  const sun::QualifiedName& getQualifiedNameInfo() const {
    return qualifiedName_;
  }
  void setQualifiedName(sun::QualifiedName qname) {
    qualifiedName_ = std::move(qname);
  }
  // Backward compat: set from mangled string (loses display info)
  void setQualifiedName(std::string name) {
    qualifiedName_ = sun::QualifiedName::fromMangled(std::move(name));
  }
  bool hasQualifiedName() const { return !qualifiedName_.empty(); }
  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  bool isGeneric() const { return !typeParameters.empty(); }
  const std::vector<InterfaceFieldDecl>& getFields() const { return fields; }
  const std::vector<InterfaceMethodDecl>& getMethods() const { return methods; }

  // Get methods with default implementations
  std::vector<const InterfaceMethodDecl*> getDefaultMethods() const {
    std::vector<const InterfaceMethodDecl*> defaults;
    for (const auto& method : methods) {
      if (method.hasDefaultImpl) defaults.push_back(&method);
    }
    return defaults;
  }

  // Get methods without default implementations (must be implemented by class)
  std::vector<const InterfaceMethodDecl*> getRequiredMethods() const {
    std::vector<const InterfaceMethodDecl*> required;
    for (const auto& method : methods) {
      if (!method.hasDefaultImpl) required.push_back(&method);
    }
    return required;
  }
  std::unique_ptr<ExprAST> clone() const override;
};

// ============================================================================
// Enum-related AST nodes
// ============================================================================

// Enum variant declaration: Red, Green, Blue (for now, without associated data)
struct EnumVariantDecl {
  std::string name;
  int64_t value;      // Explicit or implicit numeric value
  Position location;  // Source location of variant declaration
};

// Enum definition: enum Name { Variant1, Variant2, ... }
// For now, simple C-style enums without associated data
class EnumDefinitionAST : public ExprAST {
  std::string name;
  std::vector<EnumVariantDecl> variants;

 public:
  EnumDefinitionAST(std::string name, std::vector<EnumVariantDecl> variants,
                    bool precompiled = false)
      : name(std::move(name)), variants(std::move(variants)) {
    precompiled_ = precompiled;
  }

  ASTNodeType getType() const override { return ASTNodeType::ENUM_DEFINITION; }
  std::string toString() const override {
    std::string result = "enum " + name + " { ";
    for (size_t i = 0; i < variants.size(); ++i) {
      if (i > 0) result += ", ";
      result += variants[i].name;
    }
    result += " }";
    return result;
  }

  const std::string& getName() const { return name; }
  const std::vector<EnumVariantDecl>& getVariants() const { return variants; }

  // Get a variant by name
  const EnumVariantDecl* getVariant(const std::string& variantName) const {
    for (const auto& variant : variants) {
      if (variant.name == variantName) return &variant;
    }
    return nullptr;
  }

  // Get the number of variants
  size_t getNumVariants() const { return variants.size(); }
  std::unique_ptr<ExprAST> clone() const override;
};

// Member access expression: object.fieldName or object.methodName
// For method calls, this is wrapped in CallExprAST
// For generic method calls like object.method<T>(), typeArguments will be
// populated
class MemberAccessAST : public ExprAST {
  std::unique_ptr<ExprAST> object;  // The object being accessed
  std::string memberName;           // The field or method name
  std::vector<std::unique_ptr<TypeAnnotation>>
      typeArguments;  // Generic type arguments for methods

  // Resolved type arguments for generic method calls (set by semantic analyzer)
  // These are the concrete types after type parameter substitution
  mutable std::vector<sun::TypePtr> resolvedTypeArgs_;

  // Resolved qualified name for module member access (set by semantic analyzer)
  // e.g., "$d9b854ae$_b_get_version" for b.get_version() from a library
  mutable std::string resolvedQualifiedName_;

 public:
  MemberAccessAST(std::unique_ptr<ExprAST> obj, std::string member,
                  std::vector<std::unique_ptr<TypeAnnotation>> typeArgs = {})
      : object(std::move(obj)),
        memberName(std::move(member)),
        typeArguments(std::move(typeArgs)) {}

  ASTNodeType getType() const override { return ASTNodeType::MEMBER_ACCESS; }
  std::string toString() const override {
    std::string result = object->toString() + "." + memberName;
    if (!typeArguments.empty()) {
      result += "<";
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toString();
      }
      result += ">";
    }
    return result;
  }

  const ExprAST* getObject() const { return object.get(); }
  const std::string& getMemberName() const { return memberName; }
  bool hasTypeArguments() const { return !typeArguments.empty(); }
  const std::vector<std::unique_ptr<TypeAnnotation>>& getTypeArguments() const {
    return typeArguments;
  }

  // Resolved type arguments for generic method calls (set by semantic analyzer)
  void setResolvedTypeArgs(std::vector<sun::TypePtr> types) const {
    resolvedTypeArgs_ = std::move(types);
  }
  const std::vector<sun::TypePtr>& getResolvedTypeArgs() const {
    return resolvedTypeArgs_;
  }
  bool hasResolvedTypeArgs() const { return !resolvedTypeArgs_.empty(); }

  // Resolved qualified name for module member access (set by semantic analyzer)
  void setResolvedQualifiedName(std::string name) const {
    resolvedQualifiedName_ = std::move(name);
  }
  const std::string& getResolvedQualifiedName() const {
    return resolvedQualifiedName_;
  }
  bool hasResolvedQualifiedName() const {
    return !resolvedQualifiedName_.empty();
  }

  std::unique_ptr<ExprAST> clone() const override;
};

// this keyword - reference to current object instance
class ThisExprAST : public ExprAST {
 public:
  ThisExprAST() = default;

  ASTNodeType getType() const override { return ASTNodeType::THIS; }
  std::string toString() const override { return "this"; }
  std::unique_ptr<ExprAST> clone() const override;
};

// Generic function call: create<Type>(args...) or create<Type1, Type2>(args...)
// Used for generic free functions like create<T>, destroy, etc.
class GenericCallAST : public ExprAST {
  std::string functionName;  // e.g., "create", "destroy"
  std::vector<std::unique_ptr<TypeAnnotation>>
      typeArguments;                           // All type parameters
  std::vector<std::unique_ptr<ExprAST>> args;  // Function arguments

  // Resolved type arguments (set by semantic analyzer)
  // These are the concrete types after type parameter substitution
  mutable std::vector<sun::TypePtr> resolvedTypeArgs_;

  // Generic function AST (set by semantic analyzer for user-defined generic
  // functions) This allows codegen to access specializations without tracking
  // a separate map
  mutable const FunctionAST* genericFunctionAST_ = nullptr;

 public:
  GenericCallAST(std::string name,
                 std::vector<std::unique_ptr<TypeAnnotation>> typeArgs,
                 std::vector<std::unique_ptr<ExprAST>> arguments)
      : functionName(std::move(name)), args(std::move(arguments)) {
    typeArguments = std::move(typeArgs);
  }

  ASTNodeType getType() const override { return ASTNodeType::GENERIC_CALL; }
  std::string toString() const override {
    std::string result = functionName + "<";
    if (!typeArguments.empty()) {
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toString();
      }
    }
    result += ">(";
    for (size_t i = 0; i < args.size(); ++i) {
      if (i > 0) result += ", ";
      result += args[i]->toString();
    }
    return result + ")";
  }

  const std::string& getFunctionName() const { return functionName; }
  const std::vector<std::unique_ptr<TypeAnnotation>>& getTypeArguments() const {
    return typeArguments;
  }
  const std::vector<std::unique_ptr<ExprAST>>& getArgs() const { return args; }

  // Resolved type arguments (set by semantic analyzer after type param
  // substitution)
  void setResolvedTypeArgs(std::vector<sun::TypePtr> types) const {
    resolvedTypeArgs_ = std::move(types);
  }
  const std::vector<sun::TypePtr>& getResolvedTypeArgs() const {
    return resolvedTypeArgs_;
  }
  bool hasResolvedTypeArgs() const { return !resolvedTypeArgs_.empty(); }

  // Generic function AST (set by semantic analyzer)
  void setGenericFunctionAST(const FunctionAST* ast) const {
    genericFunctionAST_ = ast;
  }
  const FunctionAST* getGenericFunctionAST() const {
    return genericFunctionAST_;
  }
  bool hasGenericFunctionAST() const { return genericFunctionAST_ != nullptr; }

  std::unique_ptr<ExprAST> clone() const override;
};

// Member assignment: object.field = value
class MemberAssignmentAST : public ExprAST {
  std::unique_ptr<ExprAST> object;  // The object (can be 'this' or any expr)
  std::string memberName;           // The field name
  std::unique_ptr<ExprAST> value;   // The value to assign

 public:
  MemberAssignmentAST(std::unique_ptr<ExprAST> obj, std::string member,
                      std::unique_ptr<ExprAST> val)
      : object(std::move(obj)),
        memberName(std::move(member)),
        value(std::move(val)) {}

  ASTNodeType getType() const override {
    return ASTNodeType::MEMBER_ASSIGNMENT;
  }
  std::string toString() const override {
    return object->toString() + "." + memberName + " = " + value->toString();
  }

  const ExprAST* getObject() const { return object.get(); }
  const std::string& getMemberName() const { return memberName; }
  const ExprAST* getValue() const { return value.get(); }
  std::unique_ptr<ExprAST> clone() const override;
};

// Declare type statement: declare [Alias =] Type<Args>;
// Used to explicitly instantiate generic types and optionally create aliases
class DeclareTypeAST : public ExprAST {
  std::optional<std::string> aliasName;  // Optional alias name
  TypeAnnotation typeAnnotation;         // The type to instantiate
  mutable sun::TypePtr
      resolvedDeclaredType_;  // Type resolved by semantic analysis

 public:
  DeclareTypeAST(TypeAnnotation type,
                 std::optional<std::string> alias = std::nullopt)
      : aliasName(std::move(alias)), typeAnnotation(std::move(type)) {}

  ASTNodeType getType() const override { return ASTNodeType::DECLARE_TYPE; }
  std::string toString() const override {
    if (aliasName) {
      return "declare " + *aliasName + " = " + typeAnnotation.toString();
    }
    return "declare " + typeAnnotation.toString();
  }

  bool hasAlias() const { return aliasName.has_value(); }
  const std::string& getAliasName() const { return *aliasName; }
  const TypeAnnotation& getTypeAnnotation() const { return typeAnnotation; }

  // Resolved declared type (set by semantic analysis)
  void setResolvedDeclaredType(sun::TypePtr type) const {
    resolvedDeclaredType_ = std::move(type);
  }
  sun::TypePtr getResolvedDeclaredType() const { return resolvedDeclaredType_; }
  bool hasResolvedDeclaredType() const {
    return resolvedDeclaredType_ != nullptr;
  }

  std::unique_ptr<ExprAST> clone() const override;
};
