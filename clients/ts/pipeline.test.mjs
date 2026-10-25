/*
   Copyright 2010-2026 Atomic Kismet Company

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

import assert from "node:assert/strict";
import test from "node:test";
import { Client, OrlyError } from "./dist/index.js";

class MockSocket {
  constructor() {
    this.listeners = new Map();
    this.sent = [];
    this.readyState = 1;
  }

  addEventListener(type, listener) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(listener);
  }

  send(data) {
    this.sent.push(data);
  }

  close() {}

  reply(obj) {
    const data = JSON.stringify(obj);
    for (const l of this.listeners.get("message") || []) l({ data });
  }

  envelopes() {
    return this.sent.map((s) => JSON.parse(s));
  }
}

const tick = () => new Promise((r) => setImmediate(r));

test("results come back in the order given, whatever order the replies arrive in", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const p = c.pipeline([{ stmt: "a;", read: true }, { stmt: "b;", read: true }, { stmt: "c;", read: true }]);
  const sent = ws.envelopes();
  assert.deepEqual(sent, [
    { id: 1, stmt: "a;", read: true },
    { id: 2, stmt: "b;", read: true },
    { id: 3, stmt: "c;", read: true },
  ]);
  ws.reply({ id: 3, status: "ok", result: "C" });
  ws.reply({ id: 1, status: "ok", result: "A" });
  ws.reply({ id: 2, status: "ok", result: "B" });
  assert.deepEqual(await p, ["A", "B", "C"]);
});

test("a string is a barrier envelope without read", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const p = c.pipeline(["w;"]);
  assert.deepEqual(ws.envelopes(), [{ id: 1, stmt: "w;" }]);
  ws.reply({ id: 1, status: "ok", result: null });
  assert.deepEqual(await p, [null]);
});

test("the window bounds what is in flight", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const p = c.pipeline([1, 2, 3, 4, 5].map((i) => ({ stmt: `r${i};`, read: true })), { window: 2 });
  assert.equal(ws.sent.length, 2);
  ws.reply({ id: 1, status: "ok", result: 1 });
  await tick();
  assert.equal(ws.sent.length, 3);
  ws.reply({ id: 2, status: "ok", result: 2 });
  ws.reply({ id: 3, status: "ok", result: 3 });
  await tick();
  assert.equal(ws.sent.length, 5);
  ws.reply({ id: 4, status: "ok", result: 4 });
  ws.reply({ id: 5, status: "ok", result: 5 });
  assert.deepEqual(await p, [1, 2, 3, 4, 5]);
});

test("every reply is read before the first failure in the order given is thrown", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const p = c.pipeline([{ stmt: "a;", read: true }, { stmt: "b;", read: true }, { stmt: "c;", read: true }]);
  const settled = p.then(() => "resolved", (e) => e);
  ws.reply({ id: 3, status: "exception", result: "late" });
  ws.reply({ id: 2, status: "exception", result: "first" });
  await tick();
  let done = false;
  settled.then(() => { done = true; });
  await tick();
  assert.equal(done, false, "must wait for id 1");
  ws.reply({ id: 1, status: "ok", result: "A" });
  const err = await settled;
  assert.ok(err instanceof OrlyError);
  assert.equal(err.statement, "b;");
});

test("a bare statement still takes the next reply in line", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const bare = c.send("new session;");
  const piped = c.pipeline([{ stmt: "r;", read: true }]);
  ws.reply({ id: 1, status: "ok", result: "R" });
  ws.reply({ status: "ok", result: "S" });
  assert.equal(await bare, "S");
  assert.deepEqual(await piped, ["R"]);
});

test("a reply that matches nothing fails the statements in flight", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const p = c.pipeline([{ stmt: "r;", read: true }]);
  ws.reply({ status: "exception", result: "bad" });
  await assert.rejects(p, /matches no statement/);
});

test("callPipelined builds one try per record", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const p = c.callPipelined("p1", "pkg/v1", "get", [{ k: 1 }, { k: 2 }], { read: true });
  const sent = ws.envelopes();
  assert.equal(sent.length, 2);
  assert.ok(sent.every((e) => e.read === true));
  assert.match(sent[0].stmt, /^try \{p1\} pkg\/v1 get <\{\.k: 1\}>;$/);
  ws.reply({ id: 1, status: "ok", result: 10 });
  ws.reply({ id: 2, status: "ok", result: 20 });
  assert.deepEqual(await p, [10, 20]);
});

test("an empty pipeline resolves at once and a bad window is refused", async () => {
  const c = new Client(new MockSocket());
  assert.deepEqual(await c.pipeline([]), []);
  await assert.rejects(c.pipeline(["a;"], { window: 0 }), RangeError);
});
