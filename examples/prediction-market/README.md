# `examples/prediction-market/` — a parimutuel prediction market ("Polymarket clone")

The synthesis demo: a small prediction market that exercises **all three** of
Orly's properties at once — concurrent writes with zero coordination,
event-sourced fold-on-read, and native time-travel — wrapped in a recognizable
app and built on the TypeScript client ([`clients/ts`](../../clients/ts)).

```
8 traders, 3 rounds, outcomes YES/NO

  round 0: pool $220   YES 65.9%   NO 34.1%
  round 1: pool $496   YES 71.0%   NO 29.0%
  round 2: pool $828   YES 66.7%   NO 33.3%

=== price history (time-travel: YES probability as of each round) ===
  as of round 0: YES 65.9%
  ...

=== resolution: YES wins ===
  trader-6: staked $101 on YES -> paid $151
  total pool $828, total paid out $825

=== self-check OK ===
```

## The mechanism

It's a **parimutuel pool**, not an order book: a bet adds `amount` to the chosen
outcome's pool; an outcome's implied probability is `pool_outcome / pool_total`;
on resolution, holders of the winning outcome split the whole pool in proportion
to their stake. (A real Polymarket is a central-limit order book — this is the
reduced mechanism that fits a demo.)

That mechanism is the point, because it is **entirely commutative**. Every write
in [`market.orly`](market.orly) is a `+=` (pools, positions) or `|=` (the outcome
set):

```
place_bet = ((1) effecting {
  *<['stake', m, o, t]>::(int) += amount;   // outcome pool, this round
  *<['total', m, t]>::(int)    += amount;   // total pool, this round
  *<['user',  m, u, o]>::(int) += amount;   // the bettor's position
  *<['book',  m]>::({str})     |= {o};      // outcomes seen
}) where { ... };
```

So **N traders can bet on the same market at the same instant, each on its own
session, with no locks and no conflict-resolution code, and not one bet is lost.**
The driver fires every trader's bet for a round with `Promise.all` — real
contention on the shared per-round keys — and the self-check confirms the pool
and every price match an independently-computed ground truth.

The **price is a read**, not a stored value: `[0..t] reduce start 0 + per_round`
folds the trade log, so a past price is just a query at an earlier `t` —
that's the time-travel section, verified to be stable. Resolution + payout are
likewise folds (`market.orly`'s `payout`).

## Run

```sh
./run-ts.sh         # builds the TS client, compiles market.orly, starts orlyi,
                    # builds + runs demo.ts, self-checks
```

Needs a debug build (`make debug`) and Node 18+. The orlyscript logic also has
inline tests: `orlyc -d market.orly`.

## Files

- [`market.orly`](market.orly) — the engine: commutative `place_bet`, fold reads
  (`price`/`pool`/`position`), `resolve`/`payout`, with a `round` version axis for
  time-travel. Inline `test {}` block.
- [`demo.ts`](demo.ts) — the driver on the `orly` TS client: concurrent traders,
  live prices, time-travel price history, resolution + payouts, self-check.
