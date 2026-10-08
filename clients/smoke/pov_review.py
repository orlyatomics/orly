"""POV review smoke, Python client (#746); run by run-pov-review.sh after pov_review.mjs.

A report-mode private POV under the global POV: paused, it overwrites a key the parent changed
after the fork, deletes one, adds one and bumps a counter; its diff (whole, by range and paged)
must list exactly those changes; promoting it must report the one conflict.  Then a private POV
discards its paused writes and reads as its parent again."""

import os
import sys
import time

import orly

PKG = "pov_review"
G = 100
failures = 0


def check(what, got, want):
    global failures
    if got != want:
        failures += 1
        print(f"POV REVIEW PY FAIL: {what}: got {got!r}, want {want!r}")


def num(v):
    return round(v) if isinstance(v, float) else v


def norm(change):
    out = {"key": [num(x) for x in change["key"]], "kind": change["kind"],
           "before": num(change["before"]), "after": num(change["after"])}
    if change["kind"] == "delta":
        out["op"] = change["op"]
        out["delta"] = num(change["delta"])
    return out


def wait_for(what, read, want, timeout=30.0):
    deadline = time.monotonic() + timeout
    while True:
        got = read()
        if got == want:
            return
        if time.monotonic() > deadline:
            check(f"{what} ({timeout:.0f} s)", got, want)
            return
        time.sleep(0.1)


with orly.connect(os.environ["ORLY_URL"]) as c:
    c.new_session()
    gw = c.new_pov(safe=False, shared=False)
    gr = c.new_pov(safe=False, shared=False)
    get = lambda pov, e: num(c.call(pov, PKG, "get", {"g": G, "e": e}))
    c.call_batch(gw, PKG, "put", [{"g": G, "e": e, "w": e * 10} for e in range(4)])
    c.call(gw, PKG, "set_count", {"g": G, "k": 0, "n": 5})
    wait_for("base", lambda: [get(gr, e) for e in range(4)], [0, 10, 20, 30])

    p = c.new_pov(safe=False, shared=False, conflicts="report")
    c.pause(p)
    c.call(gw, PKG, "put", {"g": G, "e": 1, "w": 1000})
    wait_for("the parent's change", lambda: get(gr, 1), 1000)
    c.call(p, PKG, "put", {"g": G, "e": 1, "w": 111})
    c.call(p, PKG, "remove", {"g": G, "e": 2})
    c.call(p, PKG, "put", {"g": G, "e": 9, "w": 9})
    c.call(p, PKG, "bump", {"g": G, "k": 0, "n": 2})
    want = [
        {"key": ["count", G, 0], "kind": "delta", "before": 5, "after": 7, "op": "add", "delta": 2},
        {"key": ["edge", G, 1], "kind": "changed", "before": 1000, "after": 111},
        {"key": ["edge", G, 2], "kind": "removed", "before": 20, "after": None},
        {"key": ["edge", G, 9], "kind": "added", "before": None, "after": 9},
    ]
    check("diff", [norm(x) for x in c.diff(p)["changes"]], want)
    check("ranged diff", [norm(x) for x in c.diff(p, start=("edge", G), stop=("edge", G, 5))["changes"]], want[1:3])
    pages = [norm(x) for page in c.diff_pages(p, limit=1) for x in page]
    check("paged diff", pages, want)
    promoted = c.promote(p)
    check("promote", [promoted["status"], [(num(x["number"]), [num(k) for k in x["key"]], x["op"]) for x in promoted["conflicts"]]],
          ["promoted", [(1, ["edge", G, 1], "put")]])
    wait_for("promoted", lambda: [get(gr, 1), get(gr, 2), get(gr, 9)], [111, -1, 9])

    d = c.new_pov(safe=True, shared=False)
    c.pause(d)
    c.call_batch(d, PKG, "put", [{"g": G, "e": 50 + i, "w": i} for i in range(20)])
    check("discard", c.discard(d), {"discarded_updates": 1.0, "discarded_entries": 20.0})
    check("discarded", [get(d, 50), c.diff(d)["changes"]], [-1, []])

if failures:
    print(f"POV REVIEW PY FAIL: {failures} checks failed")
    sys.exit(1)
print("POV REVIEW PY OK")
