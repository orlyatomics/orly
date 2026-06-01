#!/usr/bin/env python3
"""
Time-travel + multiverse demo against a running `orlyi`.

Builds a `mainnet` chain of 8 blocks, forks at h=6 into a `fork`
branch (modelling a Bitcoin chain reorg), then queries balances on
both branches at every height. The version axis is encoded in the
key tuple `<['delta', branch, addr, h]>`; cross-branch inheritance
is the `delta_at` recursion in `bitcoin.orly`.

Run via the wrapper:

    ./run.sh

Or directly, after starting `orlyi` separately:

    python3 demo.py

This script also self-checks: it asserts the entire expected balance
trajectory for both branches. CI uses this as a smoke test for the
WebSocket protocol and the Orlyscript package together.
"""

import json
import sys
import websocket

WS_URL = "ws://127.0.0.1:8082/"
SAT_PER_BTC = 100_000_000

# --- The scenario ---------------------------------------------------------

# Mainnet activity, one entry per block. Each entry is a list of
# (address, delta) ops; positive credits, negative debits.
MAINNET_BLOCKS = [
    [("alice", 50_00000000)],                                               # h=1 coinbase
    [("alice", -10_00000000), ("bob", 10_00000000)],                        # h=2
    [("carol", 50_00000000)],                                               # h=3 coinbase
    [("alice", -5_00000000), ("bob", 5_00000000)],                          # h=4
    [("bob", -8_00000000), ("dave", 8_00000000)],                           # h=5 (last common block)
    [("alice", 50_00000000)],                                               # h=6 mainnet: coinbase to alice
    [("alice", -2_00000000), ("carol", 2_00000000)],                        # h=7
    [("alice", -1_00000000), ("dave", 1_00000000)],                         # h=8
]

# Fork: branches at fork_h=6, then has different activity.
FORK_BLOCKS = [
    [("bob", 50_00000000)],                                                 # h=6 fork: coinbase to BOB instead
    [("bob", -20_00000000), ("alice", 20_00000000)],                        # h=7
    [("alice", -5_00000000), ("carol", 5_00000000)],                        # h=8
]
FORK_H = 6

WATCH = ["alice", "bob", "carol", "dave"]

# --- Expected output, for the self-check ---------------------------------

# Balance in BTC for [alice, bob, carol, dave] at each height h on mainnet.
EXPECT_MAINNET = [
    (0, [ 0,  0,  0, 0]),
    (1, [50,  0,  0, 0]),
    (2, [40, 10,  0, 0]),
    (3, [40, 10, 50, 0]),
    (4, [35, 15, 50, 0]),
    (5, [35,  7, 50, 8]),
    (6, [85,  7, 50, 8]),
    (7, [83,  7, 52, 8]),
    (8, [82,  7, 52, 9]),
]

# Same on the fork. Heights 0..5 (pre-fork) must match mainnet exactly.
EXPECT_FORK = [
    (0, [ 0,  0,  0, 0]),
    (1, [50,  0,  0, 0]),
    (2, [40, 10,  0, 0]),
    (3, [40, 10, 50, 0]),
    (4, [35, 15, 50, 0]),
    (5, [35,  7, 50, 8]),
    (6, [35, 57, 50, 8]),
    (7, [55, 37, 50, 8]),
    (8, [50, 37, 55, 8]),
]


# --- WebSocket plumbing ---------------------------------------------------

def send(ws, stmt):
    """Send one Orlyscript statement; raise on non-OK reply."""
    ws.send(stmt)
    reply = json.loads(ws.recv())
    if reply.get("status") != "ok":
        raise RuntimeError(f"{stmt!r}\n  -> {reply}")
    return reply.get("result")


def credit_at(ws, pov, branch, addr, amount, h):
    send(ws, f'try {{{pov}}} bitcoin credit_at '
             f'<{{.branch: "{branch}", .addr: "{addr}", .amount: {amount}, .h: {h}}}>;')


def balance_at(ws, pov, branch, addr, h):
    return send(ws, f'try {{{pov}}} bitcoin balance_at '
                    f'<{{.branch: "{branch}", .addr: "{addr}", .h: {h}}}>;')


