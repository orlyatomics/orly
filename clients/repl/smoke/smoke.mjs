// Drives the built orly-repl binary over stdin and asserts on its output.
// Env (set by run-smoke.sh): ORLY_URL, ORLY_PACKAGE_DIR, ORLYC.
import { spawn } from "node:child_process";

const STEP_TIMEOUT_MS = 60_000;

const repl = spawn(process.execPath, ["dist/index.js"], {
  stdio: ["pipe", "pipe", "pipe"],
});

let transcript = "";
repl.stdout.on("data", (d) => { transcript += d; });
repl.stderr.on("data", (d) => { transcript += d; });

const exited = new Promise((resolve) => repl.on("exit", resolve));

/** Wait until the transcript grown since `from` satisfies `test`. */
function waitFor(from, test, what) {
  return new Promise((resolve, reject) => {
    const t0 = Date.now();
    const poll = () => {
      const slice = transcript.slice(from);
      if (test(slice)) return resolve(slice);
      if (Date.now() - t0 > STEP_TIMEOUT_MS) {
        return reject(new Error(`timeout waiting for ${what}\n--- transcript ---\n${transcript}`));
      }
      setTimeout(poll, 50);
    };
    poll();
  });
}

let mark = 0;
async function send(line, test, what) {
  mark = transcript.length;
  repl.stdin.write(line + "\n");
  await waitFor(mark, test, what);
}

const contains = (needle) => (s) => s.includes(needle);

try {
  await waitFor(0, contains("pov "), "startup banner");

  // Expression eval: the whole synthetic-package round trip.
  await send("1 + 2;", contains("3\n"), "1 + 2 -> 3");

  // A definition, then a call that uses it.
  await send(
    "double = (x * 2) where { x = given::(int); };",
    contains("defined double"),
    "define double",
  );
  await send("double(.x: 21);", contains("42\n"), "double(.x: 21) -> 42");

  // Multi-line entry: incomplete first line must NOT produce output, the
  // completing line must.
  mark = transcript.length;
  repl.stdin.write("triple = (x * 3)\n");
  await new Promise((r) => setTimeout(r, 500));
  const between = transcript.slice(mark);
  if (between.includes("defined") || between.includes("error")) {
    throw new Error(`continuation line evaluated too early: ${between}`);
  }
  await send(
    "  where { x = given::(int); };",
    contains("defined triple"),
    "multi-line define triple",
  );
  await send("triple(.x: 5);", contains("15\n"), "triple(.x: 5) -> 15");

  // Writes go through the pov and read back.
  await send(
    "(true) effecting { new <['smoke', 1]> <- 7; };",
    contains("true\n"),
    "effecting write",
  );
  await send("*<['smoke', 1]>::(int);", contains("7\n"), "read-back -> 7");

  // Prompt returned (the entry was processed) AND something else was
  // printed (diagnostics) — stdout/stderr chunk order is not guaranteed, so
  // don't require the prompt to be last.
  const diagnosedAndAlive = (s) =>
    s.includes("orly> ") && s.replace(/orly> */g, "").trim().length > 0;

  // A compile error surfaces diagnostics and does not kill the session...
  await send("this is not orlyscript;", diagnosedAndAlive, "compile-error diagnostics + prompt back");
  // ...and a bad definition rolls back without clobbering the good ones.
  await send(
    "double = (no_such_function(.q: 1));",
    diagnosedAndAlive,
    "bad redefinition rejected",
  );
  await send("double(.x: 10);", contains("20\n"), "double survived bad redefinition");

  // Commands.
  await send(":defs", contains("triple"), ":defs lists definitions");

  await send(":quit", () => true, "quit");
  const code = await exited;
  if (code !== 0) throw new Error(`repl exited ${code}\n--- transcript ---\n${transcript}`);

  // Piped-EOF regression (#542): the docker/CI shape — all input arrives at
  // once and stdin closes immediately, while the first entry is still
  // compiling. The entry must complete, print, and exit 0 anyway.
  const piped = spawn(process.execPath, ["dist/index.js"], {
    stdio: ["pipe", "pipe", "pipe"],
  });
  let pipedOut = "";
  piped.stdout.on("data", (d) => { pipedOut += d; });
  piped.stderr.on("data", (d) => { pipedOut += d; });
  const pipedExit = new Promise((resolve) => piped.on("exit", resolve));
  piped.stdin.end("40 + 2;\n");
  const pipedCode = await Promise.race([
    pipedExit,
    new Promise((_, reject) =>
      setTimeout(() => reject(new Error(`piped-EOF: no exit\n--- transcript ---\n${pipedOut}`)), STEP_TIMEOUT_MS),
    ),
  ]);
  if (pipedCode !== 0 || !pipedOut.includes("42\n")) {
    piped.kill("SIGKILL");
    throw new Error(`piped-EOF: exit ${pipedCode}\n--- transcript ---\n${pipedOut}`);
  }

  console.log("orly-repl smoke: all checks passed");
  process.exit(0);
} catch (e) {
  console.error(e.message ?? e);
  repl.kill("SIGKILL");
  process.exit(1);
}
