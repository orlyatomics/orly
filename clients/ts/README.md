# @orlyatomics/orly — TypeScript client

A typed client for the Orly database over its WebSocket + JSON protocol
(see [`docs/PROTOCOL.md`](../../docs/PROTOCOL.md)). Works in the **browser**
(uses the global `WebSocket`) and in **Node** (dynamically imports `ws`). It
owns the connection and session lifecycle, builds orlyscript statements safely
(string escaping, argument literals, POV threading), and resolves the parsed
JSON result.

## Install

```sh
npm install @orlyatomics/orly     # plus `npm install ws` for Node
```

Requires a running `orlyi` (default `ws://127.0.0.1:8082/`).

(Inside this repo, packages and examples install the driver under the alias
`orly` — `"orly": "file:../ts"` — so their imports read `from "orly"`; the
same alias is available to consumers as
`npm install orly@npm:@orlyatomics/orly`.)

## Use

```ts
import { connect } from "@orlyatomics/orly";

const c = await connect();                 // opens the WebSocket
await c.newSession();
await c.install("mypkg", 0);
const pov = await c.newPov();              // "new safe shared pov;"
await c.call(pov, "mypkg", "put", { k: 1, s: "hi" });
console.log(await c.call(pov, "mypkg", "get", { k: 1 }));
await c.exit();
```

`callMany(pov, [[pkg, method, args], ...])` runs several different methods in one
transaction: every write lands or none does. It resolves to an array with one result
per call.

`pages(pov, pkg, method, args)` pages through a keyset-paging method (#735): one that takes a
`last` cursor and returns `<{.rows: [...], .last: ...}>`. It is an async iterator over each
page's rows, passing each page's `.last` back for the next, and stops at the first empty page
(or, given `{ pageSize }`, at a shorter one). See
[paging through keys](../../docs/walkthrough.md#paging-through-keys).

`call(pov, package, method, args)` builds `try {pov} package method <{.k: v}>;`.
Argument values are encoded by `lit`:

| JS | orlyscript |
|---|---|
| `1`, `1.5` | `1`, `1.5` |
| `true` / `false` | `true` / `false` |
| `'a"b'` | `"a\"b"` (escaped) |
| `{ k: 1 }` | `<{.k: 1}>` (record) |
| `[1, 2]` | `[1, 2]` (list) |
| `set([1, 2])` | `{1, 2}` (set) |
| `raw("now()")` | `now()` (raw, un-encoded) |

## Reviewing a POV's changes (#746)

```ts
const draft = await c.newPov({ safe: false, shared: false, conflicts: "refuse" });
await c.pause(draft);                                  // hold its writes back
await c.call(draft, "mypkg", "put", { k: 1, v: 2 });
const page = await c.diff(draft, { start: ["edge"], stop: ["tags"], limit: 50 });
for await (const changes of c.diffPages(draft)) { /* {key, kind, before, after, op?, delta?} */ }
const result = await c.promote(draft);                 // "promoted", "refused", "blocked", ...
if (result.status !== "promoted") await c.discard(draft);
```

`diff` lists what the POV changed relative to its parent (`added`, `changed`, `removed`, or
`delta` for `+=`/`|=`); keys are arrays, sent as key literals (`addr([...])` builds one). A POV
made with `conflicts` reports (`"report"`) or refuses (`"refuse"`) a promotion that overwrites a
key its parent changed after the fork. See
[docs/pov-review.md](../../docs/pov-review.md).

## Marshaling quirks (from the engine)

Results come back via the engine's JSON marshaling, so:

- integers and floats are both JS `number`s (JSON has no split);
- sets are **unordered arrays** — compare as sets;
- variants are `{ Tag: <payload> }` (`{ Tag: {} }` for payload-less arms).

`call` resolves the parsed value as-is; handle these in your code. Non-`ok`
replies reject with an `OrlyError`. A write the server refuses because it is low
on disk space rejects with its subclass `InsufficientStorageError`: nothing was
written, reads still work, and the write can be retried later. A write refused
because the server's update pools are down to the reserve kept for merges rejects
with `InsufficientMemoryError`, which works the same way and usually clears within
seconds. A single write with more entries than the server can ever merge (half
its Update Entry reserve) rejects with `WriteTooLargeError`. That one is not
retryable: split the batch into smaller ones. A call that walks more rows, or builds more
result memory, than the server's per-read budget rejects with
`ReadTooLargeError`; that one isn't retryable as sent either: read a narrower
range. A `compile` statement sent to a
server started without `--allow_remote_compile` rejects with
`RemoteCompileDisabledError`.

A server started with a token (#710) refuses connections without it. Pass
`connect(url, { token })`; in Node, `connect` otherwise reads `ORLY_AUTH_TOKEN`
or the file named by `ORLY_AUTH_TOKEN_FILE`. A missing or wrong token rejects
with `UnauthorizedError`, which `connect` doesn't retry. A client with a token
also works against a server without one. A token in browser code is visible to
anyone who loads the page.
