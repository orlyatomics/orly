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

import unittest

from orly import lit


class LiteralTests(unittest.TestCase):
    def test_keyword_fields_are_unchanged(self):
        for name in ("id", "to", "from", "start", "after", "if", "true",
                     "int", "and", "keys", "try", "safe", "list_packages"):
            with self.subTest(name=name):
                self.assertEqual(lit({name: 1}), f"<{{.{name}: 1}}>")
        self.assertEqual(lit({"to": {"id": 7}}), "<{.to: <{.id: 7}>}>")
        self.assertEqual(lit({}), "<{}>")
        self.assertEqual(lit({"_id2": True}), "<{._id2: true}>")

    def test_invalid_fields_suggest_a_rename(self):
        for name in ("", "1st", "first-name", "a.b", "é", "id\n", 1):
            with self.subTest(name=name):
                with self.assertRaises(ValueError) as caught:
                    lit({name: 1})
                self.assertIn(repr(name), str(caught.exception))
                self.assertIn("rename", str(caught.exception))
                with self.assertRaisesRegex(ValueError, "invalid record field name"):
                    lit({"to": {name: 1}})
