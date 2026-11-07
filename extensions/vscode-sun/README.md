# Sun Language for VS Code

Editor support for the [Sun programming language](https://namo-robotics.github.io/sun/):
a compiled language with Rust-style borrow checking and an LLVM backend.

## Features

- **Syntax highlighting** with C++-style theme scopes: control flow, declarations, primitive types, named types, functions, and variables have separate roles. Constructors and destructors use function highlighting. Semantic tokens refine names without overriding keyword colors. Colors follow your active theme. Includes file icons for `.sun` and `.moon` bundles.
- **Diagnostics** from the Sun compiler as you type: parse errors, type errors and borrow-checker errors.
- **Hover** to see the type of a variable, field, function or method.
- **Go to definition**, including into library sources: symbols from a `.moon` bundle open the original `.sun` file when it is on disk.
- **Find references** to every use of a symbol across the files of a manifest. Referencing an interface member also lists the class members implementing it, and the other way around.
- **Rename symbol** across the files of a manifest. An interface member is renamed together with the class members implementing it. Symbols declared in a library cannot be renamed.
- **Formatting** via "Format Document" and format-on-save.
- **Cross-file analysis**: files listed in a `manifest` block are analyzed together with their entrypoint. Entrypoints are configured with `sun.entrypoints` or `sun.sun_configs`.

## Syntax highlighting only

Enable **Sun: Syntax Only** in Settings, or add this to workspace settings:

```json
{
    "sun.syntaxOnly": true
}
```

This keeps syntax highlighting while disabling the language server and Test
Explorer, including diagnostics, semantic tokens, formatting, and navigation.
Use it for screenshots or examples that do not compile. Changes apply immediately;
turn it off to restore language features. No Sun installation is needed in this mode.

## Requirements

The extension talks to the Sun language server, `sun-lsp`, which ships with the Sun compiler.
By default it is looked up on your `PATH`; set `sun.lsp_path` to point at it if it lives
elsewhere (for example a local build at `build/sun-lsp`).

## Building from source

Packaging requires Node.js 22 or newer, npm, and the Sun compiler build
dependencies. With nvm, select Node.js 24 before building:

```sh
nvm install 24
nvm use 24
./scripts/build-extension.sh
```

Run the script from the Sun repository root. It builds `sun-lsp`, compiles the
extension, creates `extensions/vscode-sun/sun-language.vsix`, and installs it
when a VS Code command-line tool is available.

If packaging fails with `ReferenceError: File is not defined` in `undici`, check
`node --version` in the build terminal and switch to a supported version.

## Settings

| Setting | Description |
| --- | --- |
| `sun.syntaxOnly` | Keep only syntax highlighting; disable the language server and Test Explorer. Defaults to `false`. |
| `sun.lsp_path` | Path to the `sun-lsp` executable. Defaults to `sun-lsp` on `PATH`. Relative paths resolve against the workspace folder. |
| `sun.sun_configs` | `sun-config.json` files whose `entrypoints` lists describe the project. Defaults to the workspace root's. Test discovery and cross-file analysis use their declared entrypoints. |
| `sun.sun_path` | Extra directories added to `SUN_PATH` for module resolution. Defaults to the workspace folder. |
| `sun.entrypoints` | Entrypoint files containing a `manifest` block. Combined with entrypoints declared by `sun.sun_configs`. |
| `sun.path_variables` | Path variables for manifest entries, e.g. `{"LIBS": "libs"}` for `libraries: ["$LIBS/mathlib.moon"]`. Equivalent to the compiler's `--path-var` flag; relative values resolve against the entrypoint's directory. |

A folder can also hold a `sun-config.json` defining `sunPath` and
`pathVariables` for the entrypoints in and below it. Both the compiler and
the language server merge every config from the entrypoint's folder upward
(nearest definitions win; `"root": true` stops the search), and the merged
definitions override the settings above.

## Test Explorer

Test Explorer asks `sun-lsp` for configured suites and expected test binaries,
then runs each supported binary with `--list-tests`. Suites appear immediately;
their tests appear as individual listings finish. Listing does not invoke test
functions. Test names and source locations describe the last compiled build.

Starting a run immediately marks selected tests as queued while discovery and
freshness checks finish. Each test switches to running when its process starts.

Execution uses a separate process for each selected test, passing an exact
`--test-filter` and `--test-sequential`. Tests still run in parallel, with a shared
limit of half the available CPU cores (at least one process). This isolates
process-wide signal handlers, working directories, and file descriptors between
tests. Each test's own threads still run normally. Cancellation stops active
process groups and skips queued tests. Direct binary execution retains its
existing runner options.

Build tests before first discovery. Missing binaries show **Build required**.
Older binaries without the listing capability show **Rebuild required for test
discovery** and are never launched to probe their options. Rebuild them with the
updated compiler. Update both `sun-lsp` and this extension together.

Source edits update suite freshness without relisting unchanged binaries. Stale
suites show **Needs rebuild — tests from last build** and remain runnable. A run
prints the warning again; passing tests does not clear it. Unreadable or unresolved
inputs show **Freshness unknown** and also remain runnable when a supported binary
is available. Missing binaries cannot run. Freshness currently compares input and
binary timestamps; it is not a full comparison of compiler settings or build hashes.

Project configs, manifest sources, dependency artifacts, and binaries trigger
refreshes. Exact binary paths are polled even when build directories are excluded
from editor file watching. Manual refresh bypasses the listing cache. Discovery
uses at most two listing processes at once and limits each to ten seconds. Errors
appear on their suite and in the **Sun Tests** output channel.

The binary listing is JSON with `version: 1` and a `tests` array. Each item contains
`id` (dotted runner name), `label`, `file` (absolute build-time path), and `range`
with zero-based lines and UTF-16 character positions. The executable's embedded
build record advertises the supported listing version, so old runners that ignore
unknown arguments are never accidentally executed during discovery.

## Learn more

- [Sun documentation](https://namo-robotics.github.io/sun/)
- [Source repository](https://github.com/namo-robotics/sun)
