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
import {
  Client, OperationExpiredError, OperationIdReusedError, OperationInFlightError, OrlyError,
  OutcomeTooLargeError, newOpId,
} from "./dist/index.js";

const OP = "0194a2b4-7c00-7000-8000-000000000001";

class MockSocket {
  constructor() {
    this.listeners = new Map();
    this.sent = [];
    this.readyState = 1;
    this.closed = false;
  }

  addEventListener(type, listener) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(listener);
  }

  send(data) {
    this.sent.push(data);
  }

  close() {
    this.closed = true;
  }

  emit(type, ev) {
    for (const l of this.listeners.get(type) || []) l(ev);
  }

  reply(obj) {
    this.emit("message", { data: JSON.stringify(obj) });
  }
}

const tick = () => new Promise((r) => setTimeout(r, 5));

test("newOpId is a version 7 uuid", () => {
  const a = newOpId();
  assert.match(a, /^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
  assert.notEqual(a, newOpId());
});

test("call and callBatch carry the id", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  const p = c.call("p", "pkg", "fn", { k: 1 }, { opId: OP });
  assert.equal(ws.sent[0], `try {p} pkg fn <{.k: 1}> <{.op_id: "${OP}"}>;`);
  ws.reply({ status: "ok", result: 1 });
  await p;
  const b = c.callBatch("p", "pkg", "fn", [{ k: 1 }, { k: 2 }], { opId: OP, receipt: true });
  assert.equal(ws.sent[1], `try {p} pkg fn [<{.k: 1}>, <{.k: 2}>] <{.op_id: "${OP}", .receipt: true}>;`);
  ws.reply({ status: "ok", result: [1, 2] });
  await b;
});

test("operation statuses", async () => {
  for (const [status, cls] of [
    ["operation_expired", OperationExpiredError],
    ["operation_id_reused", OperationIdReusedError],
    ["operation_in_flight", OperationInFlightError],
    ["outcome_too_large", OutcomeTooLargeError],
  ]) {
    const ws = new MockSocket();
    const c = new Client(ws);
    const p = c.call("p", "pkg", "fn", {}, { opId: OP, opRetries: 0 });
    ws.reply({ status, result: "x", version: 7 });
    await assert.rejects(p, (err) => {
      assert.ok(err instanceof cls);
      assert.ok(err instanceof OrlyError);
      if (status === "outcome_too_large") assert.equal(err.version, 7);
      return true;
    });
  }
});

test("a dropped connection is resent with the same id, only when there is one", async () => {
  const first = new MockSocket();
  const second = new MockSocket();
  const c = new Client(first);
  c.reconnect = async () => second;
  const p = c.call("p", "pkg", "fn", { k: 1 }, { opId: OP });
  first.emit("close", {});
  await tick();
  await tick();
  assert.ok(first.closed);
  assert.equal(second.sent[0], first.sent[0]);
  second.reply({ status: "ok", result: 5 });
  assert.equal(await p, 5);

  const third = new MockSocket();
  const d = new Client(third);
  d.reconnect = async () => assert.fail("must not reconnect without an id");
  const q = d.call("p", "pkg", "fn", { k: 1 });
  third.emit("close", {});
  await assert.rejects(q, /websocket closed/);
});

test("an operation in flight is asked again", async () => {
  const ws = new MockSocket();
  const c = new Client(ws);
  c.reconnect = async () => assert.fail("no reconnect for an in-flight reply");
  const p = c.call("p", "pkg", "fn", {}, { opId: OP });
  ws.reply({ status: "operation_in_flight", result: "x" });
  await new Promise((r) => setTimeout(r, 300));
  assert.equal(ws.sent.length, 2);
  assert.equal(ws.sent[1], ws.sent[0]);
  ws.reply({ status: "ok", result: 3 });
  assert.equal(await p, 3);
});
