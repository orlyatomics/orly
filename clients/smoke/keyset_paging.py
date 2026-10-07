"""Keyset-paging smoke, Python driver (#735); run by run-keyset-paging.sh after
keyset_paging.mjs has written group 1's edges. Client.pages walks them all, once each and in
order: the cursor comes back as a float (1.0) and must go back as an int."""
import os

import orly

ROWS = int(os.environ["ROWS"])
PAGE = int(os.environ["PAGE"])

c = orly.connect(os.environ["ORLY_URL"], timeout=10, recv_timeout=60)
c.new_session()
c.install("keyset_paging", 1)
pov = c.new_pov()

seen = []
pages = 0
for rows in c.pages(pov, "keyset_paging", "page", {"g": 1, "last": -1, "n": PAGE}, page_size=PAGE):
    pages += 1
    seen.extend(int(r["e"]) for r in rows)
assert seen == list(range(ROWS)), "py: pages saw %d edges, not 0 .. %d in order" % (len(seen), ROWS - 1)

# Without page_size it stops at the first empty page.
tail = [int(r["e"]) for rows in c.pages(pov, "keyset_paging", "page", {"g": 1, "last": ROWS - 5, "n": 3})
        for r in rows]
assert tail == list(range(ROWS - 4, ROWS)), "py: the last pages saw %r" % (tail,)
try:
    next(c.pages(pov, "keyset_paging", "page", {"g": 1, "n": 3}))
    raise AssertionError("py: pages without a first cursor did not raise")
except TypeError:
    pass
print("py: pages ok: %d edges in %d pages" % (len(seen), pages))
c.close()
