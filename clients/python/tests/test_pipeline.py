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

from orly import Client, OrlyError


class EchoWs:
    """Answers each envelope it was sent, in the order ``order`` picks."""

    def __init__(self, reverse=False, fail_ids=()):
        self.sent = []
        self.pending = []
        self.reverse = reverse
        self.fail_ids = set(fail_ids)
        self.max_in_flight = 0

    def send(self, data):
        env = json.loads(data)
        self.sent.append(env)
        self.pending.append(env)
        self.max_in_flight = max(self.max_in_flight, len(self.pending))

    def recv(self):
        env = self.pending.pop() if self.reverse else self.pending.pop(0)
        if env["id"] in self.fail_ids:
            return json.dumps({"id": env["id"], "status": "exception", "result": "boom"})
        return json.dumps({"id": env["id"], "status": "ok", "result": env["stmt"]})


class PipelineTests(unittest.TestCase):
    def test_results_come_back_in_the_order_given(self):
        ws = EchoWs(reverse=True)
        out = Client(ws).pipeline([("a;", True), ("b;", True), ("c;", True)])
        self.assertEqual(out, ["a;", "b;", "c;"])

    def test_envelopes_mark_reads_and_ids(self):
        ws = EchoWs()
        Client(ws).pipeline(["w;", ("r;", True)])
        self.assertEqual(ws.sent, [{"id": 0, "stmt": "w;"}, {"id": 1, "stmt": "r;", "read": True}])

    def test_window_bounds_what_is_in_flight(self):
        ws = EchoWs()
        out = Client(ws).pipeline([("r%d;" % i, True) for i in range(10)], window=3)
        self.assertEqual(len(out), 10)
        self.assertEqual(ws.max_in_flight, 3)

    def test_every_reply_is_read_before_the_first_failure_is_raised(self):
        ws = EchoWs(fail_ids=[1, 2])
        with self.assertRaises(OrlyError) as caught:
            Client(ws).pipeline([("a;", True), ("b;", True), ("c;", True), ("d;", True)])
        self.assertEqual(caught.exception.statement, "b;")
        self.assertEqual(ws.pending, [])

    def test_reply_without_an_id_is_an_error(self):
        class NoId(EchoWs):
            def recv(self):
                return json.dumps({"status": "exception", "result": "bad"})

        with self.assertRaises(OrlyError):
            Client(NoId()).pipeline(["a;"])

    def test_call_pipelined_builds_one_statement_per_record(self):
        ws = EchoWs()
        out = Client(ws).call_pipelined("p1", "pkg/v1", "get", [{"k": 1}, {"k": 2}], read=True)
        self.assertEqual(len(out), 2)
        self.assertTrue(all(e["read"] for e in ws.sent))
        self.assertIn("try {p1} pkg/v1 get <{.k: 1}>;", ws.sent[0]["stmt"])

    def test_window_must_be_positive(self):
        with self.assertRaises(ValueError):
            Client(EchoWs()).pipeline(["a;"], window=0)


if __name__ == "__main__":
    unittest.main()
