# orly — Go client

A thin Go client for the Orly database over its WebSocket + JSON protocol
(see [`docs/PROTOCOL.md`](../../docs/PROTOCOL.md)). It owns the connection and
session lifecycle, builds orlyscript statements safely (string escaping,
argument literals, POV threading), and returns the raw JSON result.

It is distinct from the lower-level packed binary protocol in `orly/protocol.h`
that the native C++ client (`orly/client`) speaks.

## Install

```sh
go get github.com/orlyatomics/orly/clients/go@v0.2.0
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

`Pages(pov, package, method, args, opts, fn)` pages through a keyset-paging method
(#735): one that takes a `last` cursor and returns `<{.rows: [...], .last: ...}>`. It calls
`fn` with each page's rows, passes each page's `.last` back for the next (integral numbers
back as ints), and stops at the first empty page, at one shorter than `opts.PageSize`, or
when `fn` returns `orly.ErrStopPages`. See
[paging through keys](../../docs/walkthrough.md#paging-through-keys).

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

## Reviewing a POV's changes (#746)

```go
draft, _ := c.NewPovWith(orly.PovOptions{Conflicts: "refuse"})
c.Send(fmt.Sprintf("pause {%s};", draft))              // hold its writes back
c.Call(draft, "mypkg", "put", map[string]any{"k": 1, "v": 2})
page, _ := c.Diff(draft, orly.DiffOptions{Start: orly.Addr{"edge"}, Stop: orly.Addr{"tags"}, Limit: 50})
all, _ := c.DiffAll(draft, orly.DiffOptions{})        // []PovChange{Key, Kind, Before, After, Op, Delta}
result, _ := c.Promote(draft, false, 30*time.Second)  // "promoted", "refused", "blocked", ...
if result.Status != "promoted" {
	c.Discard(draft)
}
```

`Diff` lists what the POV changed relative to its parent (`added`, `changed`, `removed`, or
`delta` for `+=`/`|=`). A POV made with `Conflicts` reports (`"report"`) or refuses (`"refuse"`) a
promotion that overwrites a key its parent changed after the fork. See
[docs/pov-review.md](../../docs/pov-review.md).

## Marshaling quirks (from the engine)

Results come back via the engine's JSON marshaling, so:

- integers are **floats** (`1` → `1.0`) — compare numerically;
- sets are **unordered arrays** — compare as sets;
- variants are `{"Tag": <payload>}` (`{"Tag": {}}` for payload-less arms).

`Call`/`Send` return the raw `json.RawMessage`; decode these in your code.

## Servers that require a token

A server started with a token (#710) refuses connections without it.
`ConnectURL` presents `ORLY_AUTH_TOKEN` (or the contents of the file named by
`ORLY_AUTH_TOKEN_FILE`); `ConnectURLWithToken(url, token)` takes it directly. A
missing or wrong token returns an error wrapping `ErrUnauthorized`. A client
with a token also works against a server without one.
