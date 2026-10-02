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
