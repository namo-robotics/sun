# Semantic Analysis in Sun

This document explains how semantic analysis works in the Sun compiler.

## Overview

Semantic analysis is the phase between parsing and code generation. It:

1. **Declares** variables, functions, classes, and types in scope
2. **Validates** correctness (type mismatches, undefined references, etc.)
3. **Resolves** types for every AST node (stored in `resolvedType`)

The output is a fully-typed AST that codegen can consume.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    SemanticAnalyzer                         │
├─────────────────────────────────────────────────────────────┤
│  analyzeExpr()     - Main entry point, traverses AST        │
│  inferType()       - Computes expression types              │
│  lookupVariable()  - Scope-aware variable lookup            │
│  narrowVariable()  - Type narrowing from guards             │
├─────────────────────────────────────────────────────────────┤
│  scopeStack        - Stack of lexical scopes                │
│  typeRegistry      - Shared type definitions                │
│  functionTable     - Registered functions                   │
│  currentClass      - Context for 'this' resolution          │
└─────────────────────────────────────────────────────────────┘
```

## Analysis vs Type Inference

**Semantic Analysis** (`analyzeExpr`) traverses the AST and:
- Recursively analyzes children
- Declares variables in scope
- Validates semantic rules
- Calls `inferType` to compute types
- Stores the result via `setResolvedType()`

**Type Inference** (`inferType`) computes what type an expression has:
- Reads from current scope context
- Returns a `sun::TypePtr`
- Does NOT modify scope or declare variables

```cpp
void SemanticAnalyzer::analyzeExpr(ExprAST& expr) {
    switch (expr.getType()) {
        case VARIABLE_CREATION: {
            // 1. Analyze value expression first
            analyzeExpr(*varCreate.getValue());
            
            // 2. Determine type (explicit or inferred)
            sun::TypePtr type = varCreate.hasTypeAnnotation()
                ? typeAnnotationToType(*varCreate.getTypeAnnotation())
                : inferType(*varCreate.getValue());
            
            // 3. Declare in scope (side effect!)
            declareVariable(varCreate.getName(), type);
            
            // 4. Set resolved type
            expr.setResolvedType(type);
            break;
        }
        
        case VARIABLE_REFERENCE: {
            // Just compute type - no side effects
            expr.setResolvedType(inferType(expr));
            break;
        }
        // ...
    }
}
```

## Scope Management

Scopes form a stack. Each scope contains:
- `variables` - Map of variable name → `VariableInfo`
- `narrowedTypes` - Type narrowing from `_is<T>` guards
- `typeAliases` - Local type aliases

```cpp
struct Scope {
    std::map<std::string, VariableInfo> variables;
    std::map<std::string, sun::TypePtr> narrowedTypes;
    std::map<std::string, sun::TypePtr> typeAliases;
};
```

### Scope Lifetime

```cpp
// Function body
enterScope();
declareVariable("param", paramType, /*isParam=*/true);
analyzeBlock(body);
exitScope();

