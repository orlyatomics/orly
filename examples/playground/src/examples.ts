export interface ExampleCall {
  name: string;
  func: string;
  args: Record<string, unknown>;
  description: string;
}

export interface Example {
  id: string;
  name: string;
  description: string;
  code: string;
  defaultCalls: ExampleCall[];
}

export const EXAMPLES: Record<string, Example> = {
  counter: {
    id: "counter",
    name: "Counter (+= and POVs)",
    description: "Demonstrates Orly's atomic commutative += mutations, absent-key auto-seeding (#151), and isolated POV branch reads.",
    code: `package #1;

/* Read counter by integer id */
read_counter = (*<[id]>::(int)) where {
  id = given::(int);
};

/* Commutative increment using +=
   With Orly's commutative-upsert semantics (#151), an absent key
   auto-seeds from 0 and adds delta with zero lock contention. */
inc = ((true) effecting {
  *<[id]>::(int) += delta;
}) where {
  id    = given::(int);
  delta = given::(int);
};

/* Commutative reset / overwrite */
set_counter = ((true) effecting {
  mutate *<[id]>::(int) <- val;
}) where {
  id  = given::(int);
  val = given::(int);
};

test {
  t1: true;
};
`,
    defaultCalls: [
      {
        name: "Increment counter 1 by 5",
        func: "inc",
        args: { id: 1, delta: 5 },
        description: "Atomically adds 5 to counter 1"
      },
      {
        name: "Increment counter 1 by 10",
        func: "inc",
        args: { id: 1, delta: 10 },
        description: "Atomically adds 10 to counter 1"
      },
      {
        name: "Read counter 1",
        func: "read_counter",
        args: { id: 1 },
        description: "Reads the current cumulative total of counter 1"
      }
    ]
  },

  grc20: {
    id: "grc20",
    name: "GRC-20 Knowledge Graph",
    description: "Append-only property-graph event log with in-engine replay via reduce and exhaustive when matching from examples/grc20-pov.",
    code: `package #1;

op_t  = <| Text(str) | Number(int) | Relation(str) | Deleted |>;
evt_t = <{.ts: int, .editor: str, .op: op_t}>;

/* Commutative append: adds a new event to an entity property's history set */
append_event = ((true) effecting {
  *<['hist', entity, property]>::({evt_t}) |= {<{.ts: ts, .editor: editor, .op: op}>};
}) where {
  entity   = given::(str);
  property = given::(str);
  ts       = given::(int);
  editor   = given::(str);
  op       = given::(op_t);
};

/* Read raw history set */
hist_for = (((*<['hist', entity, property]>::({evt_t})) if *<['hist', entity, property]>::({evt_t}?) is known else (empty {evt_t}))) where {
  entity   = given::(str);
  property = given::(str);
};

/* Sentinels */
forever = 9223372036854775807;

/* In-engine replay: sort by timestamp, reduce to latest event */
resolve_as_of = (((**((((**hist_for(.entity: entity, .property: property)) if (that.ts <= as_of)) as [evt_t]) sorted_by lhs.ts < rhs.ts)) reduce (start seed if false else that)).op) where {
  seed     = <{.ts: -1, .editor: "", .op: <| Text(str) | Number(int) | Relation(str) | Deleted |>.Deleted}>;
  entity   = given::(str);
  property = given::(str);
  as_of    = given::(int);
};

/* Exhaustive when formatting */
display_as_of = ((op) when {
  Text:     op.Text;
  Number:   (op.Number) as str;
  Relation: "-> " + op.Relation;
  Deleted:  "(absent)";
}) where {
  op       = resolve_as_of(.entity: entity, .property: property, .as_of: as_of);
  entity   = given::(str);
  property = given::(str);
  as_of    = given::(int);
};

display = (display_as_of(.entity: entity, .property: property, .as_of: forever)) where {
  entity   = given::(str);
  property = given::(str);
};

test {
  t1: true;
};
`,
    defaultCalls: [
      {
        name: "Set Name to 'Alice'",
        func: "append_event",
        args: {
          entity: "user-1",
          property: "name",
          ts: 1000,
          editor: "admin",
          op: { Text: "Alice" }
        },
        description: "Appends a Text('Alice') property event"
      },
      {
        name: "Update Name to 'Alice Smith'",
        func: "append_event",
        args: {
          entity: "user-1",
          property: "name",
          ts: 2000,
          editor: "admin",
          op: { Text: "Alice Smith" }
        },
        description: "Appends an updated name at t=2000"
      },
      {
        name: "Display current name",
        func: "display",
        args: { entity: "user-1", property: "name" },
        description: "Evaluates current property value folded across event log"
      },
      {
        name: "Time-travel to t=1500",
        func: "display_as_of",
        args: { entity: "user-1", property: "name", as_of: 1500 },
        description: "Replays property state as it stood at t=1500 ('Alice')"
      }
    ]
  },

  market: {
    id: "market",
    name: "Prediction Market",
    description: "Parimutuel prediction market with zero-coordination bets (+=) and price calculation folded across rounds.",
    code: `package #1;

/* Stake on outcome o in market m during round t */
stake_in = ((*<['stake', m, o, t]>::(int) if *<['stake', m, o, t]>::(int?) is known else 0)) where {
  m = given::(str);
  o = given::(str);
  t = given::(int);
};

/* Total pool staked across outcomes in round t */
total_in = ((*<['total', m, t]>::(int) if *<['total', m, t]>::(int?) is known else 0)) where {
  m = given::(str);
  t = given::(int);
};

/* Cumulative stake on outcome o */
staked = ([0..t] reduce start 0 + stake_in(.m: m, .o: o, .t: that)) where {
  m = given::(str);
  o = given::(str);
  t = given::(int);
};

/* Cumulative total pool */
pool = ([0..t] reduce start 0 + total_in(.m: m, .t: that)) where {
  m = given::(str);
  t = given::(int);
};

/* Implied price in basis points (1..10000) */
price = (((10000 * outcome_pool) / total_pool) if total_pool > 0 else 5000) where {
  outcome_pool = staked(.m: m, .o: o, .t: t);
  total_pool   = pool(.m: m, .t: t);
  m = given::(str);
  o = given::(str);
  t = given::(int);
};

/* Place a bet commutatively */
bet = ((true) effecting {
  *<['stake', m, o, t]>::(int) += amount;
  *<['total', m, t]>::(int)    += amount;
  *<['user', m, u, o]>::(int)  += amount;
  *<['book', m]>::({str})      |= {o};
}) where {
  m      = given::(str);
  u      = given::(str);
  o      = given::(str);
  amount = given::(int);
  t      = given::(int);
};

/* User position */
position = ((*<['user', m, u, o]>::(int) if *<['user', m, u, o]>::(int?) is known else 0)) where {
  m = given::(str);
  u = given::(str);
  o = given::(str);
};

test {
  t1: true;
};
`,
    defaultCalls: [
      {
        name: "Bet $100 on YES (Round 0)",
        func: "bet",
        args: { m: "btc-100k", u: "alice", o: "YES", amount: 100, t: 0 },
        description: "Places a $100 stake on YES"
      },
      {
        name: "Bet $50 on NO (Round 0)",
        func: "bet",
        args: { m: "btc-100k", u: "bob", o: "NO", amount: 50, t: 0 },
        description: "Places a $50 stake on NO"
      },
      {
        name: "Check YES Price (basis points)",
        func: "price",
        args: { m: "btc-100k", o: "YES", t: 0 },
        description: "Calculates implied probability (e.g. 6666 bps = 66.6%)"
      },
      {
        name: "Check Total Pool",
        func: "pool",
        args: { m: "btc-100k", t: 0 },
        description: "Reads total staked pool across all outcomes"
      }
    ]
  }
};
