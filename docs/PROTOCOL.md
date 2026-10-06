# Orly client protocol (WebSocket + JSON)

How an application talks to a running `orlyi` over WebSocket. This is the path
the `examples/` drivers use and the contract any client library (Python, Go,
TypeScript, …) should implement. It is distinct from the lower-level packed
binary protocol in `orly/protocol.h` that the native C++ client
(`orly/client/`) speaks.

> Status: today every demo hand-rolls this protocol in both `demo.py` and
> `demo.go` (14 near-identical copies). This document is the shared spec those
> clients should be consolidated against — see "Toward a client SDK" at the end.

## Connection

Open a WebSocket to the server (default `ws://127.0.0.1:8082/`). Each connection
carries at most one session. Concurrency is modeled by opening **one connection
(and session) per concurrent writer**, all operating on the same shared POV —
that separateness is what exercises the commutative merge.

## Request / reply

A request is a single **orlyscript statement string**, terminated with `;`,
sent as one WebSocket text message. The server replies with one JSON message:

```json
{ "status": "ok", "result": <value> }
```

- `status` is `"ok"` on success; anything else is an error (the message carries
  the detail). Clients should treat non-`ok` as a raised error.
- `result` is present on statements that produce a value (see each statement).
  Its shape depends on the statement; for `try` it is the JSON marshaling of the
  method's return value (see "JSON marshaling" below).
