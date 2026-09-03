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

## Code Comments
- All code comments should be concise, plain-english written for a general audience of software engineers.
- Every public function, class, module, etc should have a concise, plain-english block comment describing what it does.
- Do not hard-code numeric values that subject to change in comments.

## Commit messages
- Subject: `<scope>: <description>` (scope = subsystem/package/area; imperative; no `feat`/`fix` types).
- Body: blank line, then one concise bullet per key change if not already captured in the subject.
- Do not add AI attribution to commits or PRs (no Co-Authored-By, Generated-with, tool names, or session links).

## Issues and PR Guidelinese

- Never create PRs or issues unless specifically asked by the user.