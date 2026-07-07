#!/usr/bin/env node
/**
 * orly-repl: an interactive orlyscript REPL (#535).
 *
 * Type an expression, see its value; type a definition, call it in the next
 * expression. There is no eval statement in the wire protocol, so the REPL
 * drives the same loop a package author would, invisibly: every entry is
 * folded into a synthetic package (`repl_<pid>.orly`), compiled with `orlyc`,
 * copied into the server's package directory, installed at the next version
 * number (installing a newer version re-points the unversioned package name;
 * older versions are deliberately never uninstalled — uninstalling any
 * version unregisters the whole name), and — for expressions — evaluated by
 * calling the generated nullary method `__repl_eval__` against the session's
 * POV.
 *
 * Because the compile/install machinery is local, the REPL needs more than a
 * WebSocket: `orlyc` must be runnable (ORLYC, default `orlyc` on PATH; may be
 * a multi-word command like `docker exec orly orlyc`) and the server's
 * package directory must be writable at the same path the server sees it
 * (ORLY_PACKAGE_DIR / --package-dir).
 *
 * State lives in a real POV (private by default; `--shared`, or `--pov <id>`
 * to join one that agents are already writing), so mutations entered at the
 * prompt behave exactly like package code: `(true) effecting { new <['k']>
 * <- 1; };` then `*<['k']>::(int);` reads it back.
 */

import * as readline from "node:readline";
import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { connect, Client, OrlyError, DEFAULT_URL } from "orly";

const EVAL_NAME = "__repl_eval__";

// -- config -----------------------------------------------------------------

interface Config {
  url: string;
  packageDir: string;
  orlyc: string[];
  pov?: string;
  shared: boolean;
}

const USAGE = `usage: orly-repl [options]

  --url <ws-url>         orlyi WebSocket url   (or ORLY_URL; default ${DEFAULT_URL})
  --package-dir <dir>    the server's package directory, writable from here
                         at the same path the server sees it (or ORLY_PACKAGE_DIR)
  --orlyc <cmd>          orlyc command, may be multi-word (or ORLYC; default "orlyc")
  --pov <id>             join an existing pov instead of creating one
  --shared               create a shared pov (default: private, session-local)
  --help                 this text
`;

function parseArgs(argv: string[]): Config {
  const cfg: Config = {
    url: process.env.ORLY_URL ?? DEFAULT_URL,
    packageDir: process.env.ORLY_PACKAGE_DIR ?? "",
    orlyc: (process.env.ORLYC ?? "orlyc").split(/\s+/),
    shared: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) throw new Error(`orly-repl: ${arg} needs a value`);
      return argv[++i];
    };
    switch (arg) {
      case "--url": cfg.url = next(); break;
      case "--package-dir": cfg.packageDir = next(); break;
      case "--orlyc": cfg.orlyc = next().split(/\s+/); break;
      case "--pov": cfg.pov = next(); break;
      case "--shared": cfg.shared = true; break;
      case "--help": case "-h":
        process.stdout.write(USAGE);
        process.exit(0);
      default:
        throw new Error(`orly-repl: unknown option ${arg}\n${USAGE}`);
    }
  }
  if (!cfg.packageDir) {
    throw new Error("orly-repl: --package-dir (or ORLY_PACKAGE_DIR) is required\n" + USAGE);
  }
  return cfg;
}

// -- input classification -----------------------------------------------------

/** Blank out string literals and block comments so delimiters inside them
    don't confuse completeness/classification checks. */
function stripLiterals(text: string): string {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, " ")
    .replace(/"(?:[^"\\]|\\.)*"/g, '""')
    .replace(/'(?:[^'\\]|\\.)*'/g, "''");
}

/** A statement is complete when, outside strings/comments, all brackets are
    balanced and the text ends with `;`. */
function isComplete(text: string): boolean {
  const s = stripLiterals(text);
  let depth = 0;
  for (const ch of s) {
    if (ch === "(" || ch === "[" || ch === "{") depth++;
    else if (ch === ")" || ch === "]" || ch === "}") depth--;
  }
  return depth <= 0 && s.trimEnd().endsWith(";");
}

