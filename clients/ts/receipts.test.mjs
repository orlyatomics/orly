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
import { Client, DurableTimeoutError, OrlyError } from "./dist/index.js";

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
    const ls = this.listeners.get("message") || [];
    for (const l of ls) l({ data });
  }
}

test("call without receipt", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const p = c.call("pov1", "pkg", "fn", { k: 1 });
  assert.equal(ws.sent.length, 1);
  assert.equal(ws.sent[0], "try {pov1} pkg fn <{.k: 1}>;");
  ws.reply({ status: "ok", result: 42 });
  const res = await p;
  assert.equal(res, 42);
  assert.equal(c.lastReceipt, undefined);
});

test("call with receipt: true", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const receipt = { pov: "pov1", version: 101, durability: "memory" };
  const p = c.call("pov1", "pkg", "fn", { k: 1 }, { receipt: true });
  assert.equal(ws.sent[0], "try {pov1} pkg fn <{.k: 1}> <{.receipt: true}>;");
  ws.reply({ status: "ok", result: true, receipt });
  const { result, receipt: rcpt } = await p;
  assert.equal(result, true);
  assert.deepEqual(rcpt, receipt);
  assert.deepEqual(c.lastReceipt, receipt);
});

test("call with waitDurableMs", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const receipt = { pov: "pov1", version: 102, durability: "durable" };
  const p = c.call("pov1", "pkg", "fn", {}, { waitDurableMs: 1500 });
  assert.equal(ws.sent[0], "try {pov1} pkg fn <{}> <{.wait_durable_ms: 1500}>;");
  ws.reply({ status: "ok", result: "saved", receipt });
  const res = await p;
  assert.equal(res, "saved");
  assert.deepEqual(c.lastReceipt, receipt);
});

test("callBatch with receipt and waitDurableMs", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const receipt = { pov: "pov1", version: 200, durability: "durable" };
  const p = c.callBatch("pov1", "pkg", "fn", [{ a: 1 }, { a: 2 }], { receipt: true, waitDurableMs: 500 });
  assert.equal(ws.sent[0], "try {pov1} pkg fn [<{.a: 1}>, <{.a: 2}>] <{.receipt: true, .wait_durable_ms: 500}>;");
  ws.reply({ status: "ok", result: [1, 2], receipt });
  const { result, receipt: rcpt } = await p;
  assert.deepEqual(result, [1, 2]);
  assert.deepEqual(rcpt, receipt);
  assert.deepEqual(c.lastReceipt, receipt);
});

test("durable timeout error carries receipt", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const receipt = { pov: "pov1", version: 50, durability: "memory" };
  const p = c.call("pov1", "pkg", "fn", {}, { waitDurableMs: 10 });
  ws.reply({ status: "durable_timeout", result: "timed out", receipt });
  await assert.rejects(
    async () => await p,
    (err) => {
      assert(err instanceof DurableTimeoutError);
      assert(err instanceof OrlyError);
      assert.deepEqual(err.receipt, receipt);
      return true;
    }
  );
  assert.deepEqual(c.lastReceipt, receipt);
});

test("durableVersion", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const p1 = c.durableVersion("pov1");
  assert.equal(ws.sent[0], "durable_version {pov1};");
  ws.reply({ status: "ok", result: { pov: "pov1", durable_version: 88 } });
  assert.equal(await p1, 88);

  const p2 = c.durableVersion("pov2");
  assert.equal(ws.sent[1], "durable_version {pov2};");
  ws.reply({ status: "ok", result: { pov: "pov2", durable_version: null } });
  assert.equal(await p2, null);
});
