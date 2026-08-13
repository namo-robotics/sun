# Sun Language Roadmap

This document tracks the features Sun still needs for real-world adoption, ordered
by how much each one unblocks.

The guiding principle: prioritise the work that turns "wait for the compiler team"
into "a user can do it themselves". Table stakes before novelty.

---

## 🔴 P0 — Adoption Blockers

### C FFI

Sun currently reaches the outside world through compiler intrinsics
(`src/codegen/intrinsics/`). Every new OS or C-library capability requires editing
the compiler and shipping a release. FFI converts most of the rest of this roadmap
into work users can do without us.

- [x] `extern function` declarations calling C symbols — primitives and
      `raw_ptr<T>` only; other signature types are rejected with an error
      rather than miscompiled
- [x] Pass string literals to C (`static_ptr<T>` → `raw_ptr<T>` narrows to the
      data pointer at the call boundary)
- [x] Varargs (`...`) with C default argument promotions — `printf` works.
      Extern declarations only; Sun has no `va_arg`
- [x] `extern "C"` ABI string and `as "symbol"` renaming (`as` is contextual,
      not a reserved word)
- [x] Link external C libraries — `-l<name>` / `-L<dir>` through the driver,
      passed to the linker when compiling and `dlopen`ed when JITing
- [x] Load libraries under JIT (`DynamicLibrary::LoadLibraryPermanently`,
      including `lib<name>.so.N` discovery inside `-L` directories)
- [ ] C struct interop and layout compatibility
- [ ] Calling conventions — SysV classification (byval/sret) for aggregates
- [ ] Extern declarations in `.moon` libraries (serializer guards on `hasBody`)
- [ ] Safety story for unsafe interop — see *Design Decisions Needed*

### Cross-Compilation

The driver is host-only; there is no target-triple handling anywhere. An embedded
and robotics language that cannot target ARM from an x86 dev machine is unusable
for its stated audience.

- [ ] `--target <triple>` in the driver
- [ ] Sysroot / linker configuration per target
- [ ] Cross-compiled stdlib `.moon` artifacts
- [ ] Static binary linking

### Debug Info (DWARF)

There is no `DIBuilder` usage in the tree, so no debugger can step Sun code. Nobody
ships safety-critical code they cannot attach a debugger to. Comparatively cheap
against an existing LLVM 20 backend and buys disproportionate credibility.

- [ ] Line tables and `-g` in the driver
- [ ] Variable and parameter locations
- [ ] Struct/class type descriptions
- [ ] Verified `gdb` / `lldb` stepping over the examples

### Generics Across Module Boundaries

Generic instantiations do not currently cross `.moon` boundaries transparently —
`stdlib/contiguous_buffer.sun` has to pre-enumerate every specialization with
`declare ContiguousBufferIterator_u8 = ContiguousBufferIterator<u8>;`. Library
authors cannot anticipate their users' type arguments, so generic libraries cannot
be written generically. This matters more than any new generics feature.

- [ ] Instantiate generics on demand at the use site across modules
- [ ] Serialize generic definitions (not just specializations) into `.moon`
- [ ] Deduplicate identical instantiations at link time
- [ ] Remove the explicit `declare` specializations from the stdlib

### Match Exhaustiveness Checking

Enums and `match` exist; without exhaustiveness they are convenience rather than
safety, which undercuts the central pitch of the language.

- [ ] Exhaustiveness checking over enum patterns
- [ ] Useful diagnostics naming the uncovered cases
- [ ] Reachability / duplicate-arm warnings

---

## 🟡 P1 — Core Language Gaps

### Optionals

Without a first-class optional, every "might not be there" API invents its own
convention and none of them compose.

- [ ] `Option<T>` (or nullable type syntax — see *Design Decisions Needed*)
- [ ] Niche-optimised representation where possible
- [ ] Pattern matching integration
- [ ] Migrate stdlib APIs that currently signal absence via errors or sentinels

### Tuples

- [ ] Tuple types: `(i32, string)`
- [ ] Tuple construction: `(1, "hello")`
- [ ] Destructuring: `var (x, y) = pair;`
- [ ] Multiple return values without a named class

### Constants and Compile-Time Evaluation

Routine requirements in embedded and safety-critical work, not niceties.