/** `name = ...;` (but not `==`) is a definition; anything else is an
    expression to evaluate. */
function defName(text: string): string | null {
  const m = stripLiterals(text).match(/^\s*([_a-zA-Z][_a-zA-Z0-9]*)\s*=(?!=)/);
  return m ? m[1] : null;
}

// -- the synthetic package ----------------------------------------------------

class ReplPackage {
  readonly name = `repl_${process.pid}`;
  private version = 0;
  private readonly defs = new Map<string, string>();
  private readonly scratch = fs.mkdtempSync(path.join(os.tmpdir(), "orly-repl-"));
  private readonly installedSos: string[] = [];

  constructor(private readonly cfg: Config, private readonly client: Client) {}

  /** Package source for the accumulated defs, plus an optional eval body. */
  private source(evalExpr?: string): string {
    const parts = [`package #${this.version + 1};`, ...this.defs.values()];
    if (evalExpr !== undefined) parts.push(`${EVAL_NAME} = (${evalExpr});`);
    return parts.join("\n") + "\n";
  }

  /** Compile `src`, copy the .so into the server's package dir, install it.
      Throws with the compiler's diagnostics on failure. */
  private async compileAndInstall(src: string): Promise<void> {
    const v = this.version + 1;
    // orlyc generates its intermediates next to -o, so give each compile a
    // fresh subdirectory and keep the litter out of the user's CWD.
    const dir = path.join(this.scratch, `v${v}`);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, `${this.name}.orly`), src);
    const [cmd, ...prefix] = this.cfg.orlyc;
    const run = spawnSync(cmd, [...prefix, "-o", dir, `${this.name}.orly`], {
      cwd: dir,
      encoding: "utf8",
    });
    const so = path.join(dir, `${this.name}.${v}.so`);
    if (run.error) throw new Error(`orly-repl: cannot run orlyc: ${run.error.message}`);
    if (run.status !== 0 || !fs.existsSync(so)) {
      throw new Error(`${run.stdout ?? ""}${run.stderr ?? ""}`.trim() || "orlyc failed");
    }
    const dest = path.join(this.cfg.packageDir, path.basename(so));
    fs.copyFileSync(so, dest);
    await this.client.install(this.name, v);
    this.installedSos.push(dest);
    this.version = v;
    fs.rmSync(dir, { recursive: true, force: true });
  }

  /** Add or replace a definition; rolls back if it doesn't compile. */
  async define(name: string, text: string): Promise<void> {
    if (name === EVAL_NAME) throw new Error(`orly-repl: ${EVAL_NAME} is reserved`);
    const previous = this.defs.get(name);
    this.defs.set(name, text.trim());
    try {
      await this.compileAndInstall(this.source());
    } catch (e) {
      if (previous === undefined) this.defs.delete(name);
      else this.defs.set(name, previous);
      throw e;
    }
  }

  /** Evaluate an expression (sans trailing `;`) against `pov`. */
  async evaluate(expr: string, pov: string): Promise<unknown> {
    await this.compileAndInstall(this.source(expr));
    return this.client.call(pov, this.name, EVAL_NAME, {});
  }

  drop(name: string): boolean {
    return this.defs.delete(name);
  }

  listDefs(): string[] {
    return [...this.defs.values()];
  }

  currentSource(): string {
    return this.source();
  }

  /** Best-effort: our .so files mean nothing to anyone after this session. */
  cleanup(): void {
    for (const so of this.installedSos) fs.rmSync(so, { force: true });
    fs.rmSync(this.scratch, { recursive: true, force: true });
  }
}

// -- output -------------------------------------------------------------------

function printResult(v: unknown): void {
  console.log(v === undefined ? "null" : JSON.stringify(v, null, v !== null && typeof v === "object" ? 2 : undefined));
}

function printError(e: unknown): void {
  if (e instanceof OrlyError) {
    const r = e.reply as { result?: unknown } | null;
    console.error(`error: ${typeof r?.result === "string" ? r.result : JSON.stringify(e.reply)}`);
  } else {
    console.error(e instanceof Error ? e.message : String(e));
  }
}

