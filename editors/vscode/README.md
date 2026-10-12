# Orlyscript VS Code Extension

Official Visual Studio Code extension providing rich language support for **Orlyscript** (`.orly`), powered by the `orly-lsp` Language Server.

## Features

- **Syntax Highlighting**: Full TextMate grammar for keywords, built-in types, operators, mutation blocks, and literals.
- **Diagnostics**: Real-time syntax and semantic/type error reporting as you type, directly using `orlyc`'s front-end parser and typechecker.
- **Hover**: Rich hover documentation for functions, parameter definitions, type signatures, and built-in keywords (`keys`, `take`, `reduce`, `sorted_by`, etc.).
- **Go to Definition**: Jump from parameter and variable references to their declarations.
- **Document Symbols**: Outline view and breadcrumb navigation for functions, given parameters, and test blocks.
- **Code Completion**: Auto-completion for keywords, primitive types, and symbols in scope.
- **Code Snippets**: Quick templates for package skeletons, functions with `where` clauses, `test` blocks, `effecting` blocks, and query operations.
- **Local Runner**: Run orlyscript files directly against a local Orly instance (`orlyi`) via `Orly: Run against local orlyi`.

## Requirements

The language server requires the `orly-lsp` (or `orly_lsp`) binary built from the Orly repository:

```bash
./dev build orly/orly_lsp
```

By default, the extension automatically looks for `orly_lsp` in standard build directories (`out_orly/debug/orly/orly_lsp`, `out_orly/release/orly/orly_lsp`) or on `PATH`.

## Extension Settings

This extension contributes the following settings:

* `orly.lsp.enabled`: Enable/disable the Orlyscript Language Server (default: `true`).
* `orly.lsp.path`: Custom path to the `orly-lsp` executable.
* `orly.orlyi.path`: Custom path to the `orlyi` server executable.
* `orly.orlyi.port`: Port of the local `orlyi` instance (default: `1984`).

## Development & Building

To compile the extension:

```bash
npm install
npm run compile
```
