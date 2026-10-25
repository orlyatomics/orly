# Security

By default Orly has **no authentication or authorization**. Any client that can reach
`orlyi` can read and write any data its installed packages reach, and install
or uninstall packages; `set user id` is attribution a client asserts, not an
identity. Traffic isn't encrypted. Run `orlyi` only where every client that
can reach it is trusted: on one host, on a private network, or behind an
application that authenticates its users and talks to `orlyi` for them
([#705](https://github.com/orlyatomics/orly/issues/705)).

- **Listeners bind loopback by default.** On a bare host, the client
  (19380), WebSocket (8082) and reporting (19388) listeners bind `127.0.0.1`.
  `--bind_address=0.0.0.0` listens on every interface, or pass one
  interface's address. `orlyi --log_info` logs each bound address at startup.
- **Replication belongs on a private network.** The replication listener
  (`--replica_port_number`, 19381) binds every interface by default, so a replica on
  another host can reach it. Without a replication token it is just as
  unauthenticated and carries all the data: set `--replica_bind_address` to the
  private network's address.
- **The Docker image binds every interface inside the container**, because
  `docker run -p` needs that. The `-p` mapping then decides who can connect;
  publish to `127.0.0.1` as above unless clients on other machines need it.
- **`compile` over WebSocket is off.** That statement builds the source it is
  sent with the server's C++ compiler, and `install` then loads it into the
  server process, so it lets a client run code inside `orlyi`. It is refused
  with `"status": "remote_compile_disabled"` unless `orlyi` is started with
  `--allow_remote_compile`, both on a bare host and in the image. Compile
  packages with `orlyc` and `install` them instead, as every client and
  example here does.
- **An optional shared secret** ([#710](https://github.com/orlyatomics/orly/issues/710)).
  Start `orlyi` with `--auth_token_file=<path>` (or `ORLY_AUTH_TOKEN_FILE`, or
  the token in `ORLY_AUTH_TOKEN`) and every WebSocket and binary-protocol
  connection must present that token before any statement runs; one without
  it, or with a wrong one, is refused with `"status": "unauthorized"`. A replica
  must present the replication token (`--replication_token_file`, by default
  the same token) to join a primary. The Python, Go and TypeScript clients, the
  MCP server, the REPL and `orly_client` read `ORLY_AUTH_TOKEN_FILE` or
  `ORLY_AUTH_TOKEN`. Off by default, and with no token nothing changes on the
  wire. Turning it on means giving the token to **every client and replica**
  first; `docs/PROTOCOL.md` has the order. It is one secret for all clients,
  not user accounts.
- **TLS goes in a reverse proxy.** `orlyi` doesn't speak TLS; keep it on
  loopback and put a proxy such as Caddy in front of it. `docs/PROTOCOL.md`
  has a Caddyfile for the WebSocket and the binary port.

[`docs/PROTOCOL.md`](docs/PROTOCOL.md) has the details.
