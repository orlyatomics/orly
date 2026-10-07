/**
 * Orly client for the WebSocket + JSON protocol.
 *
 * A typed client for talking to a running `orlyi` over WebSocket. It owns the
 * connection + session lifecycle, builds orlyscript statement strings safely
 * (escaping, argument literals, POV threading), and resolves the parsed JSON
 * result. See `docs/PROTOCOL.md` in the Orly repo for the protocol itself.
 *
 * Works in the browser (uses the global `WebSocket`) and in Node (dynamically
 * imports the `ws` package).
 *
 * ```ts
 * import { connect } from "@orlyatomics/orly";  // in-repo: from "orly" (alias)
 *
 * const c = await connect();               // opens a WebSocket
 * await c.newSession();
 * await c.install("mypkg", 0);
 * const pov = await c.newPov();             // "new safe shared pov;"
 * await c.call(pov, "mypkg", "put", { k: 1, s: "hi" });
 * console.log(await c.call(pov, "mypkg", "get", { k: 1 }));
 * await c.exit();
 * ```
 *
 * JSON marshaling quirks the engine returns (see `docs/PROTOCOL.md`): integers
 * come back as numbers (JSON has no int/float split), sets as unordered arrays,
 * variants as `{ Tag: <payload> }`. `call` resolves the parsed value as-is.
 */

export const DEFAULT_URL = "ws://127.0.0.1:8082/";

// setTimeout is provided by both the browser and Node at runtime; declare it
// minimally so this isomorphic client type-checks under its lean lib config
// (es2020, no DOM / no @types/node) without pulling those in.
declare function setTimeout(handler: () => void, ms: number): unknown;

// A just-started or heavily loaded orlyi can briefly refuse the connection or
// time out the WebSocket handshake before it is ready. connect() retries that
// window with exponential backoff: DEFAULT_BACKOFF_MS, doubling each attempt.
export const DEFAULT_RETRIES = 5;
export const DEFAULT_BACKOFF_MS = 250;

/** Thrown when the server replies with a non-`ok` status. */
export class OrlyError extends Error {
  constructor(
    public readonly statement: string,
    public readonly reply: unknown,
  ) {
    super(`${JSON.stringify(statement)} -> ${JSON.stringify(reply)}`);
    this.name = "OrlyError";
  }
}

/** Thrown when the server refuses a write because it is low on disk space
 *  (`"status": "insufficient_storage"`). Nothing was written, reads still work, and the
 *  write can be retried once space is freed. */
export class InsufficientStorageError extends OrlyError {
  constructor(statement: string, reply: unknown) {
    super(statement, reply);
    this.name = "InsufficientStorageError";
  }
}

/** Thrown when the server refuses a write because its update pools are down to the reserve kept
 *  for merges (`"status": "insufficient_memory"`). Nothing was written, reads still work, and
 *  the write can be retried; writes are accepted again once the merges have freed the pools,
 *  usually within seconds. */
export class InsufficientMemoryError extends OrlyError {
  constructor(statement: string, reply: unknown) {
    super(statement, reply);
    this.name = "InsufficientMemoryError";
  }
}

/** Thrown when a single write holds more entries than half the server's Update Entry pool's
 *  merge reserve (`"status": "write_too_large"`), so it could never be promoted. Nothing was
 *  written. Unlike `InsufficientMemoryError` it is NOT retryable: split the batch into smaller
 *  ones. */
export class WriteTooLargeError extends OrlyError {
  constructor(statement: string, reply: unknown) {
    super(statement, reply);
    this.name = "WriteTooLargeError";
  }
}

/** Thrown when a call walks more rows, or builds more result memory, than the server's per-read
 *  budget (`"status": "read_too_large"`; `--read_budget_rows`, `--read_budget_mb`). Like
 *  `WriteTooLargeError` it is NOT retryable as sent: read a narrower range. */
export class ReadTooLargeError extends OrlyError {
  constructor(statement: string, reply: unknown) {
    super(statement, reply);
    this.name = "ReadTooLargeError";
  }
}

/** Thrown when a `compile` statement reaches a server started without `--allow_remote_compile`
 *  (`"status": "remote_compile_disabled"`, #705). Compile packages with `orlyc` and install them
 *  instead. */
export class RemoteCompileDisabledError extends OrlyError {
  constructor(statement: string, reply: unknown) {
    super(statement, reply);
    this.name = "RemoteCompileDisabledError";
  }
}

