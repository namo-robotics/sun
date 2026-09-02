# Sun Language Compiler

Sun is a compiled language with an LLVM 20 backend (C++20). Sun ensures memory-safety without runtime overhead via Rust-style borrow checking.

```
Lexer → Parser → AST → SemanticAnalyzer → BorrowChecker → CodegenVisitor → LLVM IR
```

`include/` and `src/` share one layout, grouped by stage: `support/`, `driver/`, `parsing/`, `ast/`, `semantic_analysis/`, `borrow_checker/`, `codegen/`, `serialization/`, `moon_bundling/`, `lsp/`, `debug/`. Inside `codegen/` there is a folder per component — `scopes/`, `classes/`, `functions/`, `variables/`, `expressions/`, `loops/`, `errors/`, `support/`, `abi/`, `intrinsics/` — each holding both its header and the sources that implement it. A header and its sources always sit in the same relative directory under `include/` and `src/`. Includes are spelled relative to `include/` (`#include "codegen/scopes/scope_manager.h"`). Sources are listed explicitly in `CMakeLists.txt` (`add_library(sun_lib …)`), so a new `.cpp` must be added there.

The language itself is documented in `docs/pages/` — read the page for the area you are changing rather than rediscovering the rules. `memory-safety.mdx`, `classes.mdx`, `generics.mdx`, `modules.mdx` (visibility), `builtin-types.mdx`, `errors.mdx`, `interfaces.mdx`, `enums.mdx`, `match.mdx`, `threads.mdx`, `c-ffi.mdx`, `intrinsics.mdx`, `stdlib.mdx`. Compiler internals live in `docs/pages/architecture/`. Keep those pages current when you change what they describe.

## Build & Test

```bash
./build.sh                    # always build this way; never run cmake directly
cd build && ctest -j8 --output-on-failure
./build/tests/sun_tests --gtest_filter="MemorySafety*"          # a whole group
./build/tests/sun_tests --gtest_filter="Functions_Generic_*"    # a subgroup
./build/stdlib_tests_test --test-sequential                     # the stdlib's Sun tests
./build/tls_tests_test --test-sequential                        # the tls bundle's Sun tests
./build/sun input.sun               # JIT execute
./build/sun -c -o output input.sun  # AOT compile
```

Use at most 8 cores when compiling. Module tests require `SUN_PATH` pointing at the workspace root.

```cpp
// JIT (slow, ~500ms setup)
auto value = executeString(R"( function main() i32 { return 42; } )");
EXPECT_EQ(value, 42);

// AOT (fast, no execution)
EXPECT_NO_THROW(compileFile("tests/programs/example.sun"));
```

## Ownership: No Implicit Copies

**Sun NEVER implicitly copies a compound value (class, payload enum, interface).** Every by-value transfer is a move. `docs/pages/memory-safety.mdx` has the rules; these are the invariants the compiler must not break:

- Codegen invalidates the moved-from source (`applyMoveSemantics`: memset classes to zero, poison enum tag to -1) so its own drop is a no-op. **Owning types must treat the all-zero state as "nothing to release"** — null-check pointers in `deinit`, like `Unique<T>`.
- A borrow cannot be laundered back into an owner: where a by-value `T` is expected and `T` is compound, a `ref T` is rejected (`typeCopiesByRead` in `types.h`, applied by `isAssignableTo` and by the copy of that rule in `scope_lookup.cpp` used for overload resolution). Reading `T` out of the borrow would give the copy and the borrowed value the same buffer.
- Containers own their elements. Their peek accessors (`get`, `first`, `last`, `c[i]`, `find`, iteration) hand back a **borrow**; `take()`/`pop()`/`remove()` move an element out. Handing back a bitwise copy gives the copy and the stored element the same buffer, and both release it (issue #69).
- Do not add a code path that loads a compound struct out of one location and stores it into another without moving (invalidating) the source.

## Where the Rules Are Enforced

One predicate per rule, so there is one place to change and one place to look:

- Constness: `SemanticAnalyzer::immutableBaseOf` (`analysis_utils.cpp`), applied at every write, borrow, `ref` argument, receiver and move site. `const` always leads — there is no `ref const`.
- Visibility: `sun::access::isAccessible` (`semantic_analysis/visibility.h`, `access_checker.h`). Privacy is module-scoped, and `.moon` bundles carry private items but hide them from importers.
- Argument conversions: `sun::conversions::classifyArgument` decides, codegen only switches on the tag. Add one by extending the enum, the classifier, and `emitCallArguments` — never by comparing Sun types in codegen.
- Generic bodies resolve names at their definition site: templates record `definitionScope`, and `instantiateGeneric*` switches `currentScope` to it before analyzing the body.
- Iteration is stdlib, not builtin (`stdlib/iterator.sun`). Sema requires `next` to take exactly `ref <the iterated type>`; codegen passes the iterable's address, so anything else is UB.
- Constructors run in two phases: a constructor assigns every field before `this` may be read or passed on, and it may not finish with a field unassigned. `checkFieldInitialization` (`field_initialization.cpp`) walks the body carrying one status per field — `Uninitialized`, `Initialized`, `Unknown`. Branches walk each alternative from the state before the choice and merge the ends (agree → stands, disagree → `Unknown`); a loop is walked to a fixpoint so a write in the body sees what a second pass would; a `this.method()` call is followed into the method's body with the state at the call. Errors: reading a field, or `this`, before it is `Initialized`; a constructor write to an **owning** field whose status is `Unknown`; a field not `Initialized` at the end of a path. Fields that own nothing are exempt from the write rules. A method's write always replaces and drops, whoever calls it — before a field's first value the storage is all zero, which an owning `deinit` must treat as nothing to release — so a method may give fields their first values on the constructor's behalf. Lambdas are not walked — making one that touches `this` requires the whole object. Each write is tagged `StartsLife` (drops nothing) or `ReplacesValue` (drops); codegen only switches on the tag, and no field write is decided at run time.
- A value moved on only some paths out of a branch carries a **drop flag**: an `i1` in the stack frame (never a field, so class layout and the C ABI are untouched) saying whether the frame still owns it, set where it becomes owned, cleared at the move, read by the drop. `ScopeManager::BranchArm` marks each branch arm; a move nested deeper than where the value became owned is path-dependent. A move on every path or none stays a compile-time decision costing nothing.

## Memory Allocation

- All allocations go through a `HeapAllocator` (never `_malloc` directly).
- Pattern: accept `ref HeapAllocator` in `init`, store via `allocator.copy()`, allocate with `this.alloc.alloc_raw(size)`.
- `_free()` is still used directly for deallocation.

## Conventions

- Errors: `logError()` / `logAndThrowError()` for compilation errors.
- Run all commands from the workspace root. Create any temp files in `${workspaceRoot}/tmp`.
- Do not use `git` commands except for `git diff`.
- Sun minimizes and discourages alternative syntaxes that do the same thing.
- Class and interface methods are declared with `method`; `function` declares free functions only.
- Sun DOES NOT ALLOW IMPLICIT COPIES.
- Every function, class, interface and method gets a block comment saying what it is for, in concise plain English. This includes `.sun` files and C++ files. Keep other comments minimal.
- Use plain English with minimal jargon, in docs and comments alike. Both target a public audience of open-source software engineers; avoid acronyms except widely known ones.
- Keep comments concise, plain-english, and for a general audience of engineers.