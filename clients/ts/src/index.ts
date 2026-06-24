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
 * import { connect } from "orly";
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

export type Args = Record<string, unknown>;

/**
 * Encode a JS value as an orlyscript literal:
 * - `Raw`            -> its text, verbatim
 * - `boolean`        -> `true` / `false`
 * - `number`/`bigint`-> decimal
 * - `string`         -> a quoted, escaped string literal
 * - array            -> `[a, b, ...]`
 * - `OrlySet`        -> `{a, b, ...}`
 * - object           -> a record `<{.k: v, ...}>` (empty: `<{}>`)
 */
export function lit(value: unknown): string {
  if (value instanceof Raw) return value.text;
  if (value instanceof OrlySet) return "{" + value.items.map(lit).join(", ") + "}";
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
      const parts = Object.keys(m).map((k) => `.${k}: ${lit(m[k])}`);
      return "<{" + parts.join(", ") + "}>";
    }
    default:
      throw new TypeError(`orly: cannot encode ${typeof value} as a literal`);
  }
}

function quote(s: string): string {
  return '"' + s.replace(/\\/g, "\\\\").replace(/"/g, '\\"') + '"';
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
      p.reject(new OrlyError(p.stmt, reply));
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
  /** Create a POV; resolves its id. Defaults to `new safe shared pov;`. */
  newPov(opts: { safe?: boolean; shared?: boolean; parent?: string } = {}): Promise<string> {
    const { safe = true, shared = true, parent } = opts;
    const parts = ["new"];
    if (safe) parts.push("safe");
    parts.push(shared ? "shared" : "private");
    parts.push("pov");
    if (parent) parts.push(`parent ${lit(parent)}`);
    return this.sendString(parts.join(" ") + ";");
  }

  // -- methods ----------------------------------------------------------
  /** Call `package method` on `pov`: `try {pov} package method <{.k: v}>;`. */
  call(pov: string, pkg: string, method: string, args: Args = {}): Promise<unknown> {
    return this.send(`try {${pov}} ${pkg} ${method} ${lit(args)};`);
  }

  pause(pov: string): Promise<unknown> {
    return this.send(`pause pov ${lit(pov)};`);
  }
  unpause(pov: string): Promise<unknown> {
    return this.send(`unpause pov ${lit(pov)};`);
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

/** Open a WebSocket to a running `orlyi` and resolve a {@link Client}. */
export async function connect(url: string = DEFAULT_URL): Promise<Client> {
  const WS = await resolveWebSocket();
  const ws = new WS(url);
  await new Promise<void>((resolve, reject) => {
    ws.addEventListener("open", () => resolve());
    ws.addEventListener("error", (ev: any) => reject(ev instanceof Error ? ev : new Error("orly: connect failed")));
  });
  return new Client(ws);
}
