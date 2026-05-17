# Sun Language Compiler

Sun is a compiled language with an LLVM 20 backend (C++20).

## Pipeline

```
Lexer → Parser → AST → SemanticAnalyzer → BorrowChecker → CodegenVisitor → LLVM IR
```

## Build & Test

```bash
cmake -B build && cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
./build/tests/sun_tests --gtest_filter="SuiteName.*"
./build/sun input.sun          # JIT execute
./build/sun -c -o output input.sun  # AOT compile
```

## Project Layout

```
include/          # Headers (ast.h, types.h, codegen_visitor.h, ...)
src/codegen/      # IR generation split by expression type
src/semantic_analysis/  # Type inference, classes, interfaces, captures
src/borrow_checker/     # Ownership tracking
src/lsp/          # Language server
stdlib/           # Standard library (.sun files)
tests/            # GoogleTest suites (test_*.cpp) + programs/
```

## Type System

- **Primitives**: i8–i64, u8–u32, f32, f64, bool — passed/returned by value.
- **Classes**: Value types, stack-allocated. **Must be passed by `ref`**, returned by value (struct copy).
- **Arrays**: Fat pointer `{ ptr data, i32 ndims, ptr dims }`.
- **Pointers**: `raw_ptr<T>` (bare pointer), `static_ptr<T>` (`{ ptr, i64 }` for literals).
- **Error unions**: `{ i1 isError, T value }` — functions declared with `, IError` suffix.

## Codegen Conventions

- Access LLVM via `ctx.builder` / `ctx.context`.
- `typeResolver.resolve(type)` for variables; `typeResolver.resolveForReturn(type)` for function returns.
- Functions returning classes return the struct by value; callers materialize on stack for addressability.
- New expression types: add `ASTNodeType` enum → AST class → `codegen` method in `src/codegen/*.cpp` → update `codegenExpression()` switch.

## Error Handling

```sun
function divide(a: i32, b: i32) i32, IError {
    if (b == 0) { throw 1; }
    return a / b;
}
try { var r = divide(10, x); } catch (e: IError) { return -1; }
```

## Memory Allocation

- All allocations go through a `HeapAllocator` (never `_malloc` directly).
- Pattern: accept `ref HeapAllocator` in `init`, store via `allocator.copy()`, allocate with `this.alloc.alloc_raw(size)`.
- `_free()` is still used directly for deallocation.

## Testing Patterns

```cpp
// JIT (slow, ~500ms setup)
auto value = executeString(R"( function main() i32 { return 42; } )");
EXPECT_EQ(value, 42);

// AOT (fast, no execution)
EXPECT_NO_THROW(compileFile("tests/programs/example.sun"));
```

Module tests require `SUN_PATH` env var pointing to workspace root.

## Conventions

- Namespace: `sun::` for type system classes.
- Errors: `logError()` / `logAndThrowError()` for compilation errors.
- Lambdas use closure structs; named functions use direct calls.
- Never `cd` out of workspace root. Absotely no `git` commands except for `git diff`. Use relative paths.
