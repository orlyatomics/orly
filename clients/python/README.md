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

## Reviewing a POV's changes (#746)

```python
draft = c.new_pov(safe=False, shared=False, conflicts="refuse")
c.pause(draft)                                         # hold its writes back
c.call(draft, "mypkg", "put", {"k": 1, "v": 2})
page = c.diff(draft, start=("edge",), stop=("tags",), limit=50)
for changes in c.diff_pages(draft):                    # {key, kind, before, after, op?, delta?}
    ...
result = c.promote(draft)                              # "promoted", "refused", "blocked", ...
if result["status"] != "promoted":
    c.discard(draft)
```

`diff` lists what the POV changed relative to its parent (`added`, `changed`, `removed`, or
`delta` for `+=`/`|=`); keys are lists or tuples, sent as key literals (`Addr`). A POV made with
`conflicts` reports (`"report"`) or refuses (`"refuse"`) a promotion that overwrites a key its
parent changed after the fork. See [docs/pov-review.md](../../docs/pov-review.md).

## Marshaling quirks (from the engine)

Results come back via the engine's JSON marshaling, so:

- integers are **floats** (`1` → `1.0`) — compare numerically;
- sets are **unordered arrays** — compare as sets;
- variants are `{"Tag": <payload>}` (`{"Tag": {}}` for payload-less arms).

The client returns the parsed value as-is; handle these in your code.

## Servers that require a token

A server started with a token (#710) refuses connections without it. Pass
`orly.connect(url, token=...)`, or set `ORLY_AUTH_TOKEN` (or
`ORLY_AUTH_TOKEN_FILE`, a file holding it), which `connect` reads when no
`token` is given. A missing or wrong token raises `orly.Unauthorized`. A client
with a token also works against a server without one.
