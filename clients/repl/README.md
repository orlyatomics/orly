# orly-repl

An interactive orlyscript REPL against a running `orlyi` ([#535](https://github.com/orlyatomics/orly/issues/535)). Type an expression, see its value; define a function, call it in the next entry; write through a POV and read it back — without hand-writing package files.

```
$ orly-repl --package-dir /path/to/packages
orly-repl: connected to ws://127.0.0.1:8082/
pov 3f0e... (private, new); :help for help
orly> 1 + 2;
3
orly> double = (x * 2) where { x = given::(int); };
defined double
orly> double(.x: 21);
42
orly> (true) effecting { new <['k', 1]> <- 7; };
true
orly> *<['k', 1]>::(int);
7
```

## How it works

There is no eval statement in the wire protocol, so the REPL drives the same loop a package author would, invisibly: every entry is folded into a synthetic package (`repl_<pid>`), compiled with `orlyc`, copied into the server's package directory, and installed at the next version number (a newer install re-points the unversioned name; old versions are deliberately never uninstalled, since uninstalling any version unregisters the whole name). Expressions are wrapped as a generated nullary method and called against the session's POV. Compile errors print the orlyc/g++ diagnostics and leave your definitions untouched.

The consequences of that design:

- **`orlyc` must be runnable from the REPL's host** (`ORLYC` / `--orlyc`; default `orlyc` on PATH). Multi-word commands work, e.g. `ORLYC="docker exec orly orlyc"`.
- **The server's package directory must be writable at the same path the server sees it** (`ORLY_PACKAGE_DIR` / `--package-dir`).
- Statements end with `;`; entries may span lines (the prompt switches to `  ... ` until brackets balance and a `;` closes the statement).

## Configuration

| Flag | Env | Default | Meaning |
| --- | --- | --- | --- |
| `--url` | `ORLY_URL` | `ws://127.0.0.1:8082/` | orlyi WebSocket url |
| `--package-dir` | `ORLY_PACKAGE_DIR` | (required) | the server's package directory |
| `--orlyc` | `ORLYC` | `orlyc` | orlyc command (may be multi-word) |
| `--pov <id>` | | | join an existing pov instead of creating one |
| `--shared` | | private | create a shared pov (updates promote globally) |

Commands at the prompt: `:help`, `:defs`, `:drop <name>`, `:src` (show the synthetic package source), `:pov`, `:quit`.

## Against the docker image

The image ships `orlyc` but the compile/install loop needs a package directory both sides can see, so start the container with one mounted at an identical path, and run `orlyc` inside the container:

```sh
mkdir -p /tmp/orly-repl/packages && touch /tmp/orly-repl/packages/__orly__
docker run -d --name orly -p 8082:8082 -v /tmp/orly-repl:/tmp/orly-repl \
  ghcr.io/orlyatomics/orly --package_dir=/tmp/orly-repl/packages
npx tsc && node dist/index.js \
  --package-dir /tmp/orly-repl/packages \
  --orlyc "docker exec orly orlyc"
```

(Compiled `.so` paths land in the shared mount, so the container-side `orlyc` and the host-side REPL agree on where everything is.)

## Building and joining agents

`orly-repl --pov <id> ...` attaches to a POV your agents (e.g. via [`clients/mcp`](../mcp)) are already writing — the REPL then reads and writes the same live state, which makes it a debugger for agent shared memory.

Build: `npm install && npx tsc`. Smoke test (needs a built tree, see the repo README): `npm run smoke`.
