// A small parimutuel prediction market ("Polymarket clone") on Orly, in
// TypeScript, on the shared `orly` client (clients/ts).
//
// The point: N traders bet on the same market *concurrently* -- each on its
// own WebSocket session, all into one shared POV -- with zero coordination
// and zero conflict-resolution code. Every bet is a commutative `+=` in the
// engine (market.orly), so nothing is lost under contention. The implied
// probabilities are a read-time fold, and any past price is a query
// (time-travel). On resolution the winners split the pool parimutuel-style.
//
// Run via the wrapper: ./run-ts.sh

import { connect } from "orly";

const MARKET = "will-orly-ship";
const OUTCOMES = ["YES", "NO"] as const;
const NUM_TRADERS = 8;
const ROUNDS = 3;

// Deterministic bet for (round, trader): which outcome, and how much. Kept
// deterministic so the self-check has an exact ground truth -- but the bets
// are *placed* concurrently, so the engine still has to survive real
// contention on the shared per-round pool keys.
function betFor(round: number, trader: number): { outcome: "YES" | "NO"; amount: number } {
  const outcome = (trader + round) % 3 === 0 ? "NO" : "YES";
  const amount = 10 + trader * 5 + round * 7;
  return { outcome, amount };
}

// Integer division matching orlyscript's (truncating; all values are positive).
const idiv = (a: number, b: number): number => (b === 0 ? 0 : Math.floor(a / b));

async function placeBet(pov: string, round: number, trader: number): Promise<void> {
  // Each trader is its own session/connection -- this is what makes the bets
  // genuinely concurrent on the shared market keys.
  const c = await connect();
  try {
    await c.newSession();
    const { outcome, amount } = betFor(round, trader);
    await c.call(pov, "market", "place_bet", {
      m: MARKET,
      o: outcome,
      u: `trader-${trader}`,
      amount,
      t: round,
    });
  } finally {
    c.close();
  }
}

async function main(): Promise<void> {
  const boot = await connect();
  await boot.newSession();
  await boot.install("market", 0);
  const pov = await boot.newPov();
  console.log(`market: ${MARKET}   pov: ${pov}`);
  console.log(`${NUM_TRADERS} traders, ${ROUNDS} rounds, outcomes ${OUTCOMES.join("/")}\n`);

  // Ground truth, accumulated as we go (cumulative pools through each round).
  const stake: Record<string, number> = { YES: 0, NO: 0 }; // cumulative per outcome
  const position: Record<string, Record<string, number>> = {}; // user -> outcome -> stake
  const priceHistory: number[] = []; // engine YES price (bps) as of each round

  for (let round = 0; round < ROUNDS; round++) {
    // Fire every trader's bet for this round *concurrently*.
    await Promise.all(
      Array.from({ length: NUM_TRADERS }, (_, trader) => placeBet(pov, round, trader)),
    );

    // Update ground truth for this round.
    for (let trader = 0; trader < NUM_TRADERS; trader++) {
      const { outcome, amount } = betFor(round, trader);
      stake[outcome] += amount;
      const u = `trader-${trader}`;
      (position[u] ??= { YES: 0, NO: 0 })[outcome] += amount;
    }

    // Read the live market back from the engine, as of this round.
    const yesBps = Number(await boot.call(pov, "market", "price", { m: MARKET, o: "YES", t: round }));
    const noBps = Number(await boot.call(pov, "market", "price", { m: MARKET, o: "NO", t: round }));
    const pool = Number(await boot.call(pov, "market", "pool", { m: MARKET, t: round }));
    priceHistory.push(yesBps);
    console.log(
      `  round ${round}: pool $${pool}` +
        `   YES ${(yesBps / 100).toFixed(1)}%   NO ${(noBps / 100).toFixed(1)}%`,
    );
  }

  const last = ROUNDS - 1;
  const totalPool = stake.YES + stake.NO;

  // --- self-check 1: no bet lost under concurrency (pools match ground truth) ---
  const failures: string[] = [];
  const engPool = Number(await boot.call(pov, "market", "pool", { m: MARKET, t: last }));
  if (engPool !== totalPool) failures.push(`pool: engine ${engPool}, expected ${totalPool}`);
  for (const o of OUTCOMES) {
    const engStaked = idiv(
      Number(await boot.call(pov, "market", "price", { m: MARKET, o, t: last })) * totalPool,
      10000,
    );
    // Recompute expected price the same integer way and compare prices directly.
    const expBps = idiv(stake[o] * 10000, totalPool);
    const gotBps = Number(await boot.call(pov, "market", "price", { m: MARKET, o, t: last }));
    if (gotBps !== expBps) failures.push(`price[${o}]: engine ${gotBps}, expected ${expBps}`);
    void engStaked;
  }

  // --- time-travel: the YES probability trajectory across rounds ---
  console.log("\n=== price history (time-travel: YES probability as of each round) ===");
  for (let round = 0; round < ROUNDS; round++) {
    const bps = Number(await boot.call(pov, "market", "price", { m: MARKET, o: "YES", t: round }));
    if (bps !== priceHistory[round]) {
      failures.push(`time-travel price[YES, round ${round}] drifted: ${bps} vs ${priceHistory[round]}`);
    }
    console.log(`  as of round ${round}: YES ${(bps / 100).toFixed(1)}%`);
  }

  // --- resolve YES, pay out parimutuel, and verify ---
  console.log("\n=== resolution: YES wins ===");
  await boot.call(pov, "market", "resolve", { m: MARKET, o: "YES" });
  const winner = String(await boot.call(pov, "market", "winner", { m: MARKET }));
  if (winner !== "YES") failures.push(`winner: ${winner}, expected YES`);

  let paidOut = 0;
  for (let trader = 0; trader < NUM_TRADERS; trader++) {
    const u = `trader-${trader}`;
    const got = Number(await boot.call(pov, "market", "payout", { m: MARKET, u, t: last }));
    const expected = idiv((position[u]?.YES ?? 0) * totalPool, stake.YES);
    if (got !== expected) failures.push(`payout[${u}]: engine ${got}, expected ${expected}`);
    paidOut += got;
    if ((position[u]?.YES ?? 0) > 0) {
      console.log(`  ${u}: staked $${position[u].YES} on YES -> paid $${got}`);
    }
  }
  // Parimutuel: total paid out should equal the whole pool (modulo integer
  // truncation across winners).
  console.log(`  total pool $${totalPool}, total paid out $${paidOut}`);
  if (paidOut > totalPool || paidOut < totalPool - NUM_TRADERS) {
    failures.push(`payout conservation: paid ${paidOut}, pool ${totalPool}`);
  }

  await boot.exit();

  if (failures.length > 0) {
    console.error("\n=== self-check FAILED ===");
    for (const f of failures) console.error(`  ${f}`);
    process.exit(1);
  }
  console.log("\n=== self-check OK ===");
  console.log(
    `  ${NUM_TRADERS} traders bet concurrently across ${ROUNDS} rounds into one shared market;`,
  );
  console.log(
    `  every bet landed (pool + per-outcome prices exact), the price history is stable under`,
  );
  console.log(`  time-travel, and the parimutuel payouts conserve the $${totalPool} pool.`);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
