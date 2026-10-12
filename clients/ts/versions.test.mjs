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
import { Client, OrlyError, VersionReadOnlyError, VersionUnavailableError } from "./dist/index.js";

const POV = "00000000-0000-0000-0000-000000000001";

class MockSocket {
  constructor(replies = []) {
    this.listeners = new Map();
    this.sent = [];
    this.replies = replies;
    this.readyState = 1;
  }
  addEventListener(type, listener) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(listener);
  }
  send(data) {
    this.sent.push(data);
    const reply = this.replies.shift();
    if (reply !== undefined) {
      queueMicrotask(() => this.reply(typeof reply === "function" ? reply(data) : reply));
    }
  }
  close() {}
  reply(obj) {
    for (const l of this.listeners.get("message") || []) l({ data: JSON.stringify(obj) });
  }
}

test("call at a version sends the keyword form and surfaces the reply's version", async () => {
  const ws = new MockSocket([{ status: "ok", result: 7, version: 42 }]);
  const c = new Client(ws);
  assert.equal(await c.call(POV, "pkg", "get", { n: 1 }, { at: 42 }), 7);
  assert.equal(ws.sent[0], `try {${POV}} at 42 pkg get <{.n: 1}>;`);
  assert.equal(c.lastVersion, 42);
});

test("call at a time or a save point", async () => {
  const ws = new MockSocket([{ status: "ok", result: 1, version: 500 }, { status: "ok", result: 1, version: 9 }]);
  const c = new Client(ws);
  const r = await c.call(POV, "pkg", "get", {}, { at: { time: "2026-01-01T00:00:00Z" }, receipt: true });
  assert.deepEqual(r, { result: 1, receipt: undefined, version: 500 });
  await c.call(POV, "pkg", "get", {}, { at: { save: 'q"1' } });
  assert.equal(ws.sent[0], `try {${POV}} at time "2026-01-01T00:00:00Z" pkg get <{}> <{.receipt: true}>;`);
  assert.equal(ws.sent[1], `try {${POV}} at save "q\\"1" pkg get <{}>;`);
});

test("a call without at has no version", async () => {
  const c = new Client(new MockSocket([{ status: "ok", result: 1, version: 3 }, { status: "ok", result: 1 }]));
  await c.call(POV, "pkg", "get");
  assert.equal(c.lastVersion, 3);
  await c.call(POV, "pkg", "get");
  assert.equal(c.lastVersion, undefined);
});

test("at must be a non-negative integer, and does not batch", async () => {
  const c = new Client(new MockSocket());
  await assert.rejects(() => c.call(POV, "pkg", "get", {}, { at: -1 }), TypeError);
  await assert.rejects(() => c.call(POV, "pkg", "get", {}, { at: 1.5 }), TypeError);
  await assert.rejects(() => c.callBatch(POV, "pkg", "get", [{}], { at: 1 }), TypeError);
});

test("version_unavailable is a typed error and is not resent", async () => {
  const reply = { status: "version_unavailable", result: "gone", requested: 3, oldest: 10, newest: 20 };
  const ws = new MockSocket([reply, reply]);
  const c = new Client(ws);
  c.reconnect = async () => { throw new Error("must not reconnect"); };
  await assert.rejects(
    () => c.call(POV, "pkg", "get", {}, { at: 3, opId: "x" }),
    (e) => e instanceof VersionUnavailableError && e instanceof OrlyError &&
      e.requested === 3 && e.oldest === 10 && e.newest === 20,
  );
  assert.equal(ws.sent.length, 1);
});

test("version_read_only is typed", async () => {
  const c = new Client(new MockSocket([{ status: "version_read_only", result: "no" }]));
  await assert.rejects(() => c.call(POV, "pkg", "put", {}, { at: 1 }), VersionReadOnlyError);
});

test("versionOf, hold, release, savePoint, dropSavePoint", async () => {
  const ws = new MockSocket([
    { status: "ok", result: { pov: POV, version: 1000 } },
    { status: "ok", result: { pov: "p", version: "(3, 1000)" } },
    { status: "ok", result: { pov: POV, version: 5, lease: "L", seconds: 60 } },
    { status: "ok", result: { lease: "L", released: true } },
    { status: "ok", result: { pov: POV, save_point: "a" } },
    { status: "ok", result: { pov: POV, save_point: "a" } },
    { status: "ok", result: { pov: POV, save_point: "a", dropped: false } },
  ]);
  const c = new Client(ws);
  assert.equal(await c.versionOf(POV), 1000);
  assert.equal(await c.versionOf("p"), "(3, 1000)");
  assert.equal(await c.hold(POV, 5, 60), "L");
  assert.equal(await c.release("L"), true);
  await c.savePoint(POV, "a");
  await c.savePoint(POV, "a", { at: 7 });
  assert.equal(await c.dropSavePoint(POV, "a"), false);
  assert.deepEqual(ws.sent, [
    `version of {${POV}};`,
    "version of {p};",
    `hold version {${POV}} 5 for 60;`,
    "release {L};",
    `save point {${POV}} "a";`,
    `save point {${POV}} "a" at 7;`,
    `drop save point {${POV}} "a";`,
  ]);
});

const rows = (r, last) => ({ status: "ok", result: { rows: r, last } });

test("pages pins the version of the global POV the first page ran against", async () => {
  const ws = new MockSocket([
    { status: "ok", result: { pov: POV, version: 77 } },
    rows([1, 2], 2),
    rows([3], 3),
    rows([], 3),
  ]);
  const c = new Client(ws);
  const got = [];
  for await (const page of c.pages(POV, "pkg", "list", { last: 0 })) got.push(page);
  assert.deepEqual(got, [[1, 2], [3]]);
  assert.deepEqual(ws.sent.slice(1), [
    `try {${POV}} at 77 pkg list <{.last: 0}>;`,
    `try {${POV}} at 77 pkg list <{.last: 2}>;`,
    `try {${POV}} at 77 pkg list <{.last: 3}>;`,
  ]);
});

test("pages does not pin on another POV, with pin: false, or when at is given", async () => {
  let ws = new MockSocket([{ status: "ok", result: { pov: "p", version: "(3, 1000)" } }, rows([], 0)]);
  for await (const _ of new Client(ws).pages("p", "pkg", "list", { last: 0 }));
  assert.equal(ws.sent[1], "try {p} pkg list <{.last: 0}>;");

  ws = new MockSocket([rows([], 0)]);
  for await (const _ of new Client(ws).pages(POV, "pkg", "list", { last: 0 }, { pin: false }));
  assert.deepEqual(ws.sent, [`try {${POV}} pkg list <{.last: 0}>;`]);

  ws = new MockSocket([rows([], 0)]);
  for await (const _ of new Client(ws).pages(POV, "pkg", "list", { last: 0 }, { at: 9 }));
  assert.deepEqual(ws.sent, [`try {${POV}} at 9 pkg list <{.last: 0}>;`]);
});
