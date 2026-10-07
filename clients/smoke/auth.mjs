// TS-client checks for the shared-secret smoke (#710), run by run-auth.sh against an orlyi
// started with ORLY_SMOKE_TOKEN: no token and a wrong one reject with UnauthorizedError, the
// right one works. ORLY_AUTH_TOKEN is unset here, so "no token" really sends none.
import { connect, UnauthorizedError } from "../ts/dist/index.js";

const url = process.env.ORLY_URL;
const token = process.env.ORLY_SMOKE_TOKEN;
let failed = 0;
const check = (ok, what) => {
  console.log((ok ? "  ok   " : "  FAIL ") + what);
  if (!ok) failed++;
};

// No token: the first statement is refused.
try {
  const c = await connect(url, { retries: 0 });
  await c.newSession();
  check(false, "ts: a connection with no token was accepted");
  c.close();
} catch (e) {
  check(e instanceof UnauthorizedError, `ts: no token -> UnauthorizedError (${e?.name})`);
}

// Wrong token: connect() itself rejects, without retrying.
try {
  const c = await connect(url, { retries: 3, token: "0123456789abcdef-not-the-token" });
  check(false, "ts: a wrong token was accepted");
  c.close();
} catch (e) {
  check(e instanceof UnauthorizedError, `ts: wrong token -> UnauthorizedError (${e?.name})`);
  check(!String(e.message).includes(token), "ts: the refusal does not echo the token");
}

// Right token.
try {
  const c = await connect(url, { retries: 0, token });
  check(Boolean(await c.newSession()), "ts: right token -> new session");
  check((await c.send("echo 'hello';")) === "hello", "ts: right token -> statements run");
  c.close();
} catch (e) {
  check(false, `ts: the right token was refused: ${e?.name}`);
}

if (failed) {
  console.log(`TS AUTH CHECK FAILED: ${failed} check(s)`);
  process.exit(1);
}
console.log("TS AUTH CHECK OK");
