# Compiler API Reference generation

The reference is generated from compiler declarations and documentation comments
in `include/` and `src/`. Edit those files to change reference content. Generated
MDX and Doxygen XML are ignored by Git. The generated section uses Nextra's
navigation, theme, and search; the Architecture pages remain a separate overview.

## Build locally

Install **Doxygen 1.15.0**, the version pinned in the docs workflow. Downloads for
Linux, macOS, Windows, and source builds are available from the
[Doxygen release](https://github.com/doxygen/doxygen/releases/tag/Release_1_15_0).
If it is not on your PATH, set `DOXYGEN` to the executable's absolute path.
The development Dockerfile installs Doxygen through apt. For an existing Ubuntu
26.04 development container, run `sudo apt install doxygen` (update apt's package
lists first if needed). Rebuild the container to pick up Dockerfile changes.
Node dependencies are installed with `npm ci --prefix docs`.

Run these commands from the repository root:

```sh
npm --prefix docs run gen-compiler-api
npm --prefix docs run dev
npm --prefix docs run build
npm --prefix docs test
```

Both development startup and production builds regenerate examples and the
reference. While the development server is running, run `gen-compiler-api` again
after editing compiler source. A compiler build and LLVM installation are not
required. Source links use the checkout's full Git commit hash; uncommitted source
changes appear locally but are not yet available at those GitHub links.

The generator requires the pinned Doxygen version and a Git checkout. Failed
extraction, malformed XML, or rendering errors fail generation. Existing source
comment warnings are recorded in `docs/generated/compiler-api/warnings.log`;
missing documentation does not fail the build. Doxygen is a documentation parser,
so complex C++ declarations can produce warnings that need inspection.

## Write documentation in the source

Place a concise `/** ... */` block before the declaration. Prefer documenting the
header declaration; implementation details can be documented at the definition.
Doxygen associates matching declarations and definitions. Ordinary `//` comments
are not reference documentation.

```cpp
/** Tracks the source position of a token. */
struct TokenPosition {
  /** Byte offset from the beginning of the source file. */
  unsigned offset;
};
```

Use `@param`, `@return`, and `@see` when they add useful information. Use `@file`
in a file comment and a documentation block before a namespace for organizational
explanations. Paragraphs, lists, code blocks, and symbol references are rendered
from Doxygen XML. Raw HTML-only documentation blocks are not executed or embedded.

All extracted types and members, including private members, static helpers, and
helpers in anonymous namespaces, remain visible even when undocumented. Function-local
variables, generated protobuf code, dependencies, and tests are excluded. File
and namespace listings link to the canonical member entry instead of repeating
it. Separate indexes list namespaces, classes, structs, enums, free functions, type
aliases, unions, global variables, macros, and files. Empty categories are omitted.
The main Compiler API Reference entry contains one tree of named Sun namespaces,
categorized types and variables, and their members. Member categories distinguish
public, protected, and private functions, fields, enums, and aliases. Enum entries
expand to their values. Separate indexes remain linkable but are hidden from the
sidebar. The Sun root starts expanded, and deeper branches start collapsed. Anonymous
helpers are documented on their file pages and grouped under their nearest named
namespace in symbol indexes. Anonymous namespace pages are not generated. Declarations outside named Sun namespaces appear under File scope.
Methods and fields stay with their owning types. These indexes expose declarations
without filling the sidebar with hundreds of entries.

Compiler namespaces follow the directory of their declaring header: for example,
`include/parsing/` uses `sun::parsing`. Implementations keep that namespace even
when their source files are split into deeper directories. Compiler AST types use
`sun::ast`; generated protobuf AST types use `sun::proto::ast` and are excluded
from the reference.

## Validation

`npm --prefix docs test` checks XML rendering, escaping, links, deterministic
output, stale page removal, and extraction from a small C++ fixture. The extraction
test runs when Doxygen is installed (including in CI), and otherwise reports a
skip. The production build checks the complete reference as native Nextra pages.
