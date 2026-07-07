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

## Marshaling quirks (from the engine)

Results come back via the engine's JSON marshaling, so:

- integers and floats are both JS `number`s (JSON has no split);
- sets are **unordered arrays** — compare as sets;
- variants are `{ Tag: <payload> }` (`{ Tag: {} }` for payload-less arms).

`call` resolves the parsed value as-is; handle these in your code. Non-`ok`
replies reject with an `OrlyError`.
