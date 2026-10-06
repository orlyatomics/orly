"""Write-too-large smoke, Python driver (#687); run by run-write-too-large.sh.

A batch with more entries than half the Update Entry pool's merge reserve must
raise orly.WriteTooLarge (not InsufficientMemory); the same rows in batches
under the limit must be accepted."""
import os
import time

import orly

LIMIT = int(os.environ["LIMIT"])
c = orly.connect(os.environ["ORLY_URL"], timeout=10, recv_timeout=60)
c.new_session()
c.install("sample", 1)
pov = c.new_pov()

total = 2 * LIMIT + 2
rows = [{"n": 5000 + j, "x": 9} for j in range(total)]
try:
    c.call_batch(pov, "sample", "write_val", rows)
    raise SystemExit(f"WRITE TOO LARGE FAIL (py): a {total}-entry batch was accepted")
except orly.WriteTooLarge as err:
    assert not isinstance(err, orly.InsufficientMemory)
    assert err.reply["status"] == "write_too_large", err.reply
    assert err.reply["result"].startswith("write too large"), err.reply
    print("py: refused", total, "entries")
except orly.OrlyError as err:
    raise SystemExit(f"WRITE TOO LARGE FAIL (py): a {total}-entry batch raised "
                     f"{type(err).__name__}, not WriteTooLarge: {err.reply}")
assert c.call(pov, "sample", "read_val", {"n": 5000}) != 9, "the refused batch left a write behind"

for i in range(0, total, LIMIT):
    for tries in range(300):
        try:
            c.call_batch(pov, "sample", "write_val", rows[i:i + LIMIT])
            break
        except orly.InsufficientMemory:
            time.sleep(0.1)
    else:
        raise SystemExit("WRITE TOO LARGE FAIL (py): a batch under the limit stayed refused")
assert c.call(pov, "sample", "read_val", {"n": 5000 + total - 1}) == 9
print("WRITE TOO LARGE OK (py)")
