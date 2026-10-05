"""Python half of the new-POV smoke (#580); run by run-new-pov.sh."""

import os
import sys

import orly

c = orly.connect(os.environ["ORLY_URL"])
c.new_session()
c.install("sample", 1)

n = 0
for safe in (True, False):
    for shared in (True, False):
        for parent in (None, "parent"):
            n += 1
            kind = f"safe={safe} shared={shared} parent={parent is not None}"
            pov = c.new_pov(safe=safe, shared=shared,
                            parent=c.new_pov() if parent else None)
            c.call(pov, "sample", "write_val", {"n": n, "x": n * 11})
            got = c.call(pov, "sample", "read_val", {"n": n})
            if got != n * 11:
                sys.exit(f"SMOKE FAIL (python) {kind}: read {got!r}, wrote {n * 11}")
            print(f"ok (python) {kind}")

# #625: pause and unpause must reach the server in its own syntax, and the POV
# must still take a write and read it back afterwards.
pov = c.new_pov(shared=True, parent=c.new_pov())
for op, want in (("pause", "paused"), ("unpause", "unpaused")):
    got = getattr(c, op)(pov)
    if got != want:
        sys.exit(f"SMOKE FAIL (python) {op}: got {got!r}, want {want!r}")
c.call(pov, "sample", "write_val", {"n": 1000, "x": 7})
got = c.call(pov, "sample", "read_val", {"n": 1000})
if got != 7:
    sys.exit(f"SMOKE FAIL (python) after pause/unpause: read {got!r}, wrote 7")
print("ok (python) pause/unpause")
