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

- [x] `extern function` declarations calling C symbols; signature types with
      no C spelling (arrays, interfaces, lambdas) are rejected with an error
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
- [x] C struct interop — class layout already matches C, and `ref T` is C's
      `T*`; structs by value work too
- [x] Calling conventions — System V eightbyte classification with
      byval/sret and register coercion (`include/abi/sysv_x86_64.h`)
- [x] Extern declarations in `.moon` libraries
- [x] Safety story — calling an extern requires an `unsafe` block, matching
      the rule the equivalent intrinsics already follow
- [x] Cross-target ABIs: per-target classification behind a triple dispatch
      (`include/abi/c_abi.h`); AArch64 AAPCS64 (ELF) implemented with
      HFA/register/indirect rules (`include/abi/aapcs64.h`)

### Cross-Compilation

An embedded and robotics language that cannot target ARM from an x86 dev machine
is unusable for its stated audience. `--target <triple>` compiles whole programs
(stdlib included) for the target; intrinsics call libc rather than x86-64
syscall assembly, so the emitted IR is target-neutral.

- [x] `--target <triple>` in the driver (`--emit-obj`, `--emit-moon`, and `-c`
      when a cross toolchain is installed)
- [x] Sysroot / linker configuration per target (`--sysroot`; the link driver
      is `<triple>-gcc`, then `clang --target`, overridable with `SUN_CC`)
- [x] Cross-compiled stdlib `.moon` artifacts — bundles are target-stamped
      (`.moon` format V4) and resolved by exact name; the build produces
      per-target directories (`build/aarch64-linux-gnu/stdlib.moon`), and
      linking a wrong-target bundle is a hard error naming both triples
- [x] Intrinsics call libc (`write`/`open`/sockets/`pthread_create`), removing
      the raw x86-64 syscall assembly that blocked non-x86 codegen
- [x] Cross binaries are testable on the dev machine: the container ships
      `g++-aarch64-linux-gnu` + `qemu-user`, and `CrossTargetTest` compiles,
      links and runs aarch64 binaries under qemu (including a struct-passing
      extern against C compiled by the real aarch64 toolchain)
- [x] Static binary linking — the default for `-c`: a self-contained
      static-pie binary (no loader, no .so dependencies, no libc version
      coupling) that runs on any Linux of the target architecture (verified
      under qemu with no sysroot). Static links prefer a musl toolchain
      (`<arch>-linux-musl-gcc`, shipped in the dev container) — MIT-licensed,
      no NSS dlopen, ~40% smaller than static glibc — falling back to glibc
      `-static` when absent. `--dynamic` restores shared-library linking for
      `.so`-only vendor libraries

### Debug Info (DWARF)

`sun::DebugInfoBuilder` (`include/debug_info_builder.h`) emits DWARF 5 when `-g`
is passed; without it, no debug metadata is created at all. Debug builds use
backend `CodeGenOptLevel::None` so stepping stays line-accurate. JITed code is
registered with gdb's JIT interface, so `gdb --args sun -g prog.sun` works too.

- [x] Line tables and `-g` in the driver
- [x] Variable and parameter locations (params, locals, `this`, loop vars,
      catch bindings, refs)
- [x] Struct/class type descriptions (plus enums, arrays, slices, pointers,
      references, closures)
- [x] Verified `gdb` / `lldb` stepping over the examples
      (`tests/test_debug_info.cpp`; lldb needs `DEBUGINFOD_URLS=` unset in
      restricted environments)
- [x] Stdlib `.moon` bundles built with `-g` — debug compiles `step` into
      stdlib source; non-debug compiles strip the bundle's debug info at link
      time (`DebugInfoBuilder::stripFromModule`), keeping binaries clean and
      the backend at full optimization
- [x] `DILexicalBlock` scoping — variables in `if`/`else`, loop, and
      `try`/`catch` blocks are scoped to their block, so shadowing resolves
      correctly in debuggers

### Generics Across Module Boundaries

Generic instantiations cross `.moon` boundaries transparently: generic
definitions are serialized into the `.moon`, and use sites instantiate them on
demand (verified with specializations not pre-declared anywhere, e.g.
`Vec<i16>`; `Option<T>`/`Result<T, E>` ship with no pre-declared
specializations at all). The stdlib's explicit `declare` specializations are
kept deliberately — they precompile common instantiations into `stdlib.moon`
so downstream programs don't pay codegen cost for them; link-time
deduplication makes a pre-declared and use-site instantiation resolve to one
symbol.

- [x] Instantiate generics on demand at the use site across modules
- [x] Serialize generic definitions (not just specializations) into `.moon`
- [x] Deduplicate identical instantiations at link time

### Match Exhaustiveness Checking

Enums and `match` exist; without exhaustiveness they are convenience rather than
safety, which undercuts the central pitch of the language.

- [x] Exhaustiveness checking over enum patterns
- [x] Useful diagnostics naming the uncovered cases
- [x] Reachability / duplicate-arm warnings

---

## 🟡 P1 — Core Language Gaps

### Optionals

Without a first-class optional, every "might not be there" API invents its own
convention and none of them compose. Decision: the Rust route — general
payload-carrying enums, with `Option<T>` landing as ordinary stdlib code once
enums are generic.

- [x] Payload-carrying enum variants (`Circle(f64)`), tagged-union layout,
      construction, and match destructuring with payload bindings (Stage 1;
      deferred within it: arrays and globals of payload enums, C-ABI
      classification, nested patterns, niche layout, `DW_TAG_variant_part`
      debug info)