- [ ] Constant variables and references
- [ ] Compile-time evaluation of constant expressions
- [ ] Const generics: `array<T, const N>`
- [ ] Fixed-size array types

### Iteration Ergonomics

`for (var x: Type in iterable)` works but requires an explicit type annotation and
there are no adapters. Verbose iteration is the first thing people mention when
they bounce off a language.

- [ ] Infer the element type in `for ... in`
- [ ] Iterator adapters (`map`, `filter`, `take`, `zip`, `enumerate`)
- [ ] Ranges as iterables

---

## 🟢 P2 — Tooling

### Language Server

The LSP serves diagnostics, formatting and semantic tokens today, plus a VS Code
extension in `extensions/vscode-sun`. The navigation features are the gap.

- [x] Diagnostics / errors
- [x] Document formatting (`sun fmt`, `textDocument/formatting`)
- [x] Semantic tokens
- [x] VS Code extension
- [ ] Go-to-definition
- [ ] Find references
- [ ] Hover type info
- [ ] Autocomplete
- [ ] Rename symbol

### Testing

There is no `sun test`; the suites under `tests/` are GoogleTest in C++. Users
cannot write tests in Sun, which suppresses exactly the library-writing that an
ecosystem is made of.

- [ ] `sun test` runner
- [ ] Test declarations in `.sun` source
- [ ] Assertion builtins with useful failure output

### Other Tooling

- [x] Code formatter (`sun fmt`, also LSP `textDocument/formatting`)
- [ ] Linter (`sunlint`)
- [ ] Source maps for error traces
- [ ] Re-enable the 11 `DISABLED_` tests in `tests/test_lexer.cpp`,
      `tests/test_matrix.cpp`, `tests/test_stdlib_linked_list.cpp`

---

## 🔵 P3 — Standard Library and Reach

### Data Structures

`Vec<T>`, `Map<K, V>`, `LinkedList<T>`, `ContiguousBuffer<T>`, `String`, `Matrix<T>`
and `Unique<T>` exist today.

- [x] `Vec<T>` growable array with iteration and slicing
- [x] `Map<K, V>`
- [x] `LinkedList<T>`
- [ ] `Set<T>`
- [ ] `Queue<T>`, `Stack<T>`
- [ ] `OrderedMap<K, V>`

### Library Expansion

Roughly 3,400 lines of stdlib today. Every item below is something a first-day user
hits. Most become community-supplyable once FFI lands.

- [x] Networking (TCP, HTTP server)
- [x] Threading primitives (`Mutex`)
- [ ] File I/O — round out `stdlib/io.sun`
- [ ] JSON parsing / serialization
- [ ] Date and time
- [ ] Regex (expose the existing parser)
- [ ] Process, environment and filesystem access

### Targets

- [ ] WebAssembly — also the cheapest route to an online playground, which is the
      most effective adoption funnel a new language has

### Error Handling Improvements

- [ ] Stack traces on errors
- [ ] `panic` / `assert` builtins
- [ ] Error context and chaining

### Language Odds and Ends

- [ ] Interface inheritance (`src/semantic_analysis/scope_variables.cpp:746`)
- [ ] Explicit enum values: `Red = 1` (`src/parser.cpp:3367`)

---

## Code TODOs

Found in source:

| File | Line | Note |
|------|------|------|
| `src/borrow_checker/borrow_checker.cpp` | 299 | More sophisticated tracking for ref params |
| `src/semantic_analysis/scope_variables.cpp` | 746 | Interface inheritance |
| `src/parser.cpp` | 3367 | Support explicit enum value assignment |
| `src/codegen/call_expressions.cpp` | 1745 | Implement proper i64 helper |
| `src/codegen/call_expressions.cpp` | 1766 | Implement proper float printing |

---

## Design Decisions Needed

1. **FFI safety**: how does unsafe C interop coexist with a borrow-checked language —
   an `unsafe` block, a trusted-wrapper convention, or something else?
2. **Optional representation**: a stdlib `Option<T>` enum, or nullable type syntax
   built into the type system?
3. **Generic instantiation**: where do cross-module instantiations get emitted, and
   who owns deduplication — the compiler or the linker?
4. **Enum representation**: tagged union vs inheritance hierarchy?
5. **Memory model**: should `Vec<T>` own its allocator or take a reference?

---

*Last updated: August 2026*