/** Thrown by {@link connect} when the server requires a token and this client presented none, or
 *  the wrong one (`"status": "unauthorized"`, #710). The server has closed the connection; no
 *  statement ran. Not retryable as sent: fix the token. */
export class UnauthorizedError extends OrlyError {
  constructor(statement: string, reply: unknown) {
    super(statement, reply);
    this.name = "UnauthorizedError";
  }
}

/** Wrap a string to inject it into a statement as raw orlyscript, un-encoded. */
export class Raw {
  constructor(public readonly text: string) {}
}
/** `raw("now()")` -> the orlyscript expression `now()`, un-encoded. */
export const raw = (text: string): Raw => new Raw(text);

/** Encode its items as an orlyscript set literal `{a, b, ...}`. */
export class OrlySet {
  readonly items: unknown[];
  constructor(items: Iterable<unknown>) {
    this.items = [...items];
  }
}
/** `set([1, 2])` -> the orlyscript set literal `{1, 2}`. */
export const set = (items: Iterable<unknown>): OrlySet => new OrlySet(items);

/** Encode its items as an orlyscript address (key) literal `<[a, b, ...]>`. */
export class OrlyAddr {
  readonly items: unknown[];
  constructor(items: Iterable<unknown>) {
    this.items = [...items];
  }
}
/** `addr(["edge", 1])` -> the orlyscript key literal `<["edge", 1]>`. */
export const addr = (items: Iterable<unknown>): OrlyAddr => new OrlyAddr(items);

export type Args = Record<string, unknown>;

// -- POV review (#746) ----------------------------------------------------

/** How a POV treats conflicts: an overwrite or delete of a key its parent changed after the fork. */
export type ConflictMode = "none" | "report" | "refuse";

/** One key a POV changed relative to its parent. `before` is the parent's value and `after` the
 *  POV's (null when absent); a `delta` is a run of one commutative operator (`op`: "add", "or",
 *  "union", ...) that adds up to `delta`. */
export interface PovChange {
  key: unknown[];
  kind: "added" | "changed" | "removed" | "delta";
  before: unknown;
  after: unknown;
  op?: string;
  delta?: unknown;
}

/** A page of a diff: `next` (and its exact orlyscript form, `next_literal`) is null on the last page. */
export interface PovDiff {
  changes: PovChange[];
  next: unknown[] | null;
  next_literal: string | null;
  /** The POV's unpromoted updates the diff covers. */
  updates: number;
}

/** A key range (`start` inclusive, `stop` exclusive), the previous page's `next`, and a page size
 *  (default 100, at most 10,000). Keys are arrays, sent as key literals. */
export interface DiffOptions {
  start?: unknown[] | OrlyAddr | Raw;
  stop?: unknown[] | OrlyAddr | Raw;
  after?: unknown[] | OrlyAddr | Raw;
  limit?: number;
}

export interface PovConflict {
  /** Numbered from 1 in the order found; absent for a key the POV is blocked on. */
  number?: number;
  key: unknown[];
  op: "put" | "delete";
  /** Refusing mode: the parent changed the key between Tetris's test and the promotion. */
  raced?: boolean;
}

export interface PovDiscard {
  discarded_updates: number;
  discarded_entries: number;
}

export interface PovPromote {
  status: "promoting" | "refused";
  pending: number;
  /** The last conflict's number before this promotion; its conflicts are numbered after it. */
  mark: number;
  /** When refused: the keys that would conflict. */
  conflicts: PovConflict[];
}

export interface PovReview {
  conflict_mode: ConflictMode;
  status: "normal" | "paused" | "failed";
  pending: number;
  pending_entries: number;
  blocked: boolean;
  blocked_on: PovConflict[];
  conflicts: PovConflict[];
  conflict_count: number;
  changed_keys: number;
  overflowed: boolean;
}

/** What {@link Client.promote} saw: "promoted" once nothing is pending; "refused" (nothing
 *  changed); "blocked" (refusing mode: Tetris holds the POV back on `blocked_on`); "failed";
 *  "paused" (someone paused it meanwhile); or "timeout". `conflicts` are those found during this
 *  promotion. */
export interface PromoteResult {
  status: "promoted" | "refused" | "blocked" | "failed" | "paused" | "timeout";
  pending: number;
  conflicts: PovConflict[];
  blocked_on: PovConflict[];
}

