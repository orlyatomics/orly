#!/usr/bin/env python3
"""Recursive sum-type storage + client marshaling demo (issue #115), in Python.

Connects to a running orlyi over the WebSocket protocol, stores a fixed
recursive value of each shape (self-recursive tree, container-of-self doc,
mutual tree/forest, nested-variant tree), reads it back, and self-checks the
marshaled JSON. A variant marshals as {"Tag": <payload>}; a tag-only arm as
{"Tag": {}}; numbers come back as JSON floats.

Uses the shared `orly` client (clients/python). Run via the wrapper: ./run.sh
Or directly, after starting orlyi separately:  python3 demo.py
"""

import sys

import orly


# Each case: (label, put-method, get-method, expected round-tripped JSON).
CASES = [
    (
        "tree  (self-reference in a record arm, #103)",
        "put_tree", "get_tree",
        {"Branch": {"l": {"Leaf": 1.0},
                    "r": {"Branch": {"l": {"Leaf": 2.0}, "r": {"Leaf": 3.0}}}}},
    ),
    (
        "doc   (self-reference under a list, #120)",
        "put_doc", "get_doc",
        {"Arr": [{"Num": 1.0}, {"Str": "two"}, {"Arr": [{"Num": 3.0}]}, {"Null": {}}]},
    ),
    (
        "mtree (mutual recursion, #116)",
        "put_mtree", "get_mtree",
        {"MNode": {"FCons": {"head": {"MLeaf": 1.0},
                             "tail": {"FCons": {"head": {"MLeaf": 2.0},
                                                "tail": {"FEmpty": {}}}}}}},
    ),
    (
        "ntree (nested-variant arm, #125)",
        "put_ntree", "get_ntree",
        {"NBranch": {"Un": {"NLeaf": 7.0}}},
    ),
]


def main():
    c = orly.connect()
    c.new_session()
    c.install("recursive", 0)
    pov = c.new_pov()
    print(f"pov: {pov}\n")

    failures = 0
    for kk, (label, put, get, expected) in enumerate(CASES):
        c.call(pov, "recursive", put, {"k": kk})
        got = c.call(pov, "recursive", get, {"k": kk})
        ok = got == expected
        print(f"  [{'ok' if ok else 'FAIL'}] {label}")
        if not ok:
            import json
            print(f"        expected: {json.dumps(expected)}")
            print(f"        got:      {json.dumps(got)}")
            failures += 1

    c.exit()

    if failures:
        sys.exit(f"\n{failures} case(s) failed.")
    print("\nAll recursive-variant values round-tripped through storage and marshaled correctly.")


if __name__ == "__main__":
    main()
