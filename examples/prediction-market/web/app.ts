// Browser UI for the parimutuel prediction market, on the orly TypeScript
// client (clients/ts). The SDK is browser-native here -- `connect` uses the
// global WebSocket -- so this talks straight to a running `orlyi`.
//
// The point of the UI: open it in two tabs (same #pov= link) and you're two
// traders on the SAME market. Click to bet; the price moves live, with zero
// coordination between tabs -- because every bet is a commutative `+=` in the
// engine and the price is a read-time fold of the trade log.
//
// Built with esbuild (see build.sh). NOTE: written but not verified in a real
// browser in this repo's sandbox -- run it and confirm.

import { connect, Client } from "orly";

const MARKET = "live-market";
const QUESTION = "Will Orly ship by Friday?";
const OUTCOMES = ["YES", "NO"] as const;
const BET = 10; // each click stakes $10
const ROUND = 0; // single live round; the Node demo (../demo.ts) shows time-travel across rounds

let client: Client;
let pov = "";
const me = "u-" + Math.random().toString(36).slice(2, 8);

function el(id: string): HTMLElement {
  const e = document.getElementById(id);
  if (!e) throw new Error(`missing #${id}`);
  return e;
}

// Share a market across tabs via the URL hash (#pov=<id>): the first tab
// creates the POV, later tabs opened on the same link reuse it.
async function ensurePov(): Promise<string> {
  const params = new URLSearchParams(location.hash.slice(1));
  let id = params.get("pov");
  if (!id) {
    id = await client.newPov();
    location.hash = "pov=" + id;
  }
  return id;
}

async function refresh(): Promise<void> {
  try {
    const pool = Number(await client.call(pov, "market", "pool", { m: MARKET, t: ROUND }));
    const rows = await Promise.all(
      OUTCOMES.map(async (o) => {
        const bps = Number(await client.call(pov, "market", "price", { m: MARKET, o, t: ROUND }));
        const mine = Number(await client.call(pov, "market", "position", { m: MARKET, u: me, o }));
        return { o, bps, mine };
      }),
    );
    render(pool, rows);
    el("status").textContent = "live";
  } catch (e) {
    el("status").textContent = "error: " + (e as Error).message;
  }
}

function render(pool: number, rows: { o: string; bps: number; mine: number }[]): void {
  el("pool").textContent = "$" + pool;
  el("market").innerHTML = rows
    .map((r) => {
      const pct = (r.bps / 100).toFixed(1);
      const yours = r.mine ? ` · you $${r.mine}` : "";
      return `<div class="outcome">
        <div class="bar"><div class="fill ${r.o}" style="width:${pct}%"></div>
          <span class="lbl">${r.o} — ${pct}%${yours}</span></div>
        <button data-o="${r.o}">Buy ${r.o} $${BET}</button>
      </div>`;
    })
    .join("");
  for (const btn of Array.from(document.querySelectorAll<HTMLButtonElement>("#market button"))) {
    btn.onclick = () => bet(btn.dataset.o as string);
  }
}

async function bet(o: string): Promise<void> {
  await client.call(pov, "market", "place_bet", { m: MARKET, o, u: me, amount: BET, t: ROUND });
  await refresh();
}

async function main(): Promise<void> {
  el("question").textContent = QUESTION;
  el("me").textContent = me;
  client = await connect(); // browser: native WebSocket to ws://127.0.0.1:8082/
  await client.newSession();
  await client.install("market", 0); // idempotent: no-op if already installed
  pov = await ensurePov();
  el("share").textContent = location.href;
  await refresh();
  setInterval(refresh, 1000); // poll so other tabs' bets appear live
}

main().catch((e) => {
  document.body.innerHTML = "<pre>" + (e as Error).stack + "</pre>";
});
