# orly — Go client

A thin Go client for the Orly database over its WebSocket + JSON protocol
(see [`docs/PROTOCOL.md`](../../docs/PROTOCOL.md)). It owns the connection and
session lifecycle, builds orlyscript statements safely (string escaping,
argument literals, POV threading), and returns the raw JSON result.

It is distinct from the lower-level packed binary protocol in `orly/protocol.h`
that the native C++ client (`orly/client`) speaks.

## Install

```sh
go get github.com/orlyatomics/orly/clients/go@v0.1.0
```

The package is named `orly`; since the import path ends in `/go`, alias it:

```go
import orly "github.com/orlyatomics/orly/clients/go"
```

Requires a running `orlyi` (default `ws://127.0.0.1:8082/`). Inside an Orly
checkout the example drivers use the local copy via a `replace` directive
instead of the tagged version:

```
require github.com/orlyatomics/orly/clients/go v0.0.0
replace github.com/orlyatomics/orly/clients/go => ../../clients/go
```

## Use

```go
c, err := orly.Connect()                  // opens the WebSocket
if err != nil {
	log.Fatal(err)
}
defer c.Close()

c.NewSession()
c.Install("mypkg", 0)
pov, _ := c.NewPov()                       // "new safe shared pov;"
c.Call(pov, "mypkg", "put", map[string]any{"k": 1, "s": "hi"})
raw, _ := c.Call(pov, "mypkg", "get", map[string]any{"k": 1})
fmt.Println(string(raw))
c.Exit()
```

`Connect` retries a failed connection up to 5 times with exponential backoff
(250 ms, doubling), so a just-started or loaded `orlyi` doesn't flake startup;
the last error is returned once the retries are exhausted.

`Call(pov, package, method, args)` builds `try {pov} package method <{.k: v}>;`
and returns the raw JSON result. Argument values are encoded by `orly.Lit`:

| Go | orlyscript |
|---|---|
| `1`, `1.5` | `1`, `1.5` |
| `true` / `false` | `true` / `false` |
| `"a\"b"` | `"a\"b"` (escaped) |
| `map[string]any{"k": 1}` | `<{.k: 1}>` (record) |
| `[]any{1, 2}` | `[1, 2]` (list) |
| `orly.Set{1, 2}` | `{1, 2}` (set) |
| `orly.Raw("now()")` | `now()` (raw, un-encoded) |

## Marshaling quirks (from the engine)

Results come back via the engine's JSON marshaling, so:

- integers are **floats** (`1` → `1.0`) — compare numerically;
- sets are **unordered arrays** — compare as sets;
- variants are `{"Tag": <payload>}` (`{"Tag": {}}` for payload-less arms).

`Call`/`Send` return the raw `json.RawMessage`; decode these in your code.
