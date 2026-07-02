# orly/

The Orly database. Everything that makes Orly *Orly* lives under this
directory: the storage engine, the Orlyscript compiler, the runtime,
the server daemon, and the various wire protocols.

For project-level orientation (build, test, install), see the
[top-level README](../README.md). For a hands-on walkthrough of
compiling and running a package, see [`docs/walkthrough.md`](../docs/walkthrough.md).

## Layout

The directory has a few top-level entry-point files (binary mains and
broadly-shared types) and many subdirectories grouped by concern.

### Binary entry points

| File | Builds | Purpose |
|---|---|---|
| `orlyc.cc` | `orly/orlyc` | Orlyscript compiler -- takes a `.orly` source, emits a `.so` package |
| `core_import.cc` | `orly/core_import` | Bulk data importer (`.bin` files into a package) |
| `compile_import.cc` | -- | Helper used by `core_import` |

The other production binary lives in a subdirectory:
[`server/orlyi`](server/) (the database daemon).

### Broadly-shared headers at the top level

| File | What it is |
|---|---|
| `closure.h` / `.cc` | `TClosure`: a method name + bound arguments, the unit of call into the database |
| `compiler.h` / `.cc` | Public entry point for the Orlyscript-to-`.so` pipeline; called by `orlyc` and by `server/ws.cc`'s `compile` statement |
| `context_base.h` | Base type for compilation contexts (errors, position tracking) |
| `desc.h` | `TDesc<T>`: wrapper providing descending ordering -- used in indexes that need reverse sort |
| `error.h` | Base error class for the Orly subsystem |
| `expr.h` | Forward declarations for the Orlyscript expression tree (real types in `expr/`) |
| `key_generator.h` | UUID-based key generation utilities |
| `method_request.{h,cc}` | `TMethodRequest`: pov + package + method + args, what the client sends |
| `method_result.{h,cc}` | `TMethodResult`: the value returned + the arena that owns it |
| `pos_range.h` | Source-position range type used in error reporting |
| `protocol.{h,cc}` | Native (non-WS) wire protocol types |
| `rt.h` | Umbrella include for the Orlyscript runtime (`rt/`) |
| `orly.nycr` | Top-level Orlyscript grammar definition, fed to `tools/nycr` |
| `orly.package.cst.h` | Generated CST header for the grammar above |
| `indy_lives.orly` | Smoke-test Orlyscript package, kept here as a sample |

### Subdirectories

#### Storage engine

| Dir | What it does |
|---|---|
| [`indy/`](indy/) | The Indy MVCC store: Points of View, the Flux Capacitor, the on-disk page/block format. The actual database engine |
| [`durable/`](durable/) | Durable-handle primitives (`TDurable<T>`, manager) used by sessions and povs |
| [`atom/`](atom/) | Atomic value primitives: `TCore`, `TKit2`, packed value layouts |
| [`sabot/`](sabot/) | "State abstraction over binary": runtime type introspection over packed values. Used to walk a binary blob and discover its shape |
| [`var/`](var/) | `TVar`: a discriminated-union value type used at the orlyscript boundary. Converters to/from JSON, sabot, native C++ |
| [`native/`](native/) | Native (C++) representations of Orlyscript primitive types |

#### Compiler

| Dir | What it does |
|---|---|
| [`expr/`](expr/) | Orlyscript expression AST nodes (the e in `5 + x`, `[1, 2]`, `that.x`) |
| [`symbol/`](symbol/) | Symbol table -- resolved names, scope, type binding |
| [`synth/`](synth/) | Concrete-syntax-tree to semantic-symbol synthesis. The "parse tree → typed AST" pass |
| [`type/`](type/) | The Orlyscript type system: type constructors, unification, subtyping |
| [`code_gen/`](code_gen/) | C++ code generation from the typed AST -- produces the `.cc` files that get compiled into the package `.so` |
| [`package/`](package/) | Package management: versioned names, on-disk package directory layout, installed-package registry |
| [`rt/`](rt/) | Orlyscript runtime library: `TStr`, `TList`, `TDict`, `TSet`, all the built-ins generated code calls into |
| [`csv_to_bin/`](csv_to_bin/) | Generator that produces the `data/*.cc` importer binaries from CSV schemas |

#### Servers and protocols

| Dir | What it does |
|---|---|
| [`server/`](server/) | The `orlyi` daemon: session manager, native protocol, WebSocket frontend (`ws.cc`) |
| [`balancer/`](balancer/) | TCP-level load balancer that fronts an `orlyi` cluster |
| [`client/`](client/) | The `orly_client` interactive shell + its statement grammar (`program/`) |
| [`notification/`](notification/) | Pub/sub channel between server components |

#### Data and tests

| Dir | What it does |
|---|---|
| [`data/`](data/) | Sample Orlyscript packages for the bundled datasets (twitter, social_graph, etc.) plus their compile recipes |
| [`perf/`](perf/) | Performance harness scaffolding |

For the Orlyscript regression test suite see [`tests/lang_tests/`](../tests/lang_tests/) at the project root.