function keyLit(key: unknown[] | OrlyAddr | Raw): string {
  return key instanceof Raw || key instanceof OrlyAddr ? lit(key) : lit(new OrlyAddr(key));
}

/**
 * Encode a JS value as an orlyscript literal:
 * - `Raw`            -> its text, verbatim
 * - `boolean`        -> `true` / `false`
 * - `number`/`bigint`-> decimal
 * - `string`         -> a quoted, escaped string literal
 * - array            -> `[a, b, ...]`
 * - `OrlySet`        -> `{a, b, ...}`
 * - `OrlyAddr`       -> `<[a, b, ...]>`
 * - object           -> a record `<{.k: v, ...}>` (empty: `<{}>`);
 *   field names are identifiers, including keywords such as `id` and `to`
 */
export function lit(value: unknown): string {
  if (value instanceof Raw) return value.text;
  if (value instanceof OrlySet) return "{" + value.items.map(lit).join(", ") + "}";
  if (value instanceof OrlyAddr) return "<[" + value.items.map(lit).join(", ") + "]>";
  if (value === null || value === undefined) {
    throw new TypeError("orly: cannot encode null/undefined as a literal");
  }
  switch (typeof value) {
    case "boolean":
      return value ? "true" : "false";
    case "number":
      if (!Number.isFinite(value)) throw new TypeError(`orly: cannot encode ${value}`);
      return String(value);
    case "bigint":
      return value.toString();
    case "string":
      return quote(value);
    case "object": {
      if (Array.isArray(value)) return "[" + value.map(lit).join(", ") + "]";
      const m = value as Record<string, unknown>;
      const parts = Object.keys(m).map((k) => {
        if (!/^[_a-zA-Z]/.test(k) || /[^_a-zA-Z0-9]/.test(k)) {
          throw new TypeError(`orly: invalid record field name ${JSON.stringify(k)}; rename it to an identifier (letters, digits, underscore; no leading digit)`);
        }
        return `.${k}: ${lit(m[k])}`;
      });
      return "<{" + parts.join(", ") + "}>";
    }
    default:
      throw new TypeError(`orly: cannot encode ${typeof value} as a literal`);
  }
}

