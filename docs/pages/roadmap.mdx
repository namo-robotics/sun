# Sun Language Roadmap

Features Sun still needs, ordered by priority — highest first. Completed
features are removed from this list rather than ticked off.

The guiding principle: prioritise the work that turns "wait for the compiler team"
into "a user can do it themselves". Table stakes before novelty.

---

## 1. Access Modifiers

Everything is public today; there is no way to hide representation or enforce
invariants at a type or module boundary, so stdlib containers cannot stop users
from reaching into their internals and modules cannot keep helpers private.

- [ ] `private` / `public` on class fields and methods
- [ ] `private` / `public` on interfaces and interface members
- [ ] `private` / `public` on modules themselves (a nested module hidden from
      importers of its parent)
- [ ] `private` / `public` on each item inside a module — functions, classes,
      interfaces, enums, globals, extern declarations, nested modules
- [ ] Private by default everywhere (decided): unannotated items are private;
      only `public` needs to be written. Existing code and the stdlib must be
      annotated `public` where they are meant to be visible
- [ ] Enforce in semantic analysis, with diagnostics naming the item and its
      owning class or module
- [ ] Preserve visibility across `.moon` boundaries; private items are not
      exported from a bundle. Each `.moon`'s top-level module is required to
      be `public` — `--emit-moon` rejects a bundle whose root module is private
- [ ] Reflect in the LSP (hide private items in completions outside their scope)
- [ ] Migrate the stdlib and tests: mark the intended API of `Vec`, `Map`,
      `String`, … `public`; internal fields and module-internal helpers stay
      private by omission

## 2. Optionals — Remaining Work

`Option<T>` / `Result<T, E>` ship in the stdlib (`stdlib/option.sun`) as ordinary
generic payload enums with owning payloads and drop glue, and the starter
migration (`Map.find`, `String.find_char`/`rfind_char`, `Vec.first`/`last`) has
landed. What remains:

- [ ] Migrate the remaining absence APIs (`LinkedList.first/last`, `Vec.pop`,
      iterator `next()` — the last needs the `IIterator` contract rework);
      unblocked now that owning payloads have drop glue
- [ ] Niche-optimised representation where possible (e.g. `Option<raw_ptr<T>>`
      as a nullable pointer)
- [ ] Bare `Some` / `None` prelude names (today it is `Option.Some(x)` /
      `Option.None`)
- [ ] Explicit `Option<i32>.Some(...)` in expression position
- [ ] Nested-generic payload unification
- [ ] Moving a payload *out* of a match binding (needs partial-move tracking;
      today compound bindings borrow the payload slot by pointer)
- [ ] Arrays and globals of payload enums
- [ ] C-ABI classification of payload enums
- [ ] Nested patterns in `match`
- [ ] `DW_TAG_variant_part` debug info for payload enums

## 3. Constants and Compile-Time Evaluation

Routine requirements in embedded and safety-critical work, not niceties.

- [ ] Constant variables and references
- [ ] Compile-time evaluation of constant expressions
- [ ] Const generics: `array<T, const N>`
- [ ] Fixed-size array types

## 4. Iteration Ergonomics

`for (var x: Type in iterable)` works but requires an explicit type annotation and
there are no adapters. Verbose iteration is the first thing people mention when
they bounce off a language.

- [ ] Infer the element type in `for ... in`
- [ ] Iterator adapters (`map`, `filter`, `take`, `zip`, `enumerate`)
- [ ] Ranges as iterables

## 5. Language Server Navigation

The LSP serves diagnostics, formatting and semantic tokens today, plus a VS Code
extension in `extensions/vscode-sun`. The navigation features are the gap.

- [ ] Go-to-definition
- [ ] Find references
- [ ] Hover type info
- [ ] Autocomplete
- [ ] Rename symbol

## 6. Testing

There is no `sun test`; the suites under `tests/` are GoogleTest in C++. Users
cannot write tests in Sun, which suppresses exactly the library-writing that an
ecosystem is made of.

- [ ] `sun test` runner
- [ ] Test declarations in `.sun` source
- [ ] Assertion builtins with useful failure output

## 7. Error Handling Improvements

- [ ] Stack traces on errors
- [ ] `panic` / `assert` builtins
- [ ] Error context and chaining

## 8. Library Expansion

Roughly 3,400 lines of stdlib today (networking and `Mutex` exist). Every item
below is something a first-day user hits. Most are community-supplyable now that
FFI has landed.

- [ ] File I/O — round out `stdlib/io.sun`
- [ ] Process, environment and filesystem access
- [ ] JSON parsing / serialization
- [ ] Date and time
- [ ] Regex (expose the existing parser)

## 9. Data Structures

`Vec<T>`, `Map<K, V>`, `LinkedList<T>`, `ContiguousBuffer<T>`, `String`, `Matrix<T>`
and `Unique<T>` exist today.

- [ ] `Set<T>`
- [ ] `Queue<T>`, `Stack<T>`
- [ ] `OrderedMap<K, V>`

## 10. Language Odds and Ends

- [ ] Interface inheritance (`src/semantic_analysis/scope_variables.cpp:746`)
- [ ] Explicit enum values: `Red = 1` (`src/parser.cpp:3367`)
- [ ] More sophisticated borrow tracking for ref params
      (`src/borrow_checker/borrow_checker.cpp:299`)
- [ ] Proper i64 helper and float printing in
      `src/codegen/call_expressions.cpp:1745` / `:1766`

## 11. Other Tooling

- [ ] Linter (`sunlint`)
- [ ] Source maps for error traces
- [ ] Re-enable the 11 `DISABLED_` tests in `tests/test_lexer.cpp`,
      `tests/test_matrix.cpp`, `tests/test_stdlib_linked_list.cpp`

## 12. Native Protobuf Import — Deferred Items

`protos:` manifest imports synthesize Sun source per message (encode/decode,
`_delimited` framing, `Option<T>` for proto3 `optional`, payload enums for
`oneof`, unknown-field preservation, `.moon` export), byte-identical to
libprotobuf. Docs: `docs/pages/protobuf.mdx`. Deferred:

- [ ] proto2 syntax
- [ ] `group`
- [ ] Extensions
- [ ] Recursive messages by value (need indirection)
- [ ] `--dump-proto-sun` in the LSP

## 13. WebAssembly Target

- [ ] WebAssembly — also the cheapest route to an online playground, which is
      the most effective adoption funnel a new language has

## 14. Tuples (maybe)

Under consideration, not committed to.

- [ ] Tuple types: `(i32, string)`
- [ ] Tuple construction: `(1, "hello")`
- [ ] Destructuring: `var (x, y) = pair;`
- [ ] Multiple return values without a named class

---

## Open Design Decisions

1. **FFI lifetimes**: extern calls require an `unsafe` block, and the convention
   is a safe Sun wrapper around it. What remains undecided is whether the borrow
   checker should model a pointer escaping into C at all — today `ref T` hands C
   an address with no lifetime tracking across the call.
2. **Generic instantiation**: where do cross-module instantiations get emitted,
   and who owns deduplication — the compiler or the linker? (Today: use-site
   instantiation with link-time deduplication.)
3. **Memory model**: should `Vec<T>` own its allocator or take a reference?
4. **Protobuf surface**: import-only (`.proto` via manifest) shipped first. A
   native `message` declaration in Sun source with `field: T = tag;` syntax
   remains open — it would drive the same source generator (`ProtoImporter`)
   from a parser production instead of a `FileDescriptor`.

---

*Last updated: August 2026*
