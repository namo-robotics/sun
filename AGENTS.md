# Sun Language Compiler

Sun is a compiled language with an LLVM 20 backend (C++20). Sun ensures memory-safety without runtime overhead via Rust-style borrow checking.

## Pipeline

```
Lexer → Parser → AST → SemanticAnalyzer → BorrowChecker → CodegenVisitor → LLVM IR
```

## Build & Test

```bash
cmake -B build && cmake --build build -j$(nproc)
cd build && ctest -j8 --output-on-failure
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

- **Primitives**: i8–i64, u8–u64, f32, f64, bool — passed/returned by value.
- **Classes**: Value types, stack-allocated. Pass by `ref` to borrow; passing by value **moves**. Returned by value (moves to caller).
- **Payload enums**: Tagged unions `{ i32 tag, [N x unit] }`; same ownership rules as classes.
- **Arrays**: Fat pointer `{ ptr data, i32 ndims, ptr dims }`.
- **Pointers**: `raw_ptr<T>` (bare pointer), `static_ptr<T>` (`{ ptr, i64 }` for literals).
- **Error unions**: functions declared with `, IError` suffix; implemented with native LLVM exceptions (a throwing function returns plain `T` and may unwind).

## Ownership: No Implicit Copies

**Sun NEVER implicitly copies a compound value (class, payload enum, interface).** Every by-value transfer is a move:

- `var b = a;`, `x = a;`, `obj.field = a;`, `f(a)` (by-value param), `return a;`, `Enum.Variant(a)` — all move `a`. The borrow checker rejects later uses of `a` (use-after-move).
- Codegen invalidates the moved-from source (`applyMoveSemantics`: memset classes to zero, poison enum tag to -1) so its own drop is a no-op. **Owning types must treat the all-zero state as "nothing to release"** (null-check pointers in `deinit`, like `Unique<T>`).
- Overwriting a compound variable/field drops the old value first, then moves the new one in.
- Match bindings of compound payloads **borrow** the payload slot in place (by pointer) — they cannot be moved out or passed by value; the discriminant is frozen for the match.
- Drop scheduling is codegen's job (`trackClassAllocation` / `emitCleanupForScope`): at scope exit (incl. blocks, loop iterations, `break`/`continue`), on returns, and on exception unwind (cleanup landing pads). Payload enums with owning payloads get a synthesized `__sun_enum_drop$<Enum>` function.
- Containers own their elements: `Vec`/`Map`/`LinkedList` drop live elements in `deinit`/`clear`/overwrite; use `take()`/`pop()`/`remove()` to move an element out, `get()` only for non-owning element types.
- Do not add a code path that loads a compound struct out of one location and stores it into another without moving (invalidating) the source.

## Codegen Conventions

- Access LLVM via `ctx.builder` / `ctx.context`.
- `typeResolver.resolve(type)` for variables; `typeResolver.resolveForReturn(type)` for function returns.
- Functions returning classes return the struct by value; callers materialize on stack for addressability.
- New expression types: add `ASTNodeType` enum → AST class → `codegen` method in `src/codegen/*.cpp` → update `codegenExpression()` switch.

## Error Handling

```sun
class DivByZero implements IError {
    function init() {}
    function code() i32 { return 1; }
    function message() static_ptr<u8> { return "division by zero"; }
}
function divide(a: i32, b: i32) i32, IError {
    if (b == 0) { throw DivByZero(); }
    return a / b;
}
try { var r = divide(10, x); } catch (e: IError) { return -1; }
```

`throw` takes a class implementing `IError` (never a bare int). `try` is block-form only.

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
- Lambdas use closure structs; named functions use direct calls. Class methods use the closure ABI: arg 0 is a ptr to `{ ptr func, ptr env }` with the receiver in `env`; `obj.method` in value position is a lambda-typed bound method `{ methodFn, objPtr }`.
- Never `cd` out of workspace root. 
- Absotely NEVER use `git` commands except for `git diff`.
- Run all commands from the workspace folder.
- Create any temp files in ${workspaceRoot}/tmp
- Keep code comments concise and minimal

# Build

- Always use `./build.sh` to build the project. Do not run cmake commands directly.
- Use at most 8 cores when compiling.
