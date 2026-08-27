# Sun Language Compiler

Sun is a compiled language with an LLVM 20 backend (C++20). Sun ensures memory-safety without runtime overhead via Rust-style borrow checking.

## Pipeline

```
Lexer → Parser → AST → SemanticAnalyzer → BorrowChecker → CodegenVisitor → LLVM IR
```

`include/` and `src/` share one layout, grouped by stage: `support/`, `driver/`, `parsing/`, `ast/`, `semantic_analysis/`, `borrow_checker/`, `codegen/` (with `abi/` and `intrinsics/`), `serialization/`, `moon_bundling/`, `lsp/`, `debug/`. Includes are spelled relative to `include/` (`#include "parsing/parser.h"`). Sources are listed explicitly in `CMakeLists.txt` (`add_library(sun_lib …)`), so a new `.cpp` must be added there.

## Build & Test

```bash
cmake -B build && cmake --build build -j$(nproc)
cd build && ctest -j8 --output-on-failure
./build/tests/sun_tests --gtest_filter="MemorySafety*"          # a whole group
./build/tests/sun_tests --gtest_filter="Stdlib_Collections_*"   # a subgroup
./build/sun input.sun          # JIT execute
./build/sun -c -o output input.sun  # AOT compile
```

## Type System

- **Primitives**: i8–i64, u8–u64, f32, f64, bool, char — passed/returned by value.
- **char**: one Unicode scalar value (`0..=0x10FFFF`, no surrogates), 4 bytes, LLVM `i32`. Primitive but deliberately *not* `isNumeric()`/`isIntegral()` (`types.h`), so every arithmetic and widening path rejects it; `SemanticAnalyzer::checkCharOperands` restricts it to comparisons against another `char`. Two literal forms: `'a'` is a `char`, `b'a'` is a `u8` for byte-oriented code. Both lex to one `CharLiteralAST` (an `isByte` flag) rather than a `NumberExprAST`, because neither takes its type from context. `_convert` moves between `char` and the integer types; `sun.char_of` is the checked form.
- **Classes**: Value types, stack-allocated. Pass by `ref` to borrow; passing by value **moves**. Returned by value (moves to caller).
- **Payload enums**: Tagged unions `{ i32 tag, [N x unit] }`; same ownership rules as classes. A `ref T` payload stores the referent's address and owns nothing — that is how a peek returns `Option<ref T>`.
- **Arrays**: Fat pointer `{ ptr data, i32 ndims, ptr dims }`.
- **Pointers**: `raw_ptr<T>` (bare pointer), `static_ptr<T>` (`{ ptr, i64 }` for literals; read with `.length()` / `.raw()`, builtin methods lowered in `codegenBuiltinTypeMethod`).
- **Error unions**: functions declared with `, IError` suffix; implemented with native LLVM exceptions (a throwing function returns plain `T` and may unwind).
- **Constness**: `const x = …` declares a binding that is never assigned, never mutably borrowed, never taken apart (no field moved out), and only has `const function` methods called on it; a const local may still be moved as a whole. `const ref T` is the read-only borrow (`const ref r = x;`, `v: const ref Vec<T>`); `ref T` converts to `const ref T`, never back. `const function` (in the `public` slot, never on `init`) makes `this` immutable in the body; its result seen through a constant receiver is the *const view* of the declared type (`SemanticAnalyzer::createConstView`, `type_conversion.cpp`: every `ref T` becomes `const ref T`, `Option<ref T>` becomes `Option<const ref T>`), and its body is checked against that same view — so one `first() Option<ref T>` is writable through a `var` and read-only through a `const`. Both views share a layout and cross through memory, so codegen is unaware of them. An interface's const member must be implemented by a const method. `const` always leads — there is no `ref const`. Enforced in sema by one predicate, `SemanticAnalyzer::immutableBaseOf` (`analysis_utils.cpp`), applied at every write, borrow, `ref` argument, receiver and move site; the borrow checker only records `const ref` as a Shared loan. A by-value compound parameter still *moves* its argument, so read-only parameters are spelled `const ref T`. Known gap (roadmap): `String.c_str()` is not const, so path-taking `sun.io`/`sun.env`/`sun.process` functions take `ref String`.
- **References**: `ref T` is a real type — a parameter, a variable, or an enum payload can hold one — but reading a reference reads *through* it, so the expression `c.get(i)` has type `T`. Only the contexts that want the address (binding a ref variable, a `ref` argument, returning a ref, assigning through one) take it, via `tryCodegenAddress`; everything else goes through `codegen()`, which loads (`loadIfRef`). Compound referents are carried as addresses everywhere and are never loaded out — that would be the implicit copy borrowing exists to avoid.
- **Iteration**: `IIterator<T, Container>`/`IIterable<T, Self>` are stdlib interfaces (`stdlib/iterator.sun`), not builtins; `for … in` calls `iter()` if present, then `next(ref Container) Option<T>` until `None` (`src/codegen/loops.cpp`). Sema requires `next` to take exactly `ref <the iterated type>` and return `Option<loop var type>` (codegen passes the iterable's address, so anything else would be UB). The stdlib containers iterate by borrow — `IIterator<ref T, …>`, `next() Option<ref T>` — and `for (var x: T in c)` accepts that, binding `x` to the element in place. Absence is signalled with `Option<T>` (`first`/`last`/`pop`/`find`), not by throwing.
- **Interface conformance** runs for every class and every generic specialization (`validateInterfaceImplementation`). A method may return a class where the interface declares an interface type it implements (`IIterable.iter()`); that marks the interface *static-only* for the class (`ClassType::markStaticOnlyInterface`): usable statically, but the class is not convertible to the interface value since fat-pointer dispatch would mismatch the ABI.

## Generics

- Templates record their `definitionScope` at registration; `instantiateGeneric{Class,Method,Function,Interface,Enum}` switch `currentScope` to it before analyzing the body, so bodies resolve names as written at the definition site (never the requester's locals/imports). The specialized type is registered back in the requesting scope only as a lookup fast path.

## Visibility

- Private by default; `public` is the only modifier (there is no `private` keyword). Applies to module-level items (functions, classes, interfaces, enums, globals, extern/declare, nested modules) and class/interface members.
- Privacy is **module-scoped**: a private item is reachable from its declaring module and that module's children. Root-level (module-less) items are reachable everywhere. `deinit` is always callable; `init` follows the normal rules.
- Records carry `Visibility` plus a `QualifiedName` whose `owner()` (`modulePath`) is the declaring module. Enforced in semantic analysis: module items inside the scope lookups (`AccessFilter`, `semantic_scope.h`), members via `accessibleField/accessibleMethod`; one predicate `sun::access::isAccessible` (`include/semantic_analysis/visibility.h`, `include/semantic_analysis/access_checker.h`). Generic bodies are analyzed inside their template's `definitionScope` (`ScopeSwitchGuard`), so the scope stack answers "which module is asking".
- `.moon` bundles carry private items (generic bodies need them) but hide them from importers; every top-level module of a bundle must be `public`.

## Ownership: No Implicit Copies

**Sun NEVER implicitly copies a compound value (class, payload enum, interface).** Every by-value transfer is a move:

- `var b = a;`, `x = a;`, `obj.field = a;`, `f(a)` (by-value param), `return a;`, `Enum.Variant(a)` — all move `a`. The borrow checker rejects later uses of `a` (use-after-move).
- Codegen invalidates the moved-from source (`applyMoveSemantics`: memset classes to zero, poison enum tag to -1) so its own drop is a no-op. **Owning types must treat the all-zero state as "nothing to release"** (null-check pointers in `deinit`, like `Unique<T>`).
- Overwriting a compound variable/field drops the old value first, then moves the new one in.
- A borrow cannot be laundered back into an owner: where a by-value `T` is expected and `T` is compound, a `ref T` is rejected — in an assignment, a by-value argument, a `return`, a field store, or an enum payload (`typeCopiesByRead` in `types.h`, applied by `isAssignableTo` and the copy of that rule in `scope_lookup.cpp` used by overload resolution). Reading `T` out of the borrow would give the copy and the borrowed value the same buffer. Borrow it with `ref`, or copy it explicitly with a `clone()` method. Only values a read can honestly duplicate come out of a borrow: scalars, and arrays (a fat pointer into storage owned elsewhere).
- Reading a compound **field** by value moves it out of its object and leaves the field empty; that is allowed, and the borrow checker tracks it as a partial move (`fieldPath`/`noteFieldMove`, `borrow_checker.cpp`). Until a value is assigned back into it, the field cannot be read or borrowed, and the object cannot be used as a whole (moved, borrowed, or have a method called on it) — reaching its other fields is still fine. Moved field paths (`cfg.line`, `this.keys`) live in `movedVariables_` next to moved variable names, so branches, loops and function exits treat them alike.
- A move inside a loop body that is still standing when the body ends is rejected (`checkLoopBody`): the next iteration would move the same value again. Fine if the value is made inside the body, if a value is assigned back before the body ends, or if the body always returns/throws.
- A lambda capture list entry that says `ref` borrows (`[ref x]` mutably, `[const ref x]` read-only); one that says neither is **owned** by the closure — a compound moves in, a scalar copies. The closure may change what it owns, and the scope that built it drops it (after joining any thread spawned with that lambda). A compound is never captured implicitly.
- Match bindings of compound payloads **borrow** the payload slot in place (by pointer) — they cannot be moved out or passed by value; the discriminant is frozen for the match.
- Drop scheduling is codegen's job (`trackClassAllocation` / `emitCleanupForScope`): at scope exit (incl. blocks, loop iterations, `break`/`continue`), on returns, and on exception unwind (cleanup landing pads). Payload enums with owning payloads get a synthesized `__sun_enum_drop$<Enum>` function.
- Containers own their elements: `Vec`/`Map`/`LinkedList` drop live elements in `deinit`/`clear`/overwrite. Their peek accessors (`get`, `get_unchecked`, `first`, `last`, `c[i]`, `find`, iteration) hand back a **borrow**, since the container keeps ownership; use `take()`/`pop()`/`remove()` to move an element out. Handing back a bitwise copy instead gives the copy and the stored element the same buffer, and both release it (issue #69).
- Do not add a code path that loads a compound struct out of one location and stores it into another without moving (invalidating) the source.

## Codegen Conventions

`CodegenVisitor` walks the AST and dispatches; nine components do the emitting, each with its own header under `include/codegen/` and its own state. They share one `CodegenState` (module, type registry, type resolver, DWARF, and the frame being emitted) by reference and reach each other back through the visitor — the same shape as `SemanticAnalyzer` and `SemanticContext`.

| Component | Owns |
| --- | --- |
| `ScopeManager` | the scope stack and every drop it emits |
| `FunctionRegistry` | calling conventions, provenance, name lookup |
| `ClassGenerator` | classes, interfaces, enums, generic instantiation |
| `FunctionGenerator` | functions, lambdas, closures, returns |
| `VariableGenerator` | variables, lvalues, globals |
| `LoopGenerator` | loops and the jumps out of them |
| `ErrorGenerator` | throw, try/catch, calls that may unwind |
| `IntrinsicsGenerator` | compiler intrinsics and libc built-ins |

Rules needing no codegen state are free functions so other passes reach the same answers: `sun::codegen::ops` (`scalar_ops.h`) and `sun::codegen::layout` (`struct_access.h`).

- A component that emits sub-expressions declares both `codegen(const ExprAST&)` and `codegen(const BlockExprAST&)` forwarders. The block overload is not optional: without it a block binds to the `ExprAST` one, which attaches an expression debug location and changes DWARF output. A deleted template overload turns a missing overload into a compile error rather than infinite recursion.
- Access LLVM via `ctx.builder` / `ctx.context`.
- `typeResolver.resolve(type)` for variables; `typeResolver.resolveForReturn(type)` for function returns.
- Functions returning classes return the struct by value; callers materialize on stack for addressability.
- Codegen performs no semantic analysis. At a call boundary sema records one `ArgConversion` per argument (`include/semantic_analysis/argument_conversion.h`, decided by `sun::conversions::classifyArgument`) on the `CallExprAST`/`GenericCallAST`; every call path in codegen lowers its arguments through `emitCallArguments`, which only switches on those tags. Add a new argument conversion by extending the enum, the classifier, and that one switch — never by comparing Sun types in codegen.
- New expression types: add `ASTNodeType` enum → AST class → `codegen` method in `src/codegen/*.cpp` → update `codegenExpression()` switch.

## Error Handling

```sun
class DivByZero implements IError {
    function init() {}
    function code() i32 { return 1; }
    function message() String { return String("division by zero"); }
}
function divide(a: i32, b: i32) i32, IError {
    if (b == 0) { throw DivByZero(); }
    return a / b;
}
try { var r = divide(10, x); } catch (e: IError) { return -1; }
```

`throw` takes a class implementing `IError` (never a bare int). `try` is block-form only.
With the stdlib loaded, `IError.message()` returns an owned `String` clone (the builtin
registration starts as `static_ptr<u8>` and is retargeted once `sun.String` is registered,
so stdlib-less programs stay literal-only).

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

- Errors: `logError()` / `logAndThrowError()` for compilation errors.
- Lambdas use closure structs; named functions use direct calls. Class methods use the closure ABI: arg 0 is a ptr to `{ ptr func, ptr env }` with the receiver in `env`; `obj.method` in value position is a lambda-typed bound method `{ methodFn, objPtr }`.
- Run call commands from the workspace root.
- Do not use `git` commands except for `git diff`.
- Run all commands from the workspace folder.
- Create any temp files in ${workspaceRoot}/tmp
- Keep code comments concise and minimal.
- Sun minimizes and discourages alternative syntaxes that do the same thing.
- Use plain english with minimal jargon.
- Sun DOES NOT ALLOW IMPLICIT COPIES.
- Every function, class, interface, and method should have a block comment describing its function and purpose in concise plain-english. This includes sun files and c++ files.

# Build

- Always use `./build.sh` to build the project. Do not run cmake commands directly.
- Use at most 8 cores when compiling.

# Docs

* Use concise plain-english in the docs and code comments.
* Docs and code comments should target a public audience of open-source software engineers.
* Avoid acronyms and jargon except when they are widely known to the target audience.
