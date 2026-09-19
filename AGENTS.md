# Agent Development Guide

A file for guiding coding agents.

## Commands

- Never use git commands except readonly ones like `git status` and `git diff`.
- Run all commands from the workspace root. Create any temp files in `${workspaceRoot}/tmp`.
- `sun fmt` for formating sun source files

## Sun Language Conventions

- Sun DOES NOT ALLOW IMPLICIT COPIES.
- Sun DOES NOT HAVE MACROS
- Sun minimizes and discourages alternative syntaxes that do the same thing.
- Sun avoids hidden memory allocations
- Sun is a memory-safe language that guaruntees the absense of undefined behavior outside of `unsafe` blocks
- Usage of `unsafe` blocks is highly discouraged

## Compiler Conventions

- Errors: `logError()` / `logAndThrowError()` for compilation errors.

## Code Comments (all code, including .sun files)
- Write concise comments in plain English for a general audience of software engineers.
- Every namespace, function, and type definition must have a documentation block comment (`/** ... */`) describing what it does or what it is for. This applies to all first-party source code, including C++ and `.sun` files, regardless of visibility.
- Cover private and internal helpers, methods, constructors, destructors, anonymous namespaces, classes, structs, interfaces, enums, and type aliases, as well as public APIs. Document modules too.
- Put the comment immediately before the declaration or definition it documents. For a function declared in a header and implemented separately, document the declaration; add implementation comments when they explain details beyond that contract.
- Explain the purpose or behavior instead of merely restating the symbol name. Mention ownership, side effects, constraints, or failure behavior when needed to understand correct use.
- Preserve useful existing documentation and keep it accurate when changing code. Do not edit generated code or third-party dependencies to add comments.
- Do not hard-code numeric values that are subject to change in comments.

## Commit messages
- Subject: `<scope>: <description>` (scope = subsystem/package/area; imperative; no `feat`/`fix` types).
- Body: blank line, then one concise bullet per key change if not already captured in the subject.
- Do not add AI attribution to commits or PRs (no Co-Authored-By, Generated-with, tool names, or session links).

## Planning

- Write plains in plain english, avoiding unnecessary jargon and abreviations.
- Use simple, concrete examples in explanations when it makes sense to do so.

## Issues and PR Guidelinese

- Never create PRs or issues unless specifically asked by the user.

## Roadmap

- Roadmap items that are entirely completed should be removed from the roadmap doc.