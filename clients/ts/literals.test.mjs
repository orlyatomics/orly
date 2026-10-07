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
import { lit } from "./dist/index.js";

test("keyword fields are emitted unchanged", () => {
  for (const name of ["id", "to", "from", "start", "after", "if", "true",
                      "int", "and", "keys", "try", "safe", "list_packages"]) {
    assert.equal(lit({ [name]: 1 }), `<{.${name}: 1}>`);
  }
  assert.equal(lit({ to: { id: 7 } }), "<{.to: <{.id: 7}>}>");
  assert.equal(lit({}), "<{}>");
  assert.equal(lit({ _id2: true }), "<{._id2: true}>");
});

test("invalid field names fail locally with a rename suggestion", () => {
  for (const name of ["", "1st", "first-name", "a.b", "é", "id\n"]) {
    assert.throws(() => lit({ [name]: 1 }), (error) =>
      error instanceof TypeError &&
      error.message.includes(JSON.stringify(name)) &&
      error.message.includes("rename"));
    assert.throws(() => lit({ to: { [name]: 1 } }), /invalid record field name/);
  }
});
