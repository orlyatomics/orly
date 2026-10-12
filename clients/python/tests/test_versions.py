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

from orly import Client, OrlyError, VersionReadOnly, VersionUnavailable

POV = "00000000-0000-0000-0000-000000000001"


class DummyWs:
    def __init__(self, replies):
        self.replies = [json.dumps(r) for r in replies]
        self.sent = []

    def send(self, data):
        self.sent.append(data)

    def recv(self):
        return self.replies.pop(0)


def ok(result, **extra):
    return {"status": "ok", "result": result, **extra}


class VersionTests(unittest.TestCase):
    def test_call_at_a_version_surfaces_the_version(self):
        ws = DummyWs([ok(7, version=42)])
        c = Client(ws)
        self.assertEqual(c.call(POV, "pkg", "get", {"n": 1}, at=42), 7)
        self.assertEqual(ws.sent, [f"try {{{POV}}} at 42 pkg get <{{.n: 1}}>;"])
        self.assertEqual(c.last_version, 42)

    def test_call_at_a_time_or_save_point(self):
        ws = DummyWs([ok(1, version=500), ok(1, version=9)])
        c = Client(ws)
        c.call(POV, "pkg", "get", at={"time": "2026-01-01T00:00:00Z"})
        c.call(POV, "pkg", "get", at={"save": 'q"1'})
        self.assertEqual(ws.sent, [
            f'try {{{POV}}} at time "2026-01-01T00:00:00Z" pkg get <{{}}>;',
            f'try {{{POV}}} at save "q\\"1" pkg get <{{}}>;',
        ])

    def test_no_at_no_version(self):
        c = Client(DummyWs([ok(1, version=3), ok(1)]))
        c.call(POV, "pkg", "get")
        self.assertEqual(c.last_version, 3)
        c.call(POV, "pkg", "get")
        self.assertIsNone(c.last_version)

    def test_bad_at(self):
        c = Client(DummyWs([]))
        for bad in (-1, 1.5, True, "7", {"time": "x", "save": "y"}):
            with self.assertRaises(TypeError):
                c.call(POV, "pkg", "get", at=bad)

    def test_version_unavailable(self):
        reply = {"status": "version_unavailable", "result": "gone", "requested": 3, "oldest": 10, "newest": 20}
        c = Client(DummyWs([reply]))
        with self.assertRaises(VersionUnavailable) as caught:
            c.call(POV, "pkg", "get", at=3)
        err = caught.exception
        self.assertIsInstance(err, OrlyError)
        self.assertEqual((err.requested, err.oldest, err.newest), (3, 10, 20))

    def test_version_read_only(self):
        c = Client(DummyWs([{"status": "version_read_only", "result": "no"}]))
        with self.assertRaises(VersionReadOnly):
            c.call(POV, "pkg", "put", at=1)

    def test_statements(self):
        ws = DummyWs([
            ok({"pov": POV, "version": 1000}),
            ok({"pov": "p", "version": "(3, 1000)"}),
            ok({"lease": "L", "seconds": 60}),
            ok({"lease": "L", "released": True}),
            ok({"save_point": "a"}),
            ok({"save_point": "a"}),
            ok({"save_point": "a", "dropped": False}),
        ])
        c = Client(ws)
        self.assertEqual(c.version_of(POV), 1000)
        self.assertEqual(c.version_of("p"), "(3, 1000)")
        self.assertEqual(c.hold(POV, 5, 60), "L")
        self.assertTrue(c.release("L"))
        c.save_point(POV, "a")
        c.save_point(POV, "a", at=7)
        self.assertFalse(c.drop_save_point(POV, "a"))
        self.assertEqual(ws.sent, [
            f"version of {{{POV}}};",
            "version of {p};",
            f"hold version {{{POV}}} 5 for 60;",
            "release {L};",
            f'save point {{{POV}}} "a";',
            f'save point {{{POV}}} "a" at 7;',
            f'drop save point {{{POV}}} "a";',
        ])

    def test_pages_pin_the_version(self):
        rows = lambda r, last: ok({"rows": r, "last": last})
        ws = DummyWs([ok({"version": 77}), rows([1, 2], 2), rows([3], 3), rows([], 3)])
        pages = list(Client(ws).pages(POV, "pkg", "list", {"last": 0}))
        self.assertEqual(pages, [[1, 2], [3]])
        self.assertEqual(ws.sent[1:], [
            f"try {{{POV}}} at 77 pkg list <{{.last: 0}}>;",
            f"try {{{POV}}} at 77 pkg list <{{.last: 2}}>;",
            f"try {{{POV}}} at 77 pkg list <{{.last: 3}}>;",
        ])

    def test_pages_do_not_pin_elsewhere(self):
        rows = ok({"rows": [], "last": 0})
        ws = DummyWs([ok({"version": "(3, 1000)"}), rows])
        list(Client(ws).pages("p", "pkg", "list", {"last": 0}))
        self.assertEqual(ws.sent[1], "try {p} pkg list <{.last: 0}>;")
        ws = DummyWs([rows])
        list(Client(ws).pages(POV, "pkg", "list", {"last": 0}, pin=False))
        self.assertEqual(ws.sent, [f"try {{{POV}}} pkg list <{{.last: 0}}>;"])
        ws = DummyWs([rows])
        list(Client(ws).pages(POV, "pkg", "list", {"last": 0}, at=9))
        self.assertEqual(ws.sent, [f"try {{{POV}}} at 9 pkg list <{{.last: 0}}>;"])


if __name__ == "__main__":
    unittest.main()
