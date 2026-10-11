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
import uuid
from unittest import mock

import orly
from orly import (Client, OperationExpired, OperationIdReused, OperationInFlight, OutcomeTooLarge,
                  _try_options, new_op_id)

OP = "0194a2b4-7c00-7000-8000-000000000001"


class DummyWs:
    """Replies are JSON strings, or exceptions to raise from recv()."""

    def __init__(self, responses=None):
        self.responses = list(responses or [])
        self.sent = []
        self.closed = False

    def send(self, data):
        self.sent.append(data)

    def recv(self):
        r = self.responses.pop(0)
        if isinstance(r, Exception):
            raise r
        return r

    def close(self):
        self.closed = True


def ok(result=1):
    return json.dumps({"status": "ok", "result": result})


class OperationTests(unittest.TestCase):
    def test_new_op_id_is_a_v7_uuid(self):
        a, b = new_op_id(), new_op_id()
        self.assertNotEqual(a, b)
        u = uuid.UUID(a)
        self.assertEqual(u.version, 7)
        self.assertEqual(u.variant, uuid.RFC_4122)
        self.assertEqual(str(u), a)

    def test_option_is_spelled_only_with_an_id(self):
        self.assertEqual(_try_options(op_id=OP), f' <{{.op_id: "{OP}"}}>')
        self.assertEqual(_try_options(receipt=True, op_id=OP), f' <{{.op_id: "{OP}", .receipt: true}}>')
        self.assertEqual(_try_options(), "")

    def test_call_carries_the_id(self):
        ws = DummyWs([ok()])
        Client(ws).call("p", "pkg", "fn", {"k": 1}, op_id=OP)
        self.assertEqual(ws.sent, [f'try {{p}} pkg fn <{{.k: 1}}> <{{.op_id: "{OP}"}}>;'])

    def test_batch_carries_the_id(self):
        ws = DummyWs([ok([1, 2])])
        Client(ws).call_batch("p", "pkg", "fn", [{"k": 1}, {"k": 2}], op_id=OP)
        self.assertEqual(ws.sent, [f'try {{p}} pkg fn [<{{.k: 1}}>, <{{.k: 2}}>] <{{.op_id: "{OP}"}}>;'])

    def test_statuses(self):
        for status, cls in (("operation_expired", OperationExpired),
                            ("operation_id_reused", OperationIdReused),
                            ("operation_in_flight", OperationInFlight),
                            ("outcome_too_large", OutcomeTooLarge)):
            ws = DummyWs([json.dumps({"status": status, "result": "x", "version": 7})])
            with self.assertRaises(cls) as caught:
                Client(ws).call("p", "pkg", "fn", op_id=OP, op_retries=0)
            self.assertIsInstance(caught.exception, orly.OrlyError)
        self.assertEqual(OutcomeTooLarge("s", {"version": 7}).version, 7)

    def test_no_id_no_retry(self):
        ws = DummyWs([OSError("reset")])
        c = Client(ws)
        c._reconnect = {"url": "ws://x/"}
        with self.assertRaises(OSError):
            c.call("p", "pkg", "fn")
        self.assertEqual(len(ws.sent), 1)

    def test_dropped_connection_resends_with_the_same_id(self):
        first = DummyWs([OSError("reset")])
        second = DummyWs([ok("resumed"), ok(5)])
        c = Client(first)
        c.session_id = "s1"
        c._reconnect = {"url": "ws://x/"}
        with mock.patch.object(orly, "connect", return_value=Client(second)), mock.patch.object(orly._time, "sleep"):
            self.assertEqual(c.call("p", "pkg", "fn", {"k": 1}, op_id=OP), 5)
        self.assertTrue(first.closed)
        self.assertEqual(second.sent[0], "resume session {s1};")
        self.assertEqual(second.sent[1], first.sent[0])

    def test_in_flight_is_retried_then_raised(self):
        busy = json.dumps({"status": "operation_in_flight", "result": "x"})
        ws = DummyWs([busy, ok(3)])
        c = Client(ws)
        c._reconnect = {"url": "ws://x/"}
        with mock.patch.object(orly._time, "sleep"):
            self.assertEqual(c.call("p", "pkg", "fn", op_id=OP), 3)
        ws = DummyWs([busy, busy])
        c = Client(ws)
        c._reconnect = {"url": "ws://x/"}
        with mock.patch.object(orly._time, "sleep"), self.assertRaises(OperationInFlight):
            c.call("p", "pkg", "fn", op_id=OP, op_retries=1)


if __name__ == "__main__":
    unittest.main()