- [x] Owning payloads and drop glue (Stage 3): payloads may hold classes
      with `deinit` (`Option<String>`, `Option<Vec<T>>`, …). A synthesized
      per-enum `__sun_enum_drop$<Enum>` (LinkOnceODR, merges across `.moon`)
      switches on the tag and drops live payloads; enum locals/fields are
      drop-tracked like classes. **No implicit copies**: variant construction,
      `var b = a`, assignment, by-value args, returns and `_store<T>` all
      *move* compound values (source tag-poisoned / zeroed, use-after-move
      rejected); overwrites drop the old value first. Compound match bindings
      borrow the payload slot by pointer (cannot be moved out; discriminant
      frozen for the match). Foundation work landed alongside: block-scope
      and loop-iteration drops, `break`/`continue`/nested-`return` drops,
      exception-unwind cleanup (per-call-site cleanup landing pads), and
      container element drops (`Vec`/`Map`/`LinkedList` drop live elements;
      `Vec.take` moves one out). Deferred: moving a payload *out* of a match
      binding (needs partial-move tracking).
- [x] Generic enums + expected-type inference (Stage 2): `enum Option<T>`,
      type arguments inferred from payload arguments (`Option.Some(42)`) or
      the expected type (`var x: Option<i32> = Option.None;`, return
      position, member assignment); monomorphized like generic classes with
      specializations stored on the template AST; works across `.moon`
      (deferred within it: explicit `Option<i32>.Some(...)` in expression
      position, nested-generic payload unification)
- [x] `Option<T>` and `Result<T, E>` in the stdlib (`stdlib/option.sun`,
      exported through stdlib.moon; constructed as `Option.Some(x)` /
      `Option.None` — bare `Some`/`None` prelude names are a possible later
      nicety)
- [x] Starter migration: `Map.find` (new, alongside throwing `get`),
      `String.find_char`/`rfind_char` → `Option<i64>` (sentinel `-1`
      removed), `Vec.first`/`last` → `Option<T>`; interface payloads allowed
      so `Vec<ISomething>` keeps working
- [ ] Niche-optimised representation where possible (e.g.
      `Option<raw_ptr<T>>` as a nullable pointer)
- [ ] Migrate the remaining absence APIs (`LinkedList.first/last`,
      `Vec.pop`, iterator `next()` — the last needs the `IIterator` contract
      rework); unblocked now that owning payloads have drop glue

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

### Native Protobuf Import

Import `.proto` schemas directly — no generated source files. The compiler already
links libprotobuf for `.moon`/AST serialization, so at manifest-processing time it
can parse the schema with `google::protobuf::compiler::Importer`, walk the
`FileDescriptor`, and synthesize ordinary Sun classes into a module named after the
proto `package`. Semantic analysis, the borrow checker, codegen and the LSP all see
normal code; serializers compile monomorphic and static, with no descriptor
reflection or libprotobuf dependency in the output binary.

```sun
manifest {
    protos: ["schemas/telemetry.proto"];
}
using namo.telemetry;

var status = RobotStatus(alloc);       // synthesized zero-value init (proto3 defaults)
status.robot_id = 7;
status.encode(ref buf);                // borrow-checked, appends wire bytes to Vec<u8>
var back = try RobotStatus.decode(alloc, view);   // malformed input throws IError
```

- [ ] `protos:` manifest entries, resolved through `SUN_PATH` like other imports
- [ ] Descriptor walk → synthesized AST: class per message, zero-value `init`
      taking `ref HeapAllocator`, public fields (`string` → `String`,
      `repeated T` → `Vec<T>`, `bytes` → `Vec<u8>`, `map<K,V>` → `Map<K,V>`,
      nested messages embedded by value)
- [ ] Synthesized `encode(this, ref Vec<u8>)` and `decode(...) T, IError`, plus
      `_delimited` variants (varint length prefix) for stream framing
- [ ] Proto `package foo.bar` → nested `module` decls; proto-level `import`
      resolved through the same manifest machinery
- [ ] Unknown-field preservation (hidden `Vec<u8>` per message) so
      decode-then-reencode round-trips fields from newer peers
- [ ] Depends on: `Option<T>` for proto3 explicit `optional`; tagged unions for
      `oneof`; associated/static functions for `T.decode(...)` (else a free
      function per message)

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

## ⚪ Maybe

Ideas under consideration, not committed to.

### Tuples

- [ ] Tuple types: `(i32, string)`
- [ ] Tuple construction: `(1, "hello")`
- [ ] Destructuring: `var (x, y) = pair;`
- [ ] Multiple return values without a named class

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

1. **FFI lifetimes**: extern calls now require an `unsafe` block, and the
   convention is a safe Sun wrapper around it. What remains undecided is
   whether the borrow checker should model a pointer escaping into C at all —
   today `ref T` hands C an address with no lifetime tracking across the call.
2. ~~**Optional representation**~~ *Decided:* Rust-style — `Option<T>` as an
   ordinary stdlib enum once payload enums (done) grow generics; no nullable
   type syntax.
3. **Generic instantiation**: where do cross-module instantiations get emitted, and
   who owns deduplication — the compiler or the linker?
4. ~~**Enum representation**~~ *Decided:* tagged union —
   `{ i32 tag, [M x unit] }` storage with per-variant view structs
   (`include/llvm_type_resolver.h`).
5. **Memory model**: should `Vec<T>` own its allocator or take a reference?
6. **Protobuf surface**: import-only (`.proto` via manifest), or also a native
   `message` declaration in Sun source with `field: T = tag;` syntax? The
   descriptor walk and the parser production would synthesize the same AST.

---

*Last updated: August 2026*
