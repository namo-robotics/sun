# Sun Language for VS Code

Editor support for the [Sun programming language](https://namo-robotics.github.io/sun/):
a compiled language with Rust-style borrow checking and an LLVM backend.

## Features

- **Syntax highlighting** for `.sun` source files, backed by semantic tokens from the Sun lexer, plus file icons for `.sun` and `.moon` bundles.
- **Diagnostics** from the Sun compiler as you type: parse errors, type errors and borrow-checker errors.
- **Hover** to see the type of a variable, field, function or method.
- **Go to definition**, including into library sources: symbols from a `.moon` bundle open the original `.sun` file when it is on disk.
- **Find references** to every use of a symbol across the files of a manifest.
- **Formatting** via "Format Document" and format-on-save.
- **Cross-file analysis**: files listed in a `manifest` block are analyzed together with their entrypoint. Manifests are discovered automatically, or set explicitly with `sun.entrypoints`.

## Requirements

The extension talks to the Sun language server, `sun-lsp`, which ships with the Sun compiler.
By default it is looked up on your `PATH`; set `sun.lsp.path` to point at it if it lives
elsewhere (for example a local build at `build/sun-lsp`).

## Settings

| Setting | Description |
| --- | --- |
| `sun.lsp.path` | Path to the `sun-lsp` executable. Defaults to `sun-lsp` on `PATH`. Relative paths resolve against the workspace folder. |
| `sun.sunPath` | Extra directories added to `SUN_PATH` for module resolution. Defaults to the workspace folder. |
| `sun.entrypoints` | Entrypoint files containing a `manifest` block. When set, replaces automatic manifest discovery. |

## Learn more

- [Sun documentation](https://namo-robotics.github.io/sun/)
- [Source repository](https://github.com/namo-robotics/sun)
