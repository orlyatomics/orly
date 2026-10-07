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

If the server was started with a token (see "Shared secret" below), the first
message on every connection must be `{"auth": "<token>"}`.

## Security and trust model

By default Orly has **no authentication and no authorization** (#705). Anyone
who can open a connection to `orlyi` can create sessions and POVs, install and
uninstall packages, and read and write any data the installed packages reach.
`set user id` records who a client *says* it is; it is attribution, not
identity. Neither the WebSocket nor the binary protocol encrypts traffic.

So run `orlyi` only where every client that can reach it is trusted: on the
same host, on a private network, or behind an application that authenticates
its users and talks to `orlyi` on their behalf. An optional shared secret
(#710, below) makes accidental exposure much harder; it is one secret for
every client, not per-user identities or permissions, and it travels in the
clear unless TLS carries the connection (see "TLS via a reverse proxy").

- **Listeners.** By default `orlyi` binds its client (`--port_number`),
  WebSocket (`--ws_port_number`) and reporting (`--reporting_port_number`)
  listeners to `127.0.0.1`, so only processes on the same host can connect.
  `--bind_address=<ip>` changes that for all three; `--bind_address=0.0.0.0`
  listens on every interface. `orlyi` logs each address it bound at startup
  (`client listener bound to 127.0.0.1:19380`, with `--log_info`).
- **Replication.** The replication listener (`--slave_port_number`) binds
  every interface by default, because a slave usually runs on another host.
  Without a replication token it is unauthenticated too, and its stream
  carries all of the data, so keep peers on a private network and set
  `--slave_bind_address` to that network's address.
- **Docker.** Inside the published image `orlyi` binds every interface, since
  `docker run -p` can't reach a loopback listener in the container. Who can
  connect is then decided by the `-p` mapping: `-p 127.0.0.1:8082:8082`
  publishes to the host's loopback only, while `-p 8082:8082` publishes on
  every host interface, to anyone who can reach the host.
- **`compile`.** The `compile` statement takes orlyscript source, builds it
  into a package with the server's C++ compiler, and puts the result in the
  package directory, from where `install` loads it into the `orlyi` process. A
  client that can send it can run code of its choosing inside the server. It is
  refused unless `orlyi` is started with `--allow_remote_compile`, both on a
  bare host and in the image; a refused `compile` replies `"status":
  "remote_compile_disabled"`. Without it, compile packages with `orlyc` on the
  server and `install` them over the protocol, which is what every client and
  example in this repo does. `install` only loads packages already in the
  package directory.

### Shared secret (#710)

Off unless configured. Start `orlyi` with a token and every client connection
must present it before any statement runs, and a slave must present the
replication token to join a master.

| setting | flag | environment | default |
| -- | -- | -- | -- |
| client token | `--auth_token_file=<path>` (or `--auth_token=<value>`) | `ORLY_AUTH_TOKEN_FILE`, `ORLY_AUTH_TOKEN` | none: no authentication |
| replication token | `--replication_token_file=<path>` (or `--replication_token=<value>`) | `ORLY_REPLICATION_TOKEN_FILE`, `ORLY_REPLICATION_TOKEN` | the client token |

- A token is 16 to 1024 printable ASCII characters with no spaces, for example
  `openssl rand -hex 32`. A file holds the token and at most one trailing
  newline. A flag beats the environment; giving both a file and a value at the
  same level is an error, and so is a malformed token, so `orlyi` refuses to
  start rather than run open by mistake.
- Prefer the file or the environment: a flag's value is visible in process
  listings until `orlyi` scrubs it from its command line just after startup.
  `orlyi` never logs a token. The startup log says which listeners require one
  (`client listener bound to 127.0.0.1:19380; token required`).
- Comparison takes the same time wherever a presented token first differs.
- **WebSocket.** The first message on the connection is `{"auth": "<token>"}`
  (a JSON object with exactly that key). The reply is `{"status": "ok",
  "result": null}`, and statements follow as usual. A first message that is
  anything else, or carries a different token, gets `"status": "unauthorized"`
  and the server closes the connection (close code 1008), so nothing sent
  after it runs; a connection that hasn't authenticated within 10 seconds is
  closed too. A first message is used rather than a header because browsers
  can't set headers on a WebSocket, and a failed HTTP upgrade reaches browser
  code only as an anonymous close, while this reply is a status a client can
  match. Keep in mind that a token in browser code is visible to whoever loads
  the page; for a browser app, put an application that authenticates users in
  front of `orlyi`.
- **Binary protocol** (`orly/protocol.h`). Before the session request the
  client sends `THandshake<TAuth>`: the 11-byte header with request kind `'A'`,
  the token's length as a big-endian `uint16`, then the token. The server
  replies one byte, `'A'` (accepted: send the session request next) or `'R'`
  (refused; it hangs up). A session request with no accepted `TAuth` before it
  is refused and the server hangs up: a new-session request gets the nil
  session id, an old-session request gets result `'U'`. Health checks need no
  token. The C++ client (`orly/client`, `orly_client`) presents
  `ORLY_AUTH_TOKEN_FILE` or `ORLY_AUTH_TOKEN`.
- **Replication.** A master with a replication token sends the 8 bytes
  `ORLYAUTH` to a slave as soon as it connects. The slave answers `ORLYAUTH`,
  the token's length as a big-endian `uint16`, and the token; the master
  replies `'A'` or `'R'`. On a mismatch both sides log it (master:
  `replication: refused a slave from 10.0.0.2:41234: the slave presented a
  different replication token`; slave: `replication: cannot join master
  10.0.0.1:19381: the master refused this slave's replication token`), the
  slave exits, and the master keeps listening for another slave. A slave with
  no token can't answer: the master logs that and refuses it after 10 seconds.
- **No token, no change.** Without a token neither side sends any of the
  above, and the bytes on the wire are the same as before #710. A server
  without a token answers a WebSocket auth message as the statement it doesn't
  parse (`"status": "exception"`), and the clients here take any reply but
  `unauthorized` to mean the server needs no token and carry on. Likewise a
  slave with a token joins a master without one, logging that it wasn't asked.
  That lets clients and slaves get the token before the server requires it.

**Turning it on.** Every client and every replica needs the token, or it is
refused as soon as the server requires one:

1. Generate a token and give it to every client (`ORLY_AUTH_TOKEN_FILE` or
   `ORLY_AUTH_TOKEN` for the Python, Go and TypeScript clients, the MCP server,
   the REPL and `orly_client`; or the clients' `token` option) and to every
   replica. Clients that have it keep working against the server as it is.
2. Restart slaves with the token (or `--replication_token_file`), then the
   master. A slave started with the token joins a master without one.
3. Restart the master (or solo server) with `--auth_token_file`. From then on a
   client or slave without the token is refused, and logged at `LOG_WARNING`
   (clients) or `LOG_ERR` (slaves).

Taking it off is the same in reverse: restart the server without a token, and
clients that still send one keep working.

### TLS via a reverse proxy

`orlyi` doesn't speak TLS. To encrypt traffic, and with it the token, keep
`orlyi` on loopback (the default) and put a TLS-terminating proxy on the same
host. With [Caddy](https://caddyserver.com), the WebSocket needs nothing
beyond a site block; Caddy obtains the certificate and passes the WebSocket
upgrade through. The binary protocol is plain TCP, which core Caddy doesn't
proxy, so that part needs a Caddy built with the
[caddy-l4](https://github.com/mholt/caddy-l4) module
(`xcaddy build --with github.com/mholt/caddy-l4`):

```caddyfile
{
	# Binary protocol: TLS on 19443, plain TCP to orlyi's client port.
	layer4 {
		:19443 {
			@orly tls sni orly.example.com
			route @orly {
				tls
				proxy 127.0.0.1:19380
			}
		}
	}
}

# WebSocket: wss://orly.example.com/ to orlyi's WebSocket port. This site
# block is also where Caddy gets the certificate the layer4 route uses.
orly.example.com {
	reverse_proxy 127.0.0.1:8082
}
```

Clients then connect to `wss://orly.example.com/` with the token as usual.
The C++ client speaks plain TCP, so give it a local TLS tunnel, for example
`socat TCP-LISTEN:19380,bind=127.0.0.1,fork,reuseaddr OPENSSL:orly.example.com:19443,cafile=/etc/ssl/certs/ca-certificates.crt`,
and point it at `127.0.0.1:19380`. Replication between hosts can be carried the
same way (a layer4 route to the master's `--slave_port_number`, a tunnel on the
slave's host for `--address_of_master`), or over a private network or VPN.
Only the proxy's port should be reachable from other machines.

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
  A statement of any kind, read or write, also gets `insufficient_memory` when
  no fiber frame came free for it within a second (every one of
  `--max_parallel_frames` busy, #762). It never started, so it too can be
  retried.
- A single write with more entries than half the Update Entry pool's merge
  reserve replies `"status": "write_too_large"`, with the reason in `result`
  (#687). **Unlike `insufficient_memory`, it is not retryable**: promoting a
  write copies it twice, and only the reserve is sure to be free for that, so a
  write this big could never be promoted however long the client waits. Nothing
  was written; split the write into smaller batches and send those. A write's
  entries are roughly the keys it changes, so a batch (`callMany`, `try {pov}
  [...]`) of N single-key calls has about N. The limit is
  `--update_entry_pool_size` × `--memory_reserve_pct` / 100 / 2: with the
  default 200,000-entry pool and 25% reserve, 25,000 entries. When the pool
  isn't given explicitly it is scaled from the memory budget
  (`--memory_budget_mb`, or a container's memory limit, #679), so a small
  container has a much smaller limit, though `orlyi` won't start on a budget
  that can't admit a 1,000-entry write. `orlyi` logs the pool sizes it chose at
  startup (`memory plan sizes:`). Raising `--memory_reserve_pct` raises the limit
  but leaves less of the pool for writes. `--memory_reserve_pct=0` turns the
  check off along with the rest of memory admission. Over the binary protocol
  the same refusal is an error whose message starts with `write too large`.
- A method call that walks more rows, or builds more result memory, than the
  per-read budget replies `"status": "read_too_large"`, with the reason in
  `result` (#694). Like `write_too_large` it is **not retryable as sent**: the
  same call would pass the same budget again. Read a narrower range, or page
  through it with a key bound. A row is each key a range read (`keys ... @`)
  visits and each point read (`*<[...]>`), hit or miss; a key's history folded
  on read counts once. Result memory is what the call builds in its arena,
  which is its result once encoded. The limits are `--read_budget_rows` and
  `--read_budget_mb`; by default the memory limit is a sixteenth of the memory
  budget (`--memory_budget_mb`, or a container's memory limit, #679), at least
  16 MiB, and the row limit is that memory over 256 bytes: 256 MiB and
  1,048,576 rows at 4 GiB, 64 MiB and 262,144 rows at 1 GiB. `orlyi` logs them at
  startup (`read budget:`). A batch (`callMany`, `try {pov} [...]`) shares one
  budget across its calls. `--read_budget_mb=0` turns both limits off, unless
  `--read_budget_rows` is given as well; 0 turns either off on its own. Values
  computed without reading any rows (`[0..n]`) count only once they are part
  of the result. Over the binary protocol the refusal is an error whose message
  starts with `read too large`.
- `compile` on a server started without `--allow_remote_compile` replies
  `"status": "remote_compile_disabled"` (#705). It is a configuration answer,
  not a transient one: retrying won't change it. See "Security and trust
  model" above.
- `unauthorized` (#710): the server requires a token and the connection's
  first message wasn't `{"auth": "<token>"}` with the right one. The server
  closes the connection after this reply; nothing else on it ran. Not
  retryable as sent: fix the token. See "Shared secret" above.

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
| Compile package | `compile "<orlyscript source>";` | `{"name": ..., "version": ...}`; refused unless `--allow_remote_compile` |
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
    the method plus all N arg sets under index-prefixed names, and the call count
    as `$calls`). When the batch's calls evaluated an `if`, Tetris replays every
    call, in order and with its own args, to test the batch's predicate results
    before promoting it (#751).
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
- **An `ok` is not a durability promise** (#730). It means the write committed,
  in memory, to the POV it named, on a safe POV as on a fast one. It is on disk
  once Tetris has promoted it to the global POV and the global POV's memory merge
  has written it, and nothing on this protocol says when that has happened. A
  crash loses only the newest writes: a prefix of each POV's commit order
  survives; see [`durability.md`](durability.md).
- **POVs are ephemeral across restarts** (#439). Updates promoted to the global
  POV and written to disk survive; a private/shared POV's own un-promoted state
  does not. After a
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
   wrong". `write_too_large` and `read_too_large` mean the opposite: never
   retry them as sent; split the write, or read less. `remote_compile_disabled`
   means the server doesn't take `compile` at all, and `unauthorized` that the
   connection's token was missing or wrong.

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
