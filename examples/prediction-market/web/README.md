# `web/` — browser UI for the prediction market

A tiny single-page UI for the [parimutuel prediction market](../), on the
[orly TypeScript client](../../../clients/ts) — which is **browser-native**
(its `connect` uses the global `WebSocket`), so the page talks straight to a
running `orlyi`.

The point: **open it in two browser tabs (same `#pov=` link) and you're two
traders on the same market.** Click to bet; the price moves live in both tabs,
with zero coordination — every bet is a commutative `+=` in the engine and the
price is a read-time fold of the trade log.

## Try it

```sh
./serve.sh           # compiles market.orly, starts orlyi, builds the bundle,
                     # serves on http://localhost:8000
```

Then open **http://localhost:8000** — and open it again in a second tab via the
share link the page prints (`…#pov=<id>`) to trade against yourself. Needs a
debug build (`make debug`) and Node 18+.

(`./build.sh` alone just type-checks `app.ts` and bundles it to `app.js` with
esbuild — useful in CI; the SDK's Node-only `import("ws")` is left external and
never runs in the browser.)

## Honesty note

The build is verified (type-checks under `tsc --strict` + DOM, bundles clean),
but the **in-browser runtime was not verified in this repo's CI/sandbox** (no
headless browser). If you hit a rough edge running it, that's why — please file
it. The terminal driver ([`../demo.ts`](../demo.ts), via `../run-ts.sh`) *is*
verified end-to-end and is the canonical proof of the engine behavior.

## Files

- `index.html` — the market card (YES/NO bars, live %, bet buttons, share link).
- `app.ts` — connects, reads `price`/`pool`/`position`, places bets, polls for
  other tabs' bets. Imports the `orly` client.
- `build.sh` — type-check + esbuild bundle → `app.js`.
- `serve.sh` — one command: orlyi + bundle + static server.
