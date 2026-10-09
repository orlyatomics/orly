# Copyright 2010-2026 Atomic Kismet Company
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import json
import unittest

import orly
from orly import Client, DurableTimeout, OrlyError, _try_options


class DummyWs:
    def __init__(self, responses=None):
        self.responses = list(responses or [])
        self.sent = []

    def send(self, data):
        self.sent.append(data)

    def recv(self):
        if not self.responses:
            raise RuntimeError("no more mock responses")
        return self.responses.pop(0)


class ReceiptsTests(unittest.TestCase):
    def test_try_options_formatting(self):
        self.assertEqual(_try_options(), "")
        self.assertEqual(_try_options(receipt=False, wait_durable_ms=None), "")
        self.assertEqual(_try_options(receipt=True), " <{.receipt: true}>")
        self.assertEqual(_try_options(wait_durable_ms=1000), " <{.wait_durable_ms: 1000}>")
        self.assertEqual(_try_options(receipt=True, wait_durable_ms=500), " <{.receipt: true, .wait_durable_ms: 500}>")

    def test_durable_timeout_error(self):
        reply = {
            "status": "durable_timeout",
            "result": "timeout waiting for disk",
            "receipt": {"pov": "p1", "version": 10, "durability": "memory"}
        }
        err = DurableTimeout("stmt", reply)
        self.assertIsInstance(err, OrlyError)
        self.assertEqual(err.receipt, {"pov": "p1", "version": 10, "durability": "memory"})

    def test_call_without_receipt(self):
        ws = DummyWs([json.dumps({"status": "ok", "result": 123})])
        c = Client(ws)
        res = c.call("pov1", "pkg", "fn", {"k": 1})
        self.assertEqual(res, 123)
        self.assertIsNone(c.last_receipt)
        self.assertEqual(ws.sent, ["try {pov1} pkg fn <{.k: 1}>;"])

    def test_call_with_receipt(self):
        receipt = {"pov": "pov1", "version": 42, "durability": "memory"}
        ws = DummyWs([json.dumps({"status": "ok", "result": True, "receipt": receipt})])
        c = Client(ws)
        res, rcpt = c.call("pov1", "pkg", "fn", {"k": 1}, receipt=True)
        self.assertEqual(res, True)
        self.assertEqual(rcpt, receipt)
        self.assertEqual(c.last_receipt, receipt)
        self.assertEqual(ws.sent, ["try {pov1} pkg fn <{.k: 1}> <{.receipt: true}>;"])

    def test_call_with_wait_durable(self):
        receipt = {"pov": "pov1", "version": 42, "durability": "durable"}
        ws = DummyWs([json.dumps({"status": "ok", "result": True, "receipt": receipt})])
        c = Client(ws)
        res = c.call("pov1", "pkg", "fn", {}, wait_durable_ms=2000)
        self.assertEqual(res, True)
        self.assertEqual(c.last_receipt, receipt)
        self.assertEqual(ws.sent, ["try {pov1} pkg fn <{}> <{.wait_durable_ms: 2000}>;"])

    def test_call_durable_timeout(self):
        receipt = {"pov": "pov1", "version": 42, "durability": "memory"}
        ws = DummyWs([json.dumps({"status": "durable_timeout", "result": "timed out", "receipt": receipt})])
        c = Client(ws)
        with self.assertRaises(DurableTimeout) as caught:
            c.call("pov1", "pkg", "fn", wait_durable_ms=50)
        self.assertEqual(caught.exception.receipt, receipt)
        self.assertEqual(c.last_receipt, receipt)

    def test_call_batch_receipt(self):
        receipt = {"pov": "pov1", "version": 99, "durability": "durable"}
        ws = DummyWs([json.dumps({"status": "ok", "result": [1, 2], "receipt": receipt})])
        c = Client(ws)
        res, rcpt = c.call_batch("pov1", "pkg", "fn", [{"k": 1}, {"k": 2}], receipt=True, wait_durable_ms=100)
        self.assertEqual(res, [1, 2])
        self.assertEqual(rcpt, receipt)
        self.assertEqual(c.last_receipt, receipt)
        self.assertEqual(ws.sent, ["try {pov1} pkg fn [<{.k: 1}>, <{.k: 2}>] <{.receipt: true, .wait_durable_ms: 100}>;"])

    def test_durable_version(self):
        ws = DummyWs([
            json.dumps({"status": "ok", "result": {"pov": "pov1", "durable_version": 77.0}}),
            json.dumps({"status": "ok", "result": {"pov": "pov2", "durable_version": None}}),
        ])
        c = Client(ws)
        v1 = c.durable_version("pov1")
        self.assertEqual(v1, 77)
        v2 = c.durable_version("pov2")
        self.assertIsNone(v2)
        self.assertEqual(ws.sent, ["durable_version {pov1};", "durable_version {pov2};"])
