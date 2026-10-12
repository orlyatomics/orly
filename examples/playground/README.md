# Orlyscript Browser Playground

An interactive browser playground for writing, compiling, and exploring Orlyscript against a live Orly database engine.

## Overview

The playground provides an instant zero-install route to experiment with Orly:
- **Code Editor**: Real-time Orlyscript syntax highlighting (mapped from the TextMate LSP grammar), line numbers, and keyboard shortcuts (`Ctrl+Enter` / `Cmd+Enter` to compile).
- **Compile & Install**: Shells out to Orly's remote compilation pipeline over WebSocket (`compile "<source>";`), automatically installing the built `.so` package (`install <name>.<version>;`).
- **Interactive Method Runner**: Discovers exported package functions via `list packages;`, generates typed parameter inputs dynamically, and invokes methods against safe shared POV branches.
- **Activity Log**: Real-time console displaying statements, POV IDs, execution duration in milliseconds, receipts, return values, and compiler diagnostics.
- **Preloaded Examples**:
  1. **Counter (`+=` and POVs)**: Demonstrates Orly's atomic commutative `+=` mutations on absent keys (#151 auto-seeding from 0), POV branch isolation, and concurrent merge.
  2. **GRC-20 Knowledge Graph**: Property-graph event sourcing from `examples/grc20-pov/`, featuring sum types (`<| Text | Number | ... |>`), `reduce` folds, time-travel queries, and exhaustive `when` matching.
  3. **Prediction Market**: Parimutuel prediction market from `examples/prediction-market/`, showing zero-coordination bets and probability calculations across rounds.

---

## Quickstart (Docker Compose)

Run the entire playground (Orly engine + session proxy + web UI) with a single command:

```bash
cd examples/playground
docker compose up
```

Open [http://localhost:3000](http://localhost:3000) in your browser.

---

## Native / Local Development

### 1. Start `orlyi` with remote compile enabled:

```bash
# In the repository root
./dev build orly/server/orlyi
./out_orly/debug/orly/server/orlyi \
  --allow_remote_compile \
  --auth_token=playground-secret-token-12345678 \
  --read_budget=134217728 \
  --bind_address=127.0.0.1 \
  --ws_port_number=8082
```

### 2. Build and run the playground service:

```bash
cd examples/playground
npm install
npm run build
ORLY_AUTH_TOKEN=playground-secret-token-12345678 npm start
```

Visit [http://localhost:3000](http://localhost:3000).

---

## Architectural Comparison & Recommendation (#740)

Issue #740 asked to evaluate two backend architectures:
1. **Sandboxed `orlyi` per session behind WebSocket API**
2. **WebAssembly frontend (diagnostics in browser) + server execution**

### Comparison

| Dimension | Sandboxed Server-side `orlyi` | WebAssembly Frontend Diagnostics |
| :--- | :--- | :--- |
| **Execution Accuracy** | 100% native; exact match with production C++ compiler and engine runtime. | Instant syntax/type checking in browser, but still requires server to execute bytecode/binary. |
| **Code Execution Model** | Orly packages compile to native ELF shared objects (`.so`) using `g++`. | Compiling C++ clang/g++ toolchain to Wasm requires heavy WebAssembly bundles (~30-50 MB). |
| **Security & Isolation** | Enforced via `--allow_remote_compile` (#705), read budget (#694), shared token (#710), session timeouts, and container cgroups. | Client-side diagnostics run locally; server still requires sandbox for execution. |
| **Implementation Complexity** | Low: leverages existing `orlyi` WebSocket protocol and TypeScript client (`clients/ts`). | High: requires Emscripten port of Nycr parser, AST, and type system. |
| **Hosting Costs** | Moderate: requires container instances with CPU/RAM allocations per concurrent cluster. | Low on compute for editing/diagnostics, but still requires backend workers for query execution. |

### Recommendation

**Recommend: Sandboxed per-session container / proxy model.**

Orly's architecture compiles orlyscript to optimized C++ and native shared objects (`.so`) loaded dynamically into the process. Porting that full compilation toolchain into client-side WebAssembly would require shipping an entire C++ compiler into the browser (~40+ MB download), whereas `orlyi` already has built-in remote compile (`--allow_remote_compile`), memory budgets (`--memory_budget_mb`), read budgets (`--read_budget`), and auth tokens (`--auth_token`).

A containerized proxy with ephemeral sessions, idle timeouts (15 minutes), and memory limits (`docker compose` or Fly.io/ECS micro-VMs) provides 100% engine fidelity with minimal operational overhead.
