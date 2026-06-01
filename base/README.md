# base/

The foundation layer. Everything Orly is built on top of, none of it
Orly-specific. If you stripped out [`orly/`](../orly/) you could still
use `base/` as a small standalone C++ stdlib supplement.

Two parts:

1. **Top-level headers** (`base/*.h`, `*.cc`) -- the original `base/`
   namespace. POSIX wrappers, common types, build infrastructure.
2. **Subdirectories** -- 14 grouped utility libraries that used to be
   top-level directories before [#39](https://github.com/orlyatomics/orly/pull/39).

For project-level orientation see the [top-level README](../README.md).

## Top-level: the `Base` namespace

About 150 headers + sources at the top of `base/`. These are loosely
grouped:

### POSIX / system wrappers

| Header | What it wraps |
|---|---|
| `fd.h` | `TFd` -- RAII file descriptor + `TFd::Pipe()` / `SocketPair()` factories |
| `pump.h` | `TPump` -- epoll-based pipe manager for subprocess I/O |
| `subprocess.h` | `TSubprocess` -- structured subprocess with TPump-managed pipes |
| `rigid_subprocess.h` | Simpler subprocess for synchronous wait-and-collect |
| `epoll.h` | `TEpoll` -- RAII epoll fd + wait helpers |
| `timer_fd.h` | RAII `timerfd_create` wrapper |
| `event_semaphore.h` | `eventfd`-backed semaphore |
| `mlock.h` | `mlock()` helpers (with awareness of the 64KB Ubuntu 24.04 RLIMIT default) |
| `tmp_dir.h` / `tmp_dir_maker.h` / `tmp_file.h` / `tmp_copy_to_file.h` | RAII temp paths |
| `dir_iter.h` / `dir_walker.h` | Directory traversal |
| `path.h` | Path manipulation |
| `glob.h` | Shell-glob wrapper |
| `backtrace.h` | `backtrace_symbols` wrapper for `SetBacktraceOnTerminate()` |
| `chrono.h` / `cpu_clock.h` / `timer.h` | Wall-clock + CPU-time helpers |
| `usage_meter.h` | `getrusage()` wrapper |

### Error handling, debug, assertions

| Header | What it is |
|---|---|
| `thrower.h` | `THROW_ERROR(T) << ...` / `THROW << ...` fluent exception builder |
| `code_location.h` | `TCodeLocation` + the `HERE` macro |
| `source_root.h` | `GetSrcRoot()` for source-relative paths in error messages |
| `assert_true.h` | `AssertTrue<T>(T)` pass-through |
| `no_default_case.h` | Macro for "unreachable default branch" |
| `not_implemented.h` / `impossible_error.h` / `unreachable.h` | Specific exception types |
| `debug_log.h` / `log.h` | Logging helpers |
| `demangle.h` | `abi::__cxa_demangle` wrapper |

### CLI and config

| Header | What it is |
|---|---|
| `cmd.h` | `TCmd` -- command-line parser (used by every binary) |
| `repl.h` | `linenoise` wrapper for the interactive shells |
| `json.h` | `TJson` -- in-memory JSON value + parser/serializer |

### Containers and primitives

| Header | What it is |
|---|---|
| `opt.h` | `TOpt<T>` -- optional type (predates `std::optional`) |
| `array_view.h` | `TArrayView<T>` -- non-owning view |
| `slice.h` | `TSlice` -- byte-range view |
| `piece.h` | `TPiece` -- string view |
| `ordered_array.h` | Sorted-vector wrapper |
| `peekable.h` | Iterator adapter with lookahead |
| `iter.h` | Generic iterator helpers |
| `hash.h` | `boost::hash_combine`-style helpers |
| `murmur.h` | MurmurHash3 |
| `interner.h` | String interning |
| `mini_cache.h` | Small fixed-capacity LRU |
| `ref_counted.h` | Intrusive ref counting |
| `mem_aligned_ptr.h` | Aligned-alloc'd ptrs |
| `uuid.h` | `TUuid` |
| `var_int.h` | Variable-length integer encoding |

### Concurrency

| Header | What it is |
|---|---|
| `scheduler.h` | Thread pool |
| `bg_generator.h` | Background-thread coroutine-ish generator |
| `latch.h` / `event_counter.h` | Sync primitives |
| `spin_lock.h` | Spinlock |
| `shutting_down.h` | Global shutdown flag |
| `thread_local_global_pool.h` / `thread_local_registered_pool.h` | Thread-local memory pools |

### Numerics, math

| Header | What it is |
|---|---|
| `sigma_calc.h` | Streaming variance / stddev |
| `convert.h` | Numeric conversions with overflow checks |
| `stdfloat.h` | Floating-point helpers |

### Misc

| Header | What it is |
|---|---|
| `as_str.h` | Stream-to-string helper for ad-hoc formatting |
| `class_traits.h` | `NO_COPY` / `MOVE_ONLY` / etc. macros |
| `apply.h` | `std::apply` predecessor |
| `identity.h` / `zero.h` / `false.h` / `likely.h` / `paste_tokens.h` | Single-purpose helpers |
| `split.h` | String splitting |
| `pos.h` | Source-position type |
| `regex_matcher.h` | `std::regex` wrapper |
| `time_maps.h` | Time-bucket containers |
| `booster.h` | Performance instrumentation hook |
| `unique_ident.h` / `unique_token.h` | Type-safe ID helpers |

## Subdirectories

These were 14 top-level directories before #39, consolidated under
`base/` so they stop crowding the project root.

| Dir | Role |
|---|---|
| [`util/`](util/) | Things we wish were in `<algorithm>`/`<string>`/the STL: looping read/write helpers, path manipulation, container-API gap fills |
| [`mpl/`](mpl/) | Template-metaprogramming library (`TypeList`, `TypeSet`, `Contains<>`, etc.) |
| [`io/`](io/) | Streaming I/O abstractions: `TBinaryStream`, `TChunkAndPool`, recorder/player |
| [`strm/`](strm/) | Producer/consumer pipeline framework -- a different model than `io/`, used by jhm and the on-disk format. See its own [README](strm/README) for the pipeline + agent model |
| [`socket/`](socket/) | UNIX-socket wrappers (`TAddress`, `TFactory`, `TOption`) |
| [`signal/`](signal/) | Signal handlers / mask helpers, isolated to keep thread/signal interaction tidy |
| [`multi_event/`](multi_event/) | "Wait for N events" coordination (currently only used by the SPA engine) |
| [`gz/`](gz/) | gzip I/O wrappers |
| [`utf8/`](utf8/) | UTF-8 piece/position/regex helpers |
| [`inv_con/`](inv_con/) | Invasive containers (intrusive lists, atomic ordered/unordered lists) |
| [`visitor/`](visitor/) | Generic visitor pattern (`TVariant`, `TPass`). Heavily used by the orly compiler |
| [`rpc/`](rpc/) | Full-duplex remote procedure call framework |
| [`web/`](web/) | Generic web utilities (`url_decode`, `daemonize`, `counter`, `half_latch`). Named to disambiguate from [`orly/server/`](../orly/server/) (the daemon) |
| [`test/`](test/) | The Orly unit-test framework (`EXPECT_EQ`, `FIXTURE`, runner) |