/* The lexer refuses raw control characters, so those are written as \n, \r, \t or \xNN. */
function quote(s: string): string {
  const escaped = s
    .replace(/[\\"]/g, "\\$&")
    .replace(/[\x00-\x1f\x7f]/g, (c) => {
      if (c === "\n") return "\\n";
      if (c === "\r") return "\\r";
      if (c === "\t") return "\\t";
      return "\\x" + c.charCodeAt(0).toString(16).padStart(2, "0");
    });
  return '"' + escaped + '"';
}

/** The minimal browser-WebSocket surface this client relies on. */
interface SocketLike {
  send(data: string): void;
  close(): void;
  addEventListener(type: string, listener: (ev: any) => void): void;
  readyState: number;
}

/** A connection to a running `orlyi` (one WebSocket, one session). */
export class Client {
  private pending: Array<{ stmt: string; resolve: (v: unknown) => void; reject: (e: unknown) => void }> = [];

  constructor(private readonly ws: SocketLike) {
    ws.addEventListener("message", (ev: any) => this.onMessage(ev));
    const fail = (ev: any) => this.failAll(ev);
    ws.addEventListener("error", fail);
    ws.addEventListener("close", fail);
  }

  private onMessage(ev: any): void {
    const data: string = typeof ev.data === "string" ? ev.data : String(ev.data);
    const p = this.pending.shift();
    if (!p) return;
    let reply: any;
    try {
      reply = JSON.parse(data);
    } catch (e) {
      p.reject(e);
      return;
    }
    if (reply == null || reply.status !== "ok") {
      p.reject(reply?.status === "insufficient_storage"
        ? new InsufficientStorageError(p.stmt, reply)
        : reply?.status === "insufficient_memory"
        ? new InsufficientMemoryError(p.stmt, reply)
        : reply?.status === "write_too_large"
        ? new WriteTooLargeError(p.stmt, reply)
        : reply?.status === "read_too_large"
        ? new ReadTooLargeError(p.stmt, reply)
        : reply?.status === "remote_compile_disabled"
        ? new RemoteCompileDisabledError(p.stmt, reply)
        : reply?.status === "unauthorized"
        ? new UnauthorizedError(p.stmt, reply)
        : new OrlyError(p.stmt, reply));
      return;
    }
    p.resolve(reply.result);
  }

  private failAll(ev: any): void {
    const err = ev instanceof Error ? ev : new Error("orly: websocket closed");
    const waiting = this.pending;
    this.pending = [];
    for (const p of waiting) p.reject(err);
  }

  /** Send one statement; resolve its `result`, or reject with `OrlyError`. */
  send(stmt: string): Promise<unknown> {
    return new Promise((resolve, reject) => {
      this.pending.push({ stmt, resolve, reject });
      this.ws.send(stmt);
    });
  }

  /** Present the server's shared secret (#710): the first message, `{"auth": "<token>"}`.
   *  {@link connect} calls this when given a token; the token never appears in an error. A server
   *  with a token answers only `ok` or `unauthorized`. One started without a token answers with an
   *  error status (it tries to parse the message as a statement) and the connection carries on
   *  unauthenticated, so clients can get the token before the server starts requiring it. */
  async authenticate(token: string): Promise<void> {
    try {
      await new Promise((resolve, reject) => {
        this.pending.push({ stmt: "<auth>", resolve, reject });
        this.ws.send(JSON.stringify({ auth: token }));
      });
    } catch (e) {
      if (e instanceof OrlyError && !(e instanceof UnauthorizedError)) {
        return;
      }
      throw e;
    }
  }

  async sendString(stmt: string): Promise<string> {
    return (await this.send(stmt)) as string;
  }

  // -- session / package lifecycle --------------------------------------
  newSession(): Promise<string> {
    return this.sendString("new session;");
  }
  install(pkg: string, version: number): Promise<unknown> {
    return this.send(`install ${pkg}.${version};`);
  }
  uninstall(pkg: string, version: number): Promise<unknown> {
    return this.send(`uninstall ${pkg}.${version};`);
  }
  /** Create a POV; resolves its id. Defaults to `new safe shared pov;`.
   *  `safe: false` makes a `fast` POV; `parent` is a POV id, and the new POV
   *  is created `from` it. The grammar has no default guarantee, so one of
   *  `safe`/`fast` is always spelled out (#580). */
  newPov(opts: { safe?: boolean; shared?: boolean; parent?: string; conflicts?: ConflictMode } = {}): Promise<string> {
    const { safe = true, shared = true, parent, conflicts } = opts;
    const parts = ["new", safe ? "safe" : "fast", shared ? "shared" : "private", "pov"];
    if (parent) parts.push(`from {${parent}}`);
    /* `conflicts` tracks conflicts from the fork (#746); see promote(). */
    if (conflicts && conflicts !== "none") parts.push(lit({ conflicts }));
    return this.sendString(parts.join(" ") + ";");
  }

  // -- POV review (#746) ------------------------------------------------
  /** A page of what `pov` changed relative to its parent: its unpromoted writes, in key order. */
  diff(pov: string, opts: DiffOptions = {}): Promise<PovDiff> {
    const parts: string[] = [];
    if (opts.start !== undefined) parts.push(`.start: ${keyLit(opts.start)}`);
    if (opts.stop !== undefined) parts.push(`.stop: ${keyLit(opts.stop)}`);
    if (opts.after !== undefined) parts.push(`.after: ${keyLit(opts.after)}`);
    if (opts.limit !== undefined) parts.push(`.limit: ${lit(opts.limit)}`);
    const options = parts.length ? ` <{${parts.join(", ")}}>` : "";
    return this.send(`diff_pov {${pov}}${options};`) as Promise<PovDiff>;
  }

  /** Every page of `pov`'s diff, each page after the last page's `next` (keyset paging). */
  async *diffPages(pov: string, opts: DiffOptions = {}): AsyncGenerator<PovChange[], void, undefined> {
    let page = await this.diff(pov, opts);
    for (;;) {
      if (page.changes.length) yield page.changes;
      if (page.next_literal === null) return;
      page = await this.diff(pov, { ...opts, after: raw(page.next_literal) });
    }
  }

  /** Throw away `pov`'s unpromoted changes (a private POV of this session's), so it reads as its
   *  parent again. */
  discard(pov: string): Promise<PovDiscard> {
    return this.send(`discard_pov {${pov}};`) as Promise<PovDiscard>;
  }

  /** Ask for `pov`'s changes to be promoted (unpause it), and return at once. In refusing mode it
   *  is tested first, and stays as it was if any change would conflict, unless `force`. */
  requestPromotion(pov: string, opts: { force?: boolean } = {}): Promise<PovPromote> {
    const options = opts.force ? " <{.force: true}>" : "";
    return this.send(`promote_pov {${pov}}${options};`) as Promise<PovPromote>;
  }

  /** `pov`'s promotion progress, and the conflicts numbered after `after`. */
  review(pov: string, opts: { after?: number } = {}): Promise<PovReview> {
    const options = opts.after ? ` <{.after: ${lit(opts.after)}}>` : "";
    return this.send(`review_pov {${pov}}${options};`) as Promise<PovReview>;
  }

  /** Promote `pov`'s changes and wait until nothing is pending, the POV is blocked, failed or
   *  paused, or `timeoutMs` passes; resolves the conflicts found meanwhile. */
  async promote(pov: string, opts: { force?: boolean; timeoutMs?: number; pollMs?: number } = {}): Promise<PromoteResult> {
    const { timeoutMs = 30_000, pollMs = 50 } = opts;
    const started = await this.requestPromotion(pov, { force: opts.force });
    if (started.status === "refused") {
      return { status: "refused", pending: started.pending, conflicts: started.conflicts, blocked_on: [] };
    }
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const review = await this.review(pov, { after: started.mark });
      const status: PromoteResult["status"] | null =
        review.status === "failed" ? "failed"
        : review.blocked ? "blocked"
        : review.pending === 0 ? "promoted"
        : review.status === "paused" ? "paused"
        : Date.now() > deadline ? "timeout"
        : null;
      if (status) {
        return { status, pending: review.pending, conflicts: review.conflicts, blocked_on: review.blocked_on };
      }
      await new Promise<void>((r) => setTimeout(() => r(), pollMs));
    }
  }

  // -- methods ----------------------------------------------------------
  /** Call `package method` on `pov`: `try {pov} package method <{.k: v}>;`. */
  call(pov: string, pkg: string, method: string, args: Args = {}): Promise<unknown> {
    return this.send(`try {${pov}} ${pkg} ${method} ${lit(args)};`);
  }

  /**
   * Call `package method` on `pov` once per record in `argsList`, folding all N
   * calls into a single transaction (#253):
   * `try {pov} package method [<{.k: v1}>, <{.k: v2}>, ...];`.
   *
   * Resolves to a JSON array of the N per-call results, in order. A
   * write-coalescing primitive for commutative fan-in / bulk load: the batch is
   * all-or-nothing (one bad record rejects the set) and every call runs against
   * the same pre-batch snapshot (no read-your-writes within a batch).
   */
  callBatch(pov: string, pkg: string, method: string, argsList: Args[]): Promise<unknown> {
    if (argsList.length === 0) {
      throw new TypeError("orly: callBatch requires at least one argument record");
    }
    return this.send(`try {${pov}} ${pkg} ${method} ${lit(argsList)};`);
  }

  /** Run several different methods on `pov` as one transaction (#255). Each call is
   *  `[pkg, method, args]`; resolves to an array with one result per call, in order
   *  (they may differ in type). Every call reads the same pre-batch snapshot (no
   *  read-your-writes within a batch), and the batch is all-or-nothing: if any call
   *  fails, none of the writes land. */
  callMany(pov: string, calls: Array<[string, string, Args]>): Promise<unknown[]> {
    if (calls.length === 0) {
      throw new TypeError("orly: callMany requires at least one call");
    }
    const parts = calls.map(([pkg, method, args]) => `${pkg} ${method} ${lit(args)}`);
    return this.send(`try {${pov}} [${parts.join(", ")}];`) as Promise<unknown[]>;
  }

  /** Keyset paging (#735): call a paging method page after page, yielding each page's rows.
   *
   *  The method takes its arguments plus a cursor (named by `opts.cursor`, default `"last"`)
   *  and returns `<{.rows: [...], .last: ...}>`: a page of rows, and the cursor to pass for the
   *  page after it -- typically the last row's key, which the method gives to
   *  `keys (T) @ <[...]> after <[...]>`. `args` carries the first page's cursor. The server
   *  keeps no state between pages; each page reads the data as it is when that call runs.
   *  Iteration stops at the first empty page, or, given `opts.pageSize`, at the first page
   *  shorter than that. See docs/walkthrough.md for a paging method. */
  async *pages(
    pov: string,
    pkg: string,
    method: string,
    args: Args,
    opts: { cursor?: string; pageSize?: number } = {},
  ): AsyncGenerator<unknown[], void, undefined> {
    const cursor = opts.cursor ?? "last";
    if (!(cursor in args)) {
      throw new TypeError(`orly: pages needs the first page's cursor as args.${cursor}`);
    }
    let next: Args = { ...args };
    for (;;) {
      const page = (await this.call(pov, pkg, method, next)) as Record<string, unknown> | null;
      if (page === null || typeof page !== "object" || !Array.isArray(page.rows) || !(cursor in page)) {
        throw new TypeError(`orly: ${pkg} ${method} must return <{.rows: [...], .${cursor}: ...}> to be paged`);
      }
      const rows = page.rows as unknown[];
      if (rows.length === 0) return;
      yield rows;
      if (opts.pageSize !== undefined && rows.length < opts.pageSize) return;
      next = { ...next, [cursor]: page[cursor] };
    }
  }

  pause(pov: string): Promise<unknown> {
    return this.send(`pause {${pov}};`);
  }
  unpause(pov: string): Promise<unknown> {
    return this.send(`unpause {${pov}};`);
  }

  // -- teardown ---------------------------------------------------------
  async exit(): Promise<void> {
    try {
      await this.send("exit;");
    } finally {
      this.ws.close();
    }
  }
  close(): void {
    this.ws.close();
  }
}

/** Resolve a WebSocket constructor: the global one (browser) or `ws` (Node). */
async function resolveWebSocket(): Promise<new (url: string) => SocketLike> {
  const g = globalThis as any;
  if (typeof g.WebSocket !== "undefined") return g.WebSocket;
  const mod: any = await import("ws");
  return (mod.default ?? mod.WebSocket) as new (url: string) => SocketLike;
}

/** The token from the environment, in Node: `ORLY_AUTH_TOKEN`, or the contents of the file named
 *  by `ORLY_AUTH_TOKEN_FILE` (less a trailing newline). `undefined` in a browser, or if neither
 *  is set. */
async function tokenFromEnv(): Promise<string | undefined> {
  const env = (globalThis as any).process?.env;
  if (!env) return undefined;
  if (env.ORLY_AUTH_TOKEN_FILE) {
    const spec = "node:fs"; // a variable, so this isomorphic build needs no Node typings
    const fs: any = await import(spec);
    return String(fs.readFileSync(env.ORLY_AUTH_TOKEN_FILE, "utf8")).replace(/\r?\n$/, "");
  }
  return env.ORLY_AUTH_TOKEN || undefined;
}

/**
 * Open a WebSocket to a running `orlyi` and resolve a {@link Client}.
 *
 * A just-started or heavily loaded `orlyi` can refuse the connection or time
 * out the handshake briefly before it is ready, so the connection is retried
 * up to `opts.retries` times with exponential backoff (`opts.backoffMs`,
 * doubling each attempt). The last error is re-thrown once retries are
 * exhausted; pass `retries: 0` to fail fast on the first attempt.
 *
 * `opts.token` is the server's shared secret (#710), presented before
 * anything else. In Node it defaults to `ORLY_AUTH_TOKEN` or the file named by
 * `ORLY_AUTH_TOKEN_FILE`; in a browser, pass it. A refused token rejects with
 * {@link UnauthorizedError}, which is not retried.
 */
export async function connect(
  url: string = DEFAULT_URL,
  opts: { retries?: number; backoffMs?: number; token?: string } = {},
): Promise<Client> {
  const retries = opts.retries ?? DEFAULT_RETRIES;
  let delay = opts.backoffMs ?? DEFAULT_BACKOFF_MS;
  const token = opts.token ?? (await tokenFromEnv());
  const WS = await resolveWebSocket();
  for (let attempt = 0; ; attempt++) {
    try {
      const ws = new WS(url);
      await new Promise<void>((resolve, reject) => {
        ws.addEventListener("open", () => resolve());
        ws.addEventListener("error", (ev: any) => reject(ev instanceof Error ? ev : new Error("orly: connect failed")));
      });
      const client = new Client(ws);
      if (token !== undefined) {
        try {
          await client.authenticate(token);
        } catch (e) {
          client.close();
          throw e;
        }
      }
      return client;
    } catch (e) {
      if (e instanceof UnauthorizedError) throw e;
      if (attempt >= retries) throw e;
      await new Promise<void>((r) => setTimeout(() => r(), delay));
      delay *= 2;
    }
  }
}