// If statement with type guard
if (_is<Point>(x)) {
    // New scope with x narrowed to Point
    enterScope();
    narrowVariable("x", PointType);
    analyzeExpr(thenBranch);
    exitScope();
}
```

## Type Narrowing

When analyzing `if (_is<T>(x)) { ... }`, the analyzer:

1. Detects the `_is<T>(x)` pattern in the condition
2. Enters a new scope for the then-branch
3. Calls `narrowVariable("x", T)` to record the narrowing
4. Subsequent references to `x` in that scope see type `T`

```cpp
// In analysis.cpp - IF case
auto typeGuard = extractTypeGuard(*ifExpr.getCond(), *this);
if (typeGuard) {
    enterScope();
    narrowVariable(typeGuard->first, typeGuard->second);
    analyzeExpr(*ifExpr.getThen());
    exitScope();
}
```

Type narrowing is validated in `getNarrowedType()`:
- TypeParameter → concrete type ✓
- Interface → implementing class ✓
- Concrete → more concrete ✗ (not valid narrowing)

## Early Return Optimization

Once `resolvedType` is set on an AST node, `inferType` can return it immediately:

```cpp
sun::TypePtr SemanticAnalyzer::inferType(const ExprAST& expr) {
    if (sun::TypePtr resolved = expr.getResolvedType()) {
        if (expr.getType() != ASTNodeType::ARRAY_LITERAL) {
            return resolved;  // Use cached type
        }
    }
    // ... compute type
}
```

This works because:
- Each AST node is unique
- Each node is analyzed exactly once in its proper scope context
- The cached type preserves scope-aware information (like narrowing)

**Exception**: Array literals need the full inference path to compute dimensions from elements, even when a type hint is set.

## Function Analysis

Functions are analyzed in two passes:

### Pass 1: Registration
Register function signatures before analyzing bodies (enables recursion):

```cpp
FunctionInfo funcInfo = getFunctionInfo(func);
registerFunction(proto.getName(), funcInfo);
```

### Pass 2: Body Analysis
```cpp
enterScope();
// Declare parameters
for (const auto& [argName, argType] : proto.getArgs()) {
    declareVariable(argName, typeAnnotationToType(argType), /*isParam=*/true);
}
// Analyze body
analyzeBlock(func.getBody());
exitScope();
```

## Class Analysis

Classes have a more complex flow:

1. **Create class type** in TypeRegistry
2. **Add fields** to class type
3. **Register class** (so methods can reference it)
4. **Set `currentClass`** (for `this` resolution)
5. **Pass 1**: Register all method signatures
6. **Pass 2**: Analyze all method bodies
7. **Validate interface implementations**

```cpp
auto classType = typeRegistry->getClass(classDef.getName());
// Add fields...
registerClass(classDef.getName(), classType);

auto savedClass = currentClass;
setCurrentClass(classType);

// PASS 1: Register methods
for (const auto& methodDecl : classDef.getMethods()) {
    // Register signature...
}

// PASS 2: Analyze bodies
for (const auto& methodDecl : classDef.getMethods()) {
    enterScope();
    declareVariable("this", classType, /*isParam=*/true);
    analyzeFunction(*methodDecl.function);
    exitScope();
}

setCurrentClass(savedClass);
```

## Generic Instantiation

For generic classes/functions, AST nodes are **shared** across specializations. Before re-analyzing for a new specialization:

```cpp
// Clear stale types from previous specialization
clearResolvedTypes(methodBody);

// Set up type parameter bindings for this specialization
enterScope();
addTypeParameterBindings(typeParams, typeArgs);  // T → i32, etc.

// Analyze with fresh context
analyzeBlock(methodBody);

exitScope();
```

## Key Data Structures

### VariableInfo
```cpp
struct VariableInfo {
    sun::TypePtr type;
    bool isParameter;
    bool isMutable;
};
```

### FunctionInfo
```cpp
struct FunctionInfo {
    sun::TypePtr returnType;
    std::vector<sun::TypePtr> paramTypes;
    std::vector<Capture> captures;  // For closures
};
```

### TypeRegistry
Shared between semantic analyzer and codegen:
- `getClass(name)` - Get or create ClassType
- `getInterface(name)` - Get or create InterfaceType
- `getEnum(name)` - Get or create EnumType

## File Organization

```
src/semantic_analysis/
├── analysis.cpp           # Main analyzeExpr() dispatch
├── type_inference.cpp     # inferType() implementation
├── scope_variables.cpp    # Variable/scope management
├── type_conversion.cpp    # TypeAnnotation → TypePtr
├── classes.cpp            # Class/interface analysis
└── generics.cpp           # Generic instantiation
```

## Error Handling

Semantic errors use `logAndThrowError()`:
```cpp
if (!varInfo) {
    logAndThrowError("Unknown variable: '" + name + "'");
}
```

This logs the error with source location and throws `SemanticException`.
