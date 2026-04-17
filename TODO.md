# Sun Language Roadmap

This document tracks missing features needed for real-world adoption.

## 🔴 P0 — Critical (Foundational)

### Enums + Pattern Matching
- [ ] Enums with associated data: `enum Option<T> { Some(T), None }`
- [ ] Exhaustiveness checking

### Hash Map (stdlib)
- [ ] `Map<K, V>` generic class
- [ ] Basic operations: `insert`, `get`, `remove`, `contains`
- [ ] Iteration support

---

## 🟡 P1 — High Priority (Adoption Blockers)

### Language Server Protocol (LSP)
- [ ] Go-to-definition
- [ ] Find references
- [ ] Hover type info
- [ ] Diagnostics/errors
- [ ] Autocomplete
- [ ] VS Code extension

### Dynamic Vec (stdlib)
- [ ] `Vec<T>` growable array
- [ ] `push`, `pop`, `insert`, `remove`
- [ ] Iteration
- [ ] Slicing support

### C FFI
- [ ] `extern "C"` function declarations
- [ ] Link external C libraries
- [ ] C struct interop
- [ ] Calling conventions

### Additional Data Structures (stdlib)
- [ ] `Set<T>`
- [ ] `LinkedList<T>`
- [ ] `Queue<T>`, `Stack<T>`
- [ ] `OrderedMap<K, V>`

---

## 🟢 P2 — Important (Modern Language Features)

### Moon Package Manager
- [ ] Dependency resolution
- [ ] Package registry
- [ ] Versioning
- [ ] manifest contained in `.sun` file

### Constants
- [ ] Constant variables and references
- [ ] Compile-time evaluation
- [ ] Const generics: `array<T, const N>`

### Tuples
- [ ] Tuple types: `(i32, string)`
- [ ] Tuple construction: `(1, "hello")`
- [ ] Destructuring: `var (x, y) = pair;`

---

## 🔵 P3 — Nice to Have

### Tooling
- [ ] Code formatter (`sunfmt`)
- [ ] Linter (`sunlint`)
- [ ] Debugger support (DWARF debug info)
- [ ] Source maps for error traces

### Standard Library Expansion
- [ ] File I/O (fix 3 failing tests)
- [ ] Networking (TCP, UDP, HTTP client)
- [ ] JSON parsing/serialization
- [ ] Date/time handling
- [ ] Regex (expose existing parser)
- [ ] Threading primitives

### Targets
- [ ] WebAssembly (WASM) compilation
- [ ] Cross-compilation support
- [ ] Static binary linking

### Error Handling Improvements
- [ ] Stack traces on errors
- [ ] `panic!` / `assert!` builtins
- [ ] Error context/chaining

---

## Code TODOs

Found in source:

| File | Line | Note |
|------|------|------|
| `src/borrow_checker/borrow_checker.cpp` | 225 | More sophisticated tracking for ref params |
| `src/codegen/functions_lambdas.cpp` | 414 | Support global named functions with captures |
| `src/codegen/call_expressions.cpp` | 1038 | Implement proper i64 helper |
| `src/codegen/call_expressions.cpp` | 1059 | Implement proper float printing |

---

## Design Decisions Needed

1. **Memory model**: Should `Vec<T>` own its allocator or take a reference?
2. **Enum representation**: Tagged union vs inheritance hierarchy?
4. **FFI safety**: How to handle unsafe C interop in a safe language?

---

*Last updated: April 2026*
