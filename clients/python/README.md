# orly — Python client

A thin Python client for the Orly database over its WebSocket + JSON protocol
(see [`docs/PROTOCOL.md`](../../docs/PROTOCOL.md)). It owns the connection and
session lifecycle, builds orlyscript statements safely (string escaping,
argument literals, POV threading), and returns the parsed JSON result.

## Install

```sh
pip install -e clients/python      # from an Orly checkout
```

Requires a running `orlyi` (default `ws://127.0.0.1:8082/`).

## Use

```python
import orly

with orly.connect() as c:                 # opens the WebSocket
    c.new_session()
    c.install("mypkg", 0)
    pov = c.new_pov()                     # "new safe shared pov;"
    c.call(pov, "mypkg", "put", {"k": 1, "s": "hi"})
    print(c.call(pov, "mypkg", "get", {"k": 1}))
    c.exit()
```

`call(pov, package, method, args)` builds `try {pov} package method <{.k: v}>;`.
Argument values are encoded by `orly.lit`:

| Python | orlyscript |
|---|---|
| `1`, `1.5` | `1`, `1.5` |
| `True` / `False` | `true` / `false` |
| `"a\"b"` | `"a\"b"` (escaped) |
| `{"k": 1}` | `<{.k: 1}>` (record) |
| `[1, 2]` | `[1, 2]` (list) |
| `{1, 2}` | `{1, 2}` (set) |
| `orly.Lit("now()")` | `now()` (raw, un-encoded) |

## Marshaling quirks (from the engine)

Results come back via the engine's JSON marshaling, so:

- integers are **floats** (`1` → `1.0`) — compare numerically;
- sets are **unordered arrays** — compare as sets;
- variants are `{"Tag": <payload>}` (`{"Tag": {}}` for payload-less arms).

The client returns the parsed value as-is; handle these in your code.
