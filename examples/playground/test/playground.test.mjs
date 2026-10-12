import test from "node:test";
import assert from "node:assert/strict";
import * as http from "node:http";
import * as fs from "node:fs";
import * as path from "node:path";
import { EXAMPLES } from "../dist/examples.js";
import { highlightOrlyscript } from "../dist/web/syntax.js";

test("Playground Preloaded Examples", async (t) => {
  await t.test("Contains all 3 required examples", () => {
    assert.ok(EXAMPLES.counter, "Should contain counter example");
    assert.ok(EXAMPLES.grc20, "Should contain grc20 example");
    assert.ok(EXAMPLES.market, "Should contain prediction market example");
  });

  await t.test("Counter example structure", () => {
    const ex = EXAMPLES.counter;
    assert.equal(ex.id, "counter");
    assert.ok(ex.code.includes("package #1;"));
    assert.ok(ex.code.includes("+= delta;"), "Must demonstrate += commutative mutation");
    assert.ok(ex.defaultCalls.length >= 2, "Must have default calls");
  });

  await t.test("GRC-20 example structure", () => {
    const ex = EXAMPLES.grc20;
    assert.equal(ex.id, "grc20");
    assert.ok(ex.code.includes("evt_t"));
    assert.ok(ex.code.includes("resolve_as_of"));
    assert.ok(ex.code.includes("when"));
    assert.ok(ex.defaultCalls.some((c) => c.func === "display"));
  });

  await t.test("Prediction Market example structure", () => {
    const ex = EXAMPLES.market;
    assert.equal(ex.id, "market");
    assert.ok(ex.code.includes("staked"));
    assert.ok(ex.code.includes("price"));
    assert.ok(ex.code.includes("bet"));
    assert.ok(ex.defaultCalls.some((c) => c.func === "bet"));
    assert.ok(ex.defaultCalls.some((c) => c.func === "price"));
  });
});

test("Playground Syntax Highlighter", async (t) => {
  await t.test("Highlights keywords, types, and strings", () => {
    const code = `package #1;\nx = given::(int);\n// comment\n"hello"`;
    const highlighted = highlightOrlyscript(code);
    assert.ok(highlighted.includes('class="tok-keyword">package</span>'));
    assert.ok(highlighted.includes('class="tok-keyword">given</span>'));
    assert.ok(highlighted.includes('class="tok-type">int</span>'));
    assert.ok(highlighted.includes('class="tok-comment">// comment</span>'));
    assert.ok(highlighted.includes('class="tok-string">"hello"</span>'));
  });
});

test("Playground HTTP Endpoints", async (t) => {
  // Read public index.html
  const indexPath = path.resolve(process.cwd(), "public/index.html");
  assert.ok(fs.existsSync(indexPath), "public/index.html must exist");

  const server = http.createServer((req, res) => {
    const url = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);
    if (url.pathname === "/api/examples") {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(EXAMPLES));
      return;
    }
    if (url.pathname === "/api/status") {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ status: "ok" }));
      return;
    }
    res.writeHead(200, { "Content-Type": "text/html" });
    fs.createReadStream(indexPath).pipe(res);
  });

  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const port = server.address().port;

  try {
    await t.test("GET /api/examples returns examples JSON", async () => {
      const res = await fetch(`http://127.0.0.1:${port}/api/examples`);
      assert.equal(res.status, 200);
      const data = await res.json();
      assert.ok(data.counter);
      assert.ok(data.grc20);
      assert.ok(data.market);
    });

    await t.test("GET /api/status returns ok status", async () => {
      const res = await fetch(`http://127.0.0.1:${port}/api/status`);
      assert.equal(res.status, 200);
      const data = await res.json();
      assert.equal(data.status, "ok");
    });

    await t.test("GET / serves index.html", async () => {
      const res = await fetch(`http://127.0.0.1:${port}/`);
      assert.equal(res.status, 200);
      const html = await res.text();
      assert.ok(html.includes("Orlyscript Playground"));
    });
  } finally {
    server.close();
  }
});
