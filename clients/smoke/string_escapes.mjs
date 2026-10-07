/** String-escape smoke, TS driver (#756); run by run-string-escapes.sh. */
import { connect } from "../ts/dist/index.js";

const VALUES = [
  "plain",
  'quote " and backslash \\',
  "a\nb",
  "a\tb",
  "a\rb",
  "a\fb",
  "a\x01b",
  "a\x1fb",
  "a\x7fb",
  "line one\r\nline two\n\n\ttabbed",
  "literal backslash-n: a\\nb",
];

const c = await connect(process.env.ORLY_URL);
await c.newSession();
await c.install("string_escapes", 1);
const pov = await c.newPov();

for (const [n, value] of VALUES.entries()) {
  await c.call(pov, "string_escapes", "ps", { k: n, s: value });
  const got = await c.call(pov, "string_escapes", "gs", { k: n });
  if (got !== value) {
    console.error(`STRING ESCAPES FAIL (ts): wrote ${JSON.stringify(value)}, read back ${JSON.stringify(got)}`);
    process.exit(1);
  }
}
console.log(`ts: ${VALUES.length} strings round-tripped`);
c.close();