def fork_from(ws, pov, branch, parent, fork_h):
    send(ws, f'try {{{pov}}} bitcoin fork_from '
             f'<{{.branch: "{branch}", .parent: "{parent}", .fork_h: {fork_h}}}>;')


# --- Output formatting ----------------------------------------------------

def fmt(satoshi):
    return f"{satoshi / SAT_PER_BTC:>7,.2f}"


def print_chain(ws, pov, branch, max_h):
    header = "  h    " + "  ".join(f"{a:>7}" for a in WATCH)
    print(header)
    print("  " + "-" * (len(header) - 2))
    for h in range(0, max_h + 1):
        bals = [balance_at(ws, pov, branch, addr, h) for addr in WATCH]
        print(f"  {h:>2}   " + "  ".join(fmt(b) for b in bals))


# --- Main -----------------------------------------------------------------

def main():
    ws = websocket.create_connection(WS_URL)
    send(ws, "new session;")
    send(ws, "install bitcoin.1;")
    pov = send(ws, "new safe shared pov;")
    print(f"pov: {pov}\n")

    print(f"applying mainnet ({len(MAINNET_BLOCKS)} blocks)...")
    for h, ops in enumerate(MAINNET_BLOCKS, start=1):
        for addr, delta in ops:
            credit_at(ws, pov, "mainnet", addr, delta, h)

    print(f"forking at h={FORK_H}, applying {len(FORK_BLOCKS)} fork blocks...")
    fork_from(ws, pov, "fork", "mainnet", FORK_H)
    for i, ops in enumerate(FORK_BLOCKS):
        h = FORK_H + i
        for addr, delta in ops:
            credit_at(ws, pov, "fork", addr, delta, h)

    total_h = len(MAINNET_BLOCKS)

    print("\n=== MAINNET ===")
    print_chain(ws, pov, "mainnet", total_h)

    print(f"\n=== FORK (forked from mainnet at h={FORK_H}) ===")
    print_chain(ws, pov, "fork", total_h)

    print("\n=== mainnet vs fork, per height (* = post-fork) ===")
    print("  h     mainnet alice  fork alice    mainnet bob   fork bob")
    print("  " + "-" * 58)
    for h in range(0, total_h + 1):
        ma = balance_at(ws, pov, "mainnet", "alice", h)
        fa = balance_at(ws, pov, "fork", "alice", h)
        mb = balance_at(ws, pov, "mainnet", "bob", h)
        fb = balance_at(ws, pov, "fork", "bob", h)
        marker = "  " if h < FORK_H else " *"
        print(f" {marker}{h:>2}   {fmt(ma):>10}     {fmt(fa):>10}     {fmt(mb):>10}     {fmt(fb):>10}")
    print(f"\n  rows 0..{FORK_H - 1} are identical: reads on `fork` recurse through `<['parent', 'fork']>` into mainnet's keyspace")

    # --- Self-check ------------------------------------------------------
    failures = []
    for h, expected_btc in EXPECT_MAINNET:
        for addr, want_btc in zip(WATCH, expected_btc):
            got = balance_at(ws, pov, "mainnet", addr, h)
            if got != want_btc * SAT_PER_BTC:
                failures.append(f"mainnet h={h} {addr}: got {got}, want {want_btc * SAT_PER_BTC}")
    for h, expected_btc in EXPECT_FORK:
        for addr, want_btc in zip(WATCH, expected_btc):
            got = balance_at(ws, pov, "fork", addr, h)
            if got != want_btc * SAT_PER_BTC:
                failures.append(f"fork h={h} {addr}: got {got}, want {want_btc * SAT_PER_BTC}")

    send(ws, "exit;")
    ws.close()

    if failures:
        print("\n=== self-check FAILED ===")
        for f in failures:
            print(f"  {f}")
        sys.exit(1)
    print("\n=== self-check OK ===")
    print(f"  verified {len(EXPECT_MAINNET) * len(WATCH) * 2} balance values across both branches")


if __name__ == "__main__":
    main()