- A statement that fails to compile replies `"status": "source_error"`, with
  the message in `result` and its position in `pos` (`line:col-line:col`).
  `compiler_loc` names the compiler source line that raised it -- useful in a
  compiler bug report, meaningless to the statement's author, so clients should
  not show it by default (#557).
- A write refused because the server is low on disk space replies
  `"status": "insufficient_storage"`, with the reason in `result` (#590). Only
  writes are refused: reads, new sessions and new POVs keep working. Nothing was
  written, so the write can be retried once space is freed. The refusal starts
  when free space falls below `orlyi`'s reserve plus what recent disk merges may
  still allocate. The reserve is `--disk_reserve_mb` if set, else
  `--disk_reserve_pct` of the disk (default 10%, and at least 64 MiB or a quarter
  of the disk, whichever is smaller). Over the binary protocol the same refusal
  is an error whose message starts with `insufficient storage`.
- A write refused because the server's update pools are down to the reserve kept
  for its merges replies `"status": "insufficient_memory"`, with the reason in
  `result` (#607). Writes are buffered in fixed-size memory pools (the Update and
  Update Entry pools) until merges flush them, and a merge needs room in those
  same pools to do its work, so `orlyi` keeps `--memory_reserve_pct` of each pool
  (default 25%) for the merges and refuses any write that would use it. Only
  writes are refused: reads, new sessions and new POVs keep working. Nothing was
  written, so the write can be retried; writes are accepted again once the merges
  have freed the pools, usually within seconds. Large batches reach the limit
  sooner, because a batch is held as one update with an entry per write.
  `--memory_reserve_pct=0` turns this off. Over the binary protocol the same
  refusal is an error whose message starts with `insufficient memory`.

## Statements

The server accepts exactly these (handlers in `orly/server/ws.cc`):

| Statement | Form | `result` |
|---|---|---|
| New session | `new session;` | session id (string) |
| Resume session | `resume session <id>;` | session id (string) |
| Set user id | `set user id <id>;` | — |
| Set TTL | `set ttl <durable-id> <seconds>;` | — |
| Install package | `install <pkg>.<version>;` | — |
| Uninstall package | `uninstall <pkg>.<version>;` | — |
| New POV | `new (safe\|fast) (shared\|private) pov [from {<pov-id>}];` | POV id (string) |
| Call a method | `try {<pov-id>} <pkg> <method> <args>;` | method result (JSON, marshaled) |
| Batch a method | `try {<pov-id>} <pkg> <method> [<args1>, <args2>, ...];` | JSON array of N per-call results |
| Batch different methods | `try {<pov-id>} [<pkg1> <method1> <args1>, <pkg2> <method2> <args2>, ...];` | JSON array of N per-call results |
| Pause / unpause POV | `pause {<id>};` / `unpause {<id>};` | `"paused"` / `"unpaused"` |
| Tail | `tail;` | streamed updates |
| Exit | `exit;` | — |

### Typical lifecycle

```
new session;                          -> "<session-uuid>"
install mypkg.0;
new safe shared pov;                  -> "<pov-uuid>"     (thread this into every try)
try {<pov-uuid>} mypkg my_method <{.k: 1, .s: "hi"}>;   -> <result json>
exit;
```

- **`try` args** are an orlyscript object literal: `<{.name: expr, ...}>` (empty:
  `<{}>`). Scalars, strings, records, sets, etc. are written as orlyscript
  literals; string values must be escaped for an orlyscript string literal.
- **Batched `try`** (`#253`) invokes **one** `(pkg, method)` against **N** argument
  records — a bracketed, comma-separated list (`[<{...}>, <{...}>, ...]`, at least
  one) — folding all N calls into a **single transaction**. It exists to amortize
  the fixed per-round-trip cost (parse, pov-open + context build, commit, network
  round-trip) across a bulk load or commutative fan-in; expect ~3–5× write
  throughput on batchable workloads. `result` is a JSON **array** with one entry
  per call, in statement order (pure-effect methods yield `null`s). Semantics:
  - **All-or-nothing.** The batch is one transaction; a throw in any call aborts
    the whole batch before commit (error reply, no partial write).
  - **Snapshot isolation, no read-your-writes.** Every call reads the *same*
    pre-batch snapshot — call *k+1* does **not** see call *k*'s mutation. A batch
    is a write-coalescing primitive, **not** a transaction script;
    sequential-dependent writes must stay separate `try` calls.
  - **Commutative folds, assigns collapse last-wins.** Commutative ops (`+=`, `|=`,
    …) to the same key across calls fold (summed on read — the win); a non-commutative
    `=`/delete to the same key within one batch collapses in statement order.
  - One update ⇒ **one** meta record / replication notification per batch (records
    the method plus all N arg sets under index-prefixed names).
  - Clients: `call_batch` (python), `CallBatch` (go), `callBatch` (ts).
- **Mixed batched `try`** (`#255`) is the same, except that each call names its own
  package and method: `try {<pov>} [pkg1 m1 <{...}>, pkg2 m2 <{...}>, ...];`. Use it
  to make several different writes atomic, e.g. creating an entity and linking it in
  one step. Every rule above holds: one transaction, all-or-nothing (a call to an
  unknown package or method rejects the whole batch), the same pre-batch snapshot for
  every call. `result` is a JSON array with one entry per call, in order; unlike a
  same-method batch, the entries may differ in type. The meta record names the first
  call's method and records every call's package and method under `<i>.$package` and
  `<i>.$method`, next to its args.
  - Clients: `call_many` (python), `CallMany` (go), `callMany` (ts).
- **POV flavors**: `safe` vs unsafe (conflict guarantee), `shared` vs `private`
  (visibility), optional `parent`. Demos use `new safe shared pov;`.
- **POVs are ephemeral across restarts** (#439). Updates promoted to the global
  POV are durable; a private/shared POV's own un-promoted state is not. After a
  server restart, a `try` against a pre-restart POV id fails with a clean
  "povs are ephemeral" error — create a new POV and retry. (Sessions, by
  contrast, do survive: `resume session <id>;` works across a restart.)
- **Time-travel is not a protocol verb.** Historical / as-of reads are expressed
  *in orlyscript* — a package method that takes an `.as_of` argument (and/or a
  key-encoded version axis) and folds history in-engine. The protocol just calls
  that method like any other. (The packed binary protocol additionally exposes
  tracking-ids / as-of-by-id; the WS+JSON path does not.)

## JSON marshaling (the rough edges a client should smooth)

`try` results come from `Var::Jsonify`. Clients must account for:

1. **Numbers are floats.** An `int` comes back as a JSON float — `1` reads as
   `1.0`. Compare numerically, not by JSON identity.
2. **Sets are arrays, unordered.** A set marshals as a JSON array with no order
   guarantee. Compare as a set, not a list.
3. **Variants are tagged objects.** A variant arm marshals as `{"Tag": <payload>}`;
   a payload-less arm as `{"Tag": {}}`.
4. **Records are objects** keyed by field name (without the leading `.`).
5. **Errors are stringly-typed** — failures surface as a non-`ok` `status`, not a
   structured error code. `insufficient_storage` and `insufficient_memory` are the
   statuses worth matching on: they mean "retry later", not "this statement is
   wrong".

## Toward a client SDK

The protocol above is small and language-agnostic, but it is currently
reimplemented per demo per language. The intended consolidation:

1. **This spec** — the single source of truth (no engine needed).
2. **`orly-py` / `orly-go`** — one thin client each (~100 lines) implementing the
   spec: connect, `send(stmt) -> result` (raising on non-`ok`), session/install/
   pov lifecycle helpers, a `call(pov, pkg, method, args)` that builds the `try`
   statement, and a typed-literal/escaper helper. The `examples/` drivers import
   it instead of copy-pasting.
3. **`orly-ts`** — a typed browser/Node SDK; the on-ramp for app developers.

A client's core job is to (a) own the connection + session lifecycle, (b) build
statement strings safely (escaping, arg literals, POV threading), and (c) hide
the marshaling quirks above behind typed results.
