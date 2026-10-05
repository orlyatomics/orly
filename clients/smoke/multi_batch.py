"""Mixed-batch smoke, Python driver (#255); run by run-multi-batch.sh."""
import os

import orly

c = orly.connect(os.environ["ORLY_URL"], timeout=10, recv_timeout=60)
c.new_session()
c.install("multi", 1)
pov = c.new_pov()

# Two methods with different return types in one transaction.
results = c.call_many(pov, [
    ("multi", "write_val", {"n": 1, "x": 10}),
    ("multi", "write_name", {"n": 1, "s": "alpha"}),
    ("multi", "write_val", {"n": 2, "x": 20}),
])
assert results == [True, "named", True], results
assert c.call(pov, "multi", "read_val", {"n": 1}) == 10
assert c.call(pov, "multi", "read_val", {"n": 2}) == 20
assert c.call(pov, "multi", "read_name", {"n": 1}) == "alpha"
print("py: mixed batch landed:", results)

# All-or-nothing: the second call names a method that doesn't exist, so the
# first call's write must not land either.
try:
    c.call_many(pov, [
        ("multi", "write_val", {"n": 3, "x": 30}),
        ("multi", "no_such_method", {}),
    ])
    raise SystemExit("py: a batch with a bad call was accepted")
except orly.OrlyError:
    pass
assert c.call(pov, "multi", "read_val", {"n": 3}) is None, "py: a failed batch left a write behind"
print("py: failed batch left nothing behind")

# Reads in a batch see the pre-batch snapshot, not each other's writes.
results = c.call_many(pov, [
    ("multi", "write_val", {"n": 4, "x": 40}),
    ("multi", "read_val", {"n": 4}),
])
assert results == [True, None], results
assert c.call(pov, "multi", "read_val", {"n": 4}) == 40
print("py: batch reads see the pre-batch snapshot")

# The same-method batch (#253) shares the code path; it still returns one list.
assert c.call_batch(pov, "multi", "write_val", [{"n": 5, "x": 50}, {"n": 6, "x": 60}]) == [True, True]
assert [c.call(pov, "multi", "read_val", {"n": n}) for n in (5, 6)] == [50, 60]
print("py: same-method batch still works")
c.close()
