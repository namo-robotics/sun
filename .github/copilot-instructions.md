# Sun Language Compiler - AI Agent Instructions

## Architecture Overview

Sun is a compiled language with LLVM 20 backend. The compilation pipeline:
```
Lexer (lexer.h) → Parser (parser.h) → AST (ast.h) → SemanticAnalyzer → CodegenVisitor → LLVM IR
```

Key components:
- **Type System** (`include/types.h`): Primitives, matrices, functions/lambdas, classes, interfaces, generics, error unions
- **CodegenVisitor** (`include/codegen_visitor.h`): Main IR generation, split into files under `src/codegen/` by expression type
- **TypeRegistry**: Shared between semantic analyzer and codegen for class/interface types

## Build Commands

```bash
# Configure and build (from workspace root)
cmake -B build && cmake --build build -j$(nproc)

# Run all tests
cd build && ctest --output-on-failure

# Run specific test suite
./build/tests/sun_tests --gtest_filter="ErrorTest.*"

# Compile a Sun program
./build/sun -c -o output input.sun
./build/sun input.sun  # JIT execute
```

## Testing Patterns

Tests use GoogleTest in `tests/test_*.cpp`. Two main approaches:

```cpp
// JIT execution - returns result (slow, ~500ms per test due to JIT setup)
auto value = executeString(R"(
    function main() i32 { return 42; }
)");
EXPECT_EQ(value, 42);

// AOT compilation - verifies codegen without execution (faster)
EXPECT_NO_THROW(compileFile("tests/programs/example.sun"));
```

Module tests require `SUN_PATH` environment variable pointing to workspace root.

## Codegen Conventions

When adding new expression codegen:
1. Add `ASTNodeType` enum in `ast.h`
2. Create AST class inheriting from `ExprAST`
3. Add `codegen` method to `CodegenVisitor` (in appropriate `src/codegen/*.cpp` file)
4. Update `codegenExpression()` switch in `src/codegen/codegen_visitor.cpp`

Pattern for accessing LLVM builder and context:
```cpp
llvm::Value* CodegenVisitor::codegenSomething(const SomethingAST& ast) {
    auto& builder = ctx.builder;  // IRBuilder
    auto& context = ctx.context;  // LLVMContext
    // ...
}
```

## Error Handling (try/catch/throw)

Functions that can throw errors are declared with `, IError` suffix:
```sun
function divide(a: i32, b: i32) i32, IError {
    if (b == 0) { throw 1; }
    return a / b;
}
```

Error union struct: `{ i1 isError, T value }`. Division in error-returning functions automatically checks for zero.

Callers use `try/catch`:
```sun
try {
    var result = divide(10, x);
} catch (e: IError) {
    return -1;
}
```

## Key Files

- `include/codegen_visitor.h`: CodegenVisitor class with all codegen state
- `include/execution_utils.h`: Test helpers (`executeString`, `compileFile`)
- `src/codegen/error_handling.cpp`: throw and try/catch codegen
- `src/codegen/functions_lambdas.cpp`: Function/lambda codegen with closure support
- `include/types.h`: Full type system with `TypeRegistry`

## Type System & Memory Model

### Value vs Reference Types

**Primitives** (i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, bool): Passed/returned by value.

**Classes**: Value types, stack-allocated by default.
```sun
var point = Point(3, 4);  // Stack alloca, 'point' has type Point (value type)
point.x = 5;              // Field access
return point;             // Returns struct BY VALUE (copy out)
```
In LLVM IR, `point` is an alloca (pointer to stack memory), but in Sun semantics it's a value type.

**Arrays**: Fat pointer struct `{ ptr data, i32 ndims, ptr dims }`. Always this representation.

**Pointers**
- `raw_ptr<T>`: Non-owning raw pointer, manual management (LLVM: just a pointer)
- `static_ptr<T>`: Fat pointer struct `{ ptr, i64 }` for immortal data (string literals)

### Function Return Semantics

Functions returning class types return the **struct by value**, not a pointer:
```llvm
; CORRECT: returns struct by value
define %Point_struct @make_point() {
  %p = alloca %Point_struct
  ; ... initialize ...
  %ret = load %Point_struct, ptr %p
  ret %Point_struct %ret
}

; WRONG: returns dangling stack pointer
define ptr @make_point() {
  %p = alloca %Point_struct
  ret ptr %p  ; BUG: stack memory freed after return!
}
```

When calling such functions, the caller must materialize the returned struct on its stack to make it addressable (for field access, method calls, etc.):
```cpp
// In call_expressions.cpp - after calling function returning struct
// Must exclude special struct types that have their own handling:
// - Error unions { i1 isError, T value } - unwrapped earlier in the function
// - Array fat structs { ptr, i32, ptr } - not addressable class instances
if (callResult->getType()->isStructTy()) {
  auto* structType = cast<StructType>(callResult->getType());
  bool isErrorUnion = structType->getNumElements() == 2 &&
                      structType->getElementType(0)->isIntegerTy(1);
  bool isArrayFat = structType->getNumElements() == 3 &&
                    structType->getElementType(1)->isIntegerTy(32);
  if (!isErrorUnion && !isArrayFat) {
    // Class returned by value - store to caller's stack for addressability
    AllocaInst* resultAlloca = createEntryBlockAlloca(func, "ret.struct", structType);
    builder->CreateStore(callResult, resultAlloca);
    callResult = resultAlloca;  // Now usable for field access / method calls
  }
}
```

### LLVMTypeResolver (LLVM codegen details)

- `typeResolver.resolveForReturn(type)` - Function return types: returns struct type for classes (return by value)
- `typeResolver.resolve(type)` - Variables/allocas: returns `ptr` for classes (alloca holds pointer to struct on stack)

Note: This is LLVM representation. In Sun semantics, `var p = Point()` has type `Point` (value type), not `ptr<Point>`.

## Conventions

- Headers in `include/`, implementations in `src/`
- Use `sun::` namespace for type system classes
- Error handling: Use `logError()` / `logAndThrowError()` for compilation errors
- Lambda captures use closure structs; named functions use direct calls when possible

## Memory Allocation
- ALL memory allocations MUST go through an allocator object (e.g., `HeapAllocator`).
- Never use `_malloc` directly in stdlib or user code. Use `allocator.alloc_raw(size)` instead.
- `_free()` is still used directly for deallocation (no allocator method for free yet).
- Classes that allocate memory (Vec, Map, etc.) must accept a `ref HeapAllocator` parameter in their `init` and store it as a field via `allocator.copy()`.
- Pattern: `this.alloc = allocator.copy();` in init, then `this.alloc.alloc_raw(...)` for allocations.
- `make_heap_allocator()` creates a new HeapAllocator instance.

When using the terminal tool:
- Always run commands from the workspace root.
- Never propose or execute `cd` commands that leave the workspace.
- Never use `git` commands.
- Prefer using full relative paths from the workspace root (e.g., `./src` instead of changing directory).
