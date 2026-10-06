"""Read-budget smoke, Python driver (#694); run by run-read-budget.sh.

With BUDGETED=1 orlyi runs with --read_budget_rows=ROW_LIMIT and
--read_budget_mb=1: reads over more rows than that, or building more than
1 MiB of result, must raise orly.ReadTooLarge, while reads under the budget
answer as before and the server keeps serving. With BUDGETED=0 the budget is
off (--read_budget_mb=0) and the same reads must succeed, so the refusals are
the budget's doing."""
import os

import orly

ROWS = int(os.environ["ROWS"])
ROW_LIMIT = int(os.environ["ROW_LIMIT"])
BIG = int(os.environ["BIG"])
BUDGETED = os.environ["BUDGETED"] == "1"
c = orly.connect(os.environ["ORLY_URL"], timeout=10, recv_timeout=120)
c.new_session()
c.install("read_budget", 1)
pov = c.new_pov()
for i in range(0, ROWS, 1000):
    c.call_batch(pov, "read_budget", "write_val", [{"n": j, "x": 1} for j in range(i, min(i + 1000, ROWS))])
print(f"py: wrote {ROWS} rows")


def refused(method, args):
    try:
        got = c.call(pov, "read_budget", method, args)
    except orly.ReadTooLarge as err:
        assert err.reply["status"] == "read_too_large", err.reply
        assert err.reply["result"].startswith("read too large"), err.reply
        return err.reply["result"]
    except orly.OrlyError as err:
        raise SystemExit(f"READ BUDGET FAIL (py): {method} raised {type(err).__name__}, "
                         f"not ReadTooLarge: {err.reply}")
    raise SystemExit(f"READ BUDGET FAIL (py): {method} was answered ({str(got)[:80]}) over the budget")


if BUDGETED:
    print("py: range read refused:", refused("count_all", {}))
    print("py: point-read loop refused:", refused("sum_points", {"last": ROWS - 1}))
    print("py: big result refused:", refused("big_list", {"n": BIG}))
    # Under the budget, and after the refusals, reads answer as before.
    assert c.call(pov, "read_budget", "read_val", {"n": 7}) == 1
    assert c.call(pov, "read_budget", "sum_points", {"last": ROW_LIMIT // 2 - 1}) == ROW_LIMIT // 2
    assert len(c.call(pov, "read_budget", "big_list", {"n": 1000})) == 1001
else:
    assert c.call(pov, "read_budget", "count_all", {}) == ROWS
    assert c.call(pov, "read_budget", "sum_points", {"last": ROWS - 1}) == ROWS
    assert len(c.call(pov, "read_budget", "big_list", {"n": BIG})) == BIG + 1
print(f"READ BUDGET OK (py, budgeted={int(BUDGETED)})")
