# orly - Zig client

A Zig client for the Orly database over its WebSocket + JSON protocol (see
[`docs/PROTOCOL.md`](../../docs/PROTOCOL.md)). It owns the connection and
session lifecycle, builds orlyscript statements from Zig values (escaping,
argument literals, POV threading), and returns the raw JSON of each result. It
speaks the same protocol as the [Go](../go), [Python](../python) and
[TypeScript](../ts) clients, not the packed binary protocol of the C++ client.

Needs Zig 0.17.0 or newer (the client uses the `std.Io` API) and a running
`orlyi` (default `ws://127.0.0.1:8082/`).

## Install

Zig's package manager fetches a whole repository root, and this package lives
in a subdirectory, so a `clients/zig/vX.Y.Z` tag cannot be fetched by URL
directly. Use it by path instead: vendor or submodule the Orly repository (or
just copy `clients/zig`) and point your `build.zig.zon` at the directory.

```zig
// build.zig.zon
.dependencies = .{
    .orly = .{ .path = "vendor/orly/clients/zig" },
},
```

```zig
// build.zig
const orly = b.dependency("orly", .{ .target = target, .optimize = optimize });
exe.root_module.addImport("orly", orly.module("orly"));
```

The tags `clients/zig/vX.Y.Z` mark the commit each client release was cut from,
so a vendored copy can be pinned to one.

## Use

```zig
const std = @import("std");
const orly = @import("orly");

pub fn main(init: std.process.Init) !void {
    const c = try orly.Client.connect(init.gpa, init.io, .{
        .environ_map = init.environ_map, // for ORLY_AUTH_TOKEN(_FILE)
    });
    defer c.close();

    _ = try c.newSession();
    try c.install("mypkg", 1);

    const pov = try c.newPov(.{}); // new safe shared pov;
    _ = try c.call(pov.slice(), "mypkg", "put", .{ .k = 1, .s = "hi" });
    const got = try c.call(pov.slice(), "mypkg", "get", .{ .k = 1 });
    std.debug.print("{s}\n", .{got}); // raw JSON of the result
}
```

A complete program is [`examples/basic.zig`](examples/basic.zig):
`zig build example`.

### The API

| | |
| -- | -- |
| `Client.connect(gpa, io, options)` | Connects, retrying a refused connection with exponential backoff (`retries`, `backoff_ms`), and presents the token. Returns `*Client`. |
| `c.close()` | Closes the connection and frees the client. |
| `c.newSession()`, `c.exit()` | Open and end the session. `newSession` returns an `Id`. |
| `c.install(pkg, version)`, `c.uninstall(pkg, version)` | Install or uninstall a package version. |
| `c.newPov(.{ .safe, .shared, .parent })` | `new safe\|fast shared\|private pov [from {parent}];`. The defaults are `safe` and `shared`. Returns an `Id`; use `id.slice()`. |
| `c.call(pov, pkg, method, args)` | `try {pov} pkg method <{...}>;`. `args` is a struct, or `.{}` for none. |
| `c.callBatch(pov, pkg, method, args_list)` | One method, N argument records, one transaction (#253). All or nothing. |
| `c.callMany(pov, calls)` | Several different methods, one transaction (#255): `.{ .{ .pkg, .method, .args }, ... }`. |
| `c.pause(pov)`, `c.unpause(pov)` | Pause or unpause a POV's promotion. |
| `c.pages(pov, pkg, method, args, opts, context, visit)` | Keyset paging (#735): calls a method page after page, as long as it returns `<{.rows: [...], .last: ...}>`, passing `.last` back as the cursor. Needs a server with keyset paging. |
| `c.send(statement)` | Any statement, as text. |

Memory is explicit. A client keeps one buffer for the statement it builds and
one for the reply, and reuses both, so a call allocates only when a statement
or reply is larger than any before it. The slice a call returns points into the
reply buffer and stays valid until the next call on that client; copy it to
keep it. `Id` holds a UUID inline, so it needs no allocation and outlives the
reply.

### Values

Zig values become orlyscript literals:

| Zig | orlyscript |
| -- | -- |
| `bool`, integers | `true`, `42` |
| floats | `1.5` (always with a `.` or an exponent; NaN and infinity are `error.NotEncodable`) |
| `[]const u8` | a quoted string; `\`, `"` and control characters are escaped |
| a struct | a record `<{.k: 1, .s: "hi"}>`, fields in declaration order |
| a slice, array or tuple | a list `[a, b]` |
| `orly.set(x)` | a set `{a, b}` |
| `orly.raw("now()")` | verbatim, for expressions with no literal form |
| `orly.Value` | a tree built at run time |

`.{}` is the empty record `<{}>`; an empty list is an empty slice.

Results come back as the engine sends them: integers as JSON floats, sets as
unordered arrays, variants as `{"Tag": ...}`. Parse them with `std.json`.

### Errors

Every refusal the server has a typed status for is its own error, and
`c.detail()` keeps the raw reply (the server's message is in its `result`):

| Error | Status | Retry? |
| -- | -- | -- |
| `error.InsufficientMemory` | `insufficient_memory` | yes, in a few seconds |
| `error.InsufficientStorage` | `insufficient_storage` | yes, once space is freed |
| `error.WriteTooLarge` | `write_too_large` | no: split the write |
| `error.ReadTooLarge` | `read_too_large` | no: read less |
| `error.RemoteCompileDisabled` | `remote_compile_disabled` | no: compile with `orlyc` |
| `error.Unauthorized` | `unauthorized` | no: fix the token |
| `error.ServerError` | any other non-`ok` status (`source_error`, `exception`, ...) | no |

Connection trouble is `error.ConnectFailed` (after the retries),
`error.ConnectionClosed` and the I/O errors.

### The token (#710)

When `orlyi` requires a shared secret, the client presents it as the first
message, `{"auth": "<token>"}`. It comes from `Options.token`, or else from
`ORLY_AUTH_TOKEN_FILE` (the file's contents, less a trailing newline) or
`ORLY_AUTH_TOKEN`. Zig 0.17 has no global environment, so pass
`init.environ_map` as `Options.environ_map` for the environment to be read.
A refused token is `error.Unauthorized` from `connect`, which is not retried;
a client with no token connects, and its first statement gets
`error.Unauthorized`. The token never appears in an error or in `detail()`.

## Tests

```sh
cd clients/zig
zig build test     # unit tests: WebSocket framing, literals, replies; no server needed
```

`clients/smoke/run-zig.sh` runs the client against a live `orlyi`: the four
new-POV flavours with and without a parent, pause and unpause, a mixed batch, a
failing batch that leaves nothing behind, paging, and the auth refusals against
a token-enabled server. CI runs it on every push.