const HELP = `orlyscript goes straight in; end a statement with ";" (multi-line is fine):
  1 + 2;                                   evaluate an expression
  double = (x * 2) where { x = given::(int); };   define (then: double(.x: 21);)
  (true) effecting { new <['k']> <- 1; };  write through the pov
  *<['k']>::(int);                         read it back
commands:
  :defs          show current definitions
  :drop <name>   remove a definition
  :src           show the synthetic package source
  :pov           show the pov id
  :quit          leave (also Ctrl-D)
`;

// -- main ---------------------------------------------------------------------

async function main(): Promise<void> {
  const cfg = parseArgs(process.argv.slice(2));
  const client = await connect(cfg.url);
  await client.newSession();
  const pov = cfg.pov ?? (await client.newPov({ shared: cfg.shared, safe: true }));
  const pkg = new ReplPackage(cfg, client);

  console.log(`orly-repl: connected to ${cfg.url}`);
  console.log(`pov ${pov} (${cfg.pov ? "joined" : cfg.shared ? "shared, new" : "private, new"}); :help for help`);

  const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
  const PROMPT = "orly> ";
  const CONT = "  ... ";
  let buffer = "";

  const prompt = () => {
    rl.setPrompt(buffer ? CONT : PROMPT);
    rl.prompt();
  };

  let quitting = false;
  const quit = async () => {
    if (quitting) return;
    quitting = true;
    rl.close();
    pkg.cleanup();
    try {
      await client.exit();
    } catch {
      client.close();
    }
    // No process.exit(): it truncates pending pipe writes (a piped consumer
    // would lose the last result/error, #542). With stdin, readline, and the
    // socket all closed, the event loop drains and node exits on its own.
    process.exitCode = 0;
  };

  const runCommand = async (line: string): Promise<void> => {
    const [cmd, ...rest] = line.split(/\s+/);
    switch (cmd) {
      case ":help": process.stdout.write(HELP); break;
      case ":defs": {
        const defs = pkg.listDefs();
        process.stdout.write(defs.length ? defs.join("\n") + "\n" : "(no definitions)\n");
        break;
      }
      case ":drop":
        console.log(pkg.drop(rest[0] ?? "") ? `dropped ${rest[0]}` : `no definition ${rest[0] ?? ""}`);
        break;
      case ":src": process.stdout.write(pkg.currentSource()); break;
      case ":pov": console.log(pov); break;
      case ":quit": case ":exit": await quit(); break;
      default: console.error(`unknown command ${cmd}; :help for help`);
    }
  };

  const runEntry = async (entry: string): Promise<void> => {
    const name = defName(entry);
    if (name) {
      await pkg.define(name, entry);
      console.log(`defined ${name}`);
    } else {
      printResult(await pkg.evaluate(entry.trim().replace(/;\s*$/, ""), pov));
    }
  };

  // Serialize entries: lines pasted faster than they compile must not race
  // the shared buffer / version counter.
  let chain: Promise<void> = Promise.resolve();
  rl.on("line", (line) => {
    chain = chain.then(async () => {
      if (quitting) return;
      try {
        if (!buffer && line.trimStart().startsWith(":")) {
          await runCommand(line.trim());
        } else {
          buffer += (buffer ? "\n" : "") + line;
          if (!buffer.trim()) buffer = "";
          else if (isComplete(buffer)) {
            const entry = buffer;
            buffer = "";
            await runEntry(entry);
          }
        }
      } catch (e) {
        printError(e);
      } finally {
        if (!quitting) prompt();
      }
    });
  });

  rl.on("SIGINT", () => {
    buffer = "";
    process.stdout.write("\n");
    prompt();
  });

  // stdin EOF (piped input, Ctrl-D). Quitting immediately would tear down
  // the socket and scratch dir under whatever entry is still compiling
  // (#542) — chain it so EOF means "quit after pending entries drain".
  rl.on("close", () => {
    chain = chain.then(() => quit());
  });

  prompt();
}

main().catch((e) => {
  console.error(e instanceof Error ? e.message : String(e));
  process.exitCode = 1;
});
