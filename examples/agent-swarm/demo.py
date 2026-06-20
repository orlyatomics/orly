#!/usr/bin/env python3
"""
Multi-agent knowledge-graph demo, in Python.

Spawns N concurrent WebSocket "agents" that each process a disjoint
slice of a corpus, extracting entities + tags + cooccurrences and
streaming them into a single shared POV via Orly's commutative field
calls (`+=`, `|=`). No coordination between agents -- no locks, no
per-agent partitioning, no merge-on-read in the driver.

The demo's AI angle: this is exactly the shape of a multi-agent
LLM-extraction pipeline. Many agents read disjoint chunks of a corpus,
each emits structured facts about overlapping entities, and the
naive approach (read-modify-write per fact) drops updates the moment
two agents touch the same entity at the same time. Orly's field calls
don't read-modify-write -- the database aggregates increments and
set-unions correctly under any interleaving.

For demo simplicity, the "extraction" is deterministic: each doc in
the corpus comes pre-tagged with the entities and tags an extractor
would produce. So the headline property (concurrent writes land
correctly) is testable without an actual LLM in the loop.

Run via the wrapper:

    ./run.sh

Or directly, after starting orlyi separately:

    python3 demo.py
"""

import json
import os
import sys
import threading
import time
import websocket

WS_URL = "ws://127.0.0.1:8082/"

# Per-call recv timeout. Mirrors wikipedia-pageviews/demo.py: normal
# latency is well under a second; 30s is generous-but-finite so a hung
# orlyi surfaces as a failed read rather than a multi-minute CI timeout.
WS_TIMEOUT_S = 30

# ----------------------------------------------------------------------
# The corpus. Each entry is (sentence text, {entity: [tags]}).
#
# The sentence text is human-readable flavor only -- the structured
# {entity: [tags]} dict IS the "extraction" the demo treats as ground
# truth. An entity may carry multiple tags from a single doc (different
# extractors infer different attributes); the same entity across many
# docs accumulates a UNION of all tags it ever received.
#
# Hot entities (Python, OpenAI, Anthropic, Claude, GPT-4, Microsoft,
# Google) appear in many docs to drive real contention on the
# `+=` mention counters and the `|=` tag sets.
# ----------------------------------------------------------------------
CORPUS = [
    ("Python and Rust dominate modern systems work at OpenAI.",
     {"Python": ["language", "popular"], "Rust": ["language"],
      "OpenAI": ["company", "ai-lab"]}),
    ("Anthropic trained Claude on a custom Transformer architecture.",
     {"Anthropic": ["company", "ai-lab"], "Claude": ["model", "llm"],
      "Transformer": ["model"]}),
    ("GitHub Copilot now offers Claude alongside GPT-4 for code.",
     {"GitHub": ["company", "tool"], "Claude": ["model", "llm"],
      "GPT-4": ["model", "llm"]}),
    ("Linus Torvalds still maintains Linux from a laptop running Git.",
     {"Linus": ["person"], "Linux": ["tool", "open-source"],
      "Git": ["tool", "open-source"]}),
    ("Guido van Rossum joined Microsoft to improve Python performance.",
     {"Guido": ["person"], "Microsoft": ["company"],
      "Python": ["language", "popular"]}),
    ("Kubernetes manages workloads at Google, Meta, and Microsoft.",
     {"Kubernetes": ["tool"], "Google": ["company"],
      "Meta": ["company"], "Microsoft": ["company"]}),
    ("Docker simplified deployment for Python and JavaScript developers.",
     {"Docker": ["tool"], "Python": ["language", "popular"],
      "JavaScript": ["language", "popular"]}),
    ("Meta released Llama as a competitive open-source LLM.",
     {"Meta": ["company", "ai-lab"],
      "Llama": ["model", "llm", "open-source"]}),
    ("PostgreSQL remains the database of choice at OpenAI for analytics.",
     {"PostgreSQL": ["tool", "database"], "OpenAI": ["company", "ai-lab"]}),
    ("Sam Altman discussed GPT-4 in a recent OpenAI announcement.",
     {"Sam": ["person"], "GPT-4": ["model", "llm"],
      "OpenAI": ["company", "ai-lab"]}),
    ("Rust is gaining traction at Microsoft for systems-level libraries.",
     {"Rust": ["language"], "Microsoft": ["company"]}),
    ("TypeScript powers most modern JavaScript codebases at Google.",
     {"TypeScript": ["language"], "JavaScript": ["language", "popular"],
      "Google": ["company"]}),
    ("Java still dominates enterprise software at Microsoft and Google.",
     {"Java": ["language", "enterprise"], "Microsoft": ["company"],
      "Google": ["company"]}),
    ("Ruby on Rails was created by a Danish developer; GitHub uses it.",
     {"Ruby": ["language"], "GitHub": ["company", "tool"]}),
    ("Go was designed at Google to simplify backend programming.",
     {"Go": ["language"], "Google": ["company"]}),
    ("Kubernetes orchestrates Docker containers in production at Meta.",
     {"Kubernetes": ["tool"], "Docker": ["tool"],
      "Meta": ["company", "ai-lab"]}),
    ("Claude can analyze long documents with its large context window.",
     {"Claude": ["model", "llm"]}),
    ("GPT-4 powers many tools released by OpenAI in recent years.",
     {"GPT-4": ["model", "llm"], "OpenAI": ["company", "ai-lab"]}),
    ("Linux servers run most of the infrastructure at Google.",
     {"Linux": ["tool", "open-source"], "Google": ["company"]}),
    ("Git is the version control system at Microsoft, Google, and Anthropic.",
     {"Git": ["tool", "open-source"], "Microsoft": ["company"],
      "Google": ["company"], "Anthropic": ["company", "ai-lab"]}),
    ("Llama runs locally on a laptop with sufficient RAM.",
     {"Llama": ["model", "llm", "open-source"]}),
    ("PostgreSQL backs most production systems at GitHub and Anthropic.",
     {"PostgreSQL": ["tool", "database"], "GitHub": ["company", "tool"],
      "Anthropic": ["company", "ai-lab"]}),
    ("Python's ecosystem includes scientific tools widely used at OpenAI.",
     {"Python": ["language", "popular"], "OpenAI": ["company", "ai-lab"]}),
    ("Anthropic recently expanded the Claude API for enterprise customers.",
     {"Anthropic": ["company", "ai-lab"], "Claude": ["model", "llm"]}),
    ("Guido and Linus both shaped modern open-source culture.",
     {"Guido": ["person"], "Linus": ["person"]}),
    ("Sam Altman leads OpenAI through aggressive product launches.",
     {"Sam": ["person"], "OpenAI": ["company", "ai-lab"]}),
    ("Transformers underpin both Claude and GPT-4 architectures.",
     {"Transformer": ["model"], "Claude": ["model", "llm"],
      "GPT-4": ["model", "llm"]}),
    ("Meta engineers contribute heavily to PyTorch and the Python ecosystem.",
     {"Meta": ["company", "ai-lab"], "Python": ["language", "popular"]}),
    ("Rust adoption is rising at Microsoft, Google, and Meta.",
     {"Rust": ["language"], "Microsoft": ["company"],
      "Google": ["company"], "Meta": ["company"]}),
    ("JavaScript remains the most-used language on GitHub by far.",
     {"JavaScript": ["language", "popular"], "GitHub": ["company", "tool"]}),
    ("TypeScript brings type safety to JavaScript codebases at Microsoft.",
     {"TypeScript": ["language"], "JavaScript": ["language", "popular"],
      "Microsoft": ["company"]}),
    ("Go's concurrency model influenced design choices at Anthropic.",
     {"Go": ["language"], "Anthropic": ["company", "ai-lab"]}),
    ("Linux Foundation projects include Kubernetes, Docker, and Git.",
     {"Linux": ["tool", "open-source"], "Kubernetes": ["tool"],
      "Docker": ["tool"], "Git": ["tool", "open-source"]}),
    ("Java powers Android development; Google supports it heavily.",
     {"Java": ["language", "enterprise"], "Google": ["company"]}),
    ("Ruby developers often migrate to Python or Go for performance.",
     {"Ruby": ["language"], "Python": ["language", "popular"],
      "Go": ["language"]}),
    ("Claude can write and review Python, JavaScript, and Go code.",
     {"Claude": ["model", "llm"], "Python": ["language", "popular"],
      "JavaScript": ["language", "popular"], "Go": ["language"]}),
    ("GPT-4 fine-tunes are restricted compared to open Llama weights.",
     {"GPT-4": ["model", "llm"],
      "Llama": ["model", "llm", "open-source"]}),
    ("OpenAI and Anthropic compete in the enterprise LLM market.",
     {"OpenAI": ["company", "ai-lab"],
      "Anthropic": ["company", "ai-lab"]}),
    ("Microsoft has invested heavily in OpenAI and integrated GPT-4.",
     {"Microsoft": ["company"], "OpenAI": ["company", "ai-lab"],
      "GPT-4": ["model", "llm"]}),
    ("Docker images at Anthropic typically ship Python and Rust binaries.",
     {"Docker": ["tool"], "Anthropic": ["company", "ai-lab"],
      "Python": ["language", "popular"], "Rust": ["language"]}),
]

# Two workload sizes (mirroring wikipedia-pageviews).
#
#   DEMO_SCALE=small  -- 4 agents over the first 20 docs (CI-friendly).
#   anything else     -- 8 agents over all 40 docs (local showcase).
_SCALE = os.environ.get("DEMO_SCALE", "large").lower()
if _SCALE == "small":
    NUM_AGENTS = 4
    NUM_DOCS = 20
else:
    NUM_AGENTS = 8
    NUM_DOCS = 40


def canonical_pair(a, b):
    """Unordered pair as (smaller, larger) so add_cooccur(A,B) and
    add_cooccur(B,A) collapse to the same key. Driver-side
    canonicalisation matches the (a, b) tuple shape in graph.orly."""
    return (a, b) if a < b else (b, a)


def compute_ground_truth(docs, num_agents):
    """Derive the expected end-state from the corpus alone.

    Tags carry provenance: doc_id `d` is processed by agent `d %
    num_agents` (the round-robin split below), so the expected tag set
    for an entity is the set of (tag, agent) records, not just tags."""
    expected_tag_records = {}  # entity -> set((tag, agent))
    expected_mentions = {}     # (entity, doc_id) -> 1
    expected_cooccur = {}      # (a, b) -> count
    for doc_id, (_text, entities) in enumerate(docs):
        agent_name = f"agent-{doc_id % num_agents}"
        for entity, tags in entities.items():
            for tag in tags:
                expected_tag_records.setdefault(entity, set()).add((tag, agent_name))
            expected_mentions[(entity, doc_id)] = 1
        names = sorted(entities.keys())
        for i, a in enumerate(names):
            for b in names[i + 1:]:
                key = canonical_pair(a, b)
                expected_cooccur[key] = expected_cooccur.get(key, 0) + 1
    return expected_tag_records, expected_mentions, expected_cooccur


def send(ws, stmt):
    ws.send(stmt)
    reply = json.loads(ws.recv())
    if reply.get("status") != "ok":
        raise RuntimeError(f"{stmt!r}\n  -> {reply}")
    return reply.get("result")


def add_tag(ws, pov, entity, tag, agent):
    send(ws, f'try {{{pov}}} graph add_tag '
             f'<{{.e: "{entity}", .t: "{tag}", .agent: "{agent}"}}>;')


def add_mention(ws, pov, entity, doc_id):
    send(ws, f'try {{{pov}}} graph add_mention '
             f'<{{.e: "{entity}", .d: {doc_id}}}>;')


def add_cooccur(ws, pov, a, b):
    send(ws, f'try {{{pov}}} graph add_cooccur '
             f'<{{.a: "{a}", .b: "{b}"}}>;')


def tags_for_entity(ws, pov, entity):
    r = send(ws, f'try {{{pov}}} graph tags_for_entity <{{.e: "{entity}"}}>;')
    # graph.orly returns a set of <{.tag, .agent}> records; WS wire form
    # is a JSON array of objects. Surface it as a set of (tag, agent).
    return {(rec["tag"], rec["agent"]) for rec in (r or [])}


def mention_count(ws, pov, entity, doc_id):
    r = send(ws, f'try {{{pov}}} graph mention_count '
                 f'<{{.e: "{entity}", .d: {doc_id}}}>;')
    return int(r)


def mentions_total(ws, pov, entity, d_start, d_end):
    r = send(ws, f'try {{{pov}}} graph mentions_total '
                 f'<{{.e: "{entity}", .d_start: {d_start}, .d_end: {d_end}}}>;')
    return int(r)


def cooccur_count(ws, pov, a, b):
    r = send(ws, f'try {{{pov}}} graph cooccur_count '
                 f'<{{.a: "{a}", .b: "{b}"}}>;')
    return int(r)


def agent(agent_id, pov, docs):
    """One agent: open a fresh WebSocket, open a session, ingest its
    slice of (doc_id, extraction) pairs into the shared POV. Every tag
    it asserts is stamped with this agent's name for provenance."""
    agent_name = f"agent-{agent_id}"
    ws = websocket.create_connection(WS_URL, timeout=WS_TIMEOUT_S)
    try:
        send(ws, "new session;")
        for doc_id, (_text, entities) in docs:
            # Per doc: tag every entity, mention every entity, and emit
            # one cooccurrence per unordered pair.
            for entity, tags in entities.items():
                for tag in tags:
                    add_tag(ws, pov, entity, tag, agent_name)
                add_mention(ws, pov, entity, doc_id)
            names = sorted(entities.keys())
            for i, a in enumerate(names):
                for b in names[i + 1:]:
                    pa, pb = canonical_pair(a, b)
                    add_cooccur(ws, pov, pa, pb)
    finally:
        ws.close()


def main():
    docs = CORPUS[:NUM_DOCS]
    expected_tag_records, expected_mentions, expected_cooccur = compute_ground_truth(
        docs, NUM_AGENTS)

    # Split docs across agents round-robin so hot entities collide
    # across multiple agents (real contention on shared keys).
    per_agent_docs = [[] for _ in range(NUM_AGENTS)]
    for doc_id, doc in enumerate(docs):
        per_agent_docs[doc_id % NUM_AGENTS].append((doc_id, doc))

    add_tag_calls = sum(len(tags) for _text, ents in docs for tags in ents.values())
    total_writes = (
        add_tag_calls                       # add_tag calls (one per doc/entity/tag)
        + len(expected_mentions)            # add_mention calls
        + sum(expected_cooccur.values())    # add_cooccur calls
    )

    # Bootstrap connection: install graph + create the shared POV.
    boot = websocket.create_connection(WS_URL, timeout=WS_TIMEOUT_S)
    send(boot, "new session;")
    send(boot, "install graph.1;")
    pov = send(boot, "new safe shared pov;")
    print(f"pov: {pov}")
    print(f"agents: {NUM_AGENTS}, docs: {NUM_DOCS}, "
          f"entities: {len(expected_tag_records)}, "
          f"unordered pairs: {len(expected_cooccur)}")
    print(f"total writes from agents: {total_writes:,}")

    # No pre-init and no read-back barrier. With commutative-upsert
    # (issue #151) the very first write to a key is the same bare `+= 1`
    # / `|=` the engine treats commutatively -- an absent key folds from
    # the monoid identity (0 / empty set) -- so concurrent agents can
    # *create* and aggregate brand-new keys with zero coordination, no
    # seeding, and no create-race. The expected end-state is therefore
    # exactly the corpus rollup: every (entity, doc) mention is written
    # once, and each pair's cooccurrence count is its number of docs.
    expected_mention_count = expected_mentions
    expected_cooccur_count = expected_cooccur

    # Fan out: each agent opens its own WS and runs concurrently.
    threads = []
    t0 = time.monotonic()
    for i in range(NUM_AGENTS):
        t = threading.Thread(target=agent, args=(i, pov, per_agent_docs[i]))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t0
    rate = total_writes / elapsed
    print(f"\ningest done in {elapsed:.2f}s "
          f"({rate:,.0f} writes/sec end-to-end with {NUM_AGENTS} agents)")

    # ------------------------------------------------------------------
    # Verify against ground truth.
    # ------------------------------------------------------------------
    print()
    print("=== verifying knowledge graph ===")
    failures = []

    for entity, want_records in expected_tag_records.items():
        got_records = tags_for_entity(boot, pov, entity)
        if got_records != want_records:
            failures.append(
                f"tags({entity}): got {sorted(got_records)}, "
                f"want {sorted(want_records)}")

    for (entity, doc_id), want in expected_mention_count.items():
        got = mention_count(boot, pov, entity, doc_id)
        if got != want:
            failures.append(
                f"mention({entity}, {doc_id}): got {got}, want {want}")

    for (a, b), want in expected_cooccur_count.items():
        got = cooccur_count(boot, pov, a, b)
        if got != want:
            failures.append(
                f"cooccur({a}, {b}): got {got}, want {want}")

    # ------------------------------------------------------------------
    # Per-entity rollup (one line per entity, sorted by total mentions
    # so the hot entities surface at the top).
    # ------------------------------------------------------------------
    per_entity_mentions = {}
    for (entity, _doc_id), v in expected_mention_count.items():
        per_entity_mentions[entity] = per_entity_mentions.get(entity, 0) + v

    print(f"  {'entity':<14}  {'mentions':>9}  {'expected':>9}  {'tags (×contributing agents)'}")
    print(f"  {'-'*14}  {'-'*9}  {'-'*9}  {'-'*30}")
    for entity in sorted(per_entity_mentions, key=lambda e: -per_entity_mentions[e]):
        # Doc IDs in this corpus are 0..NUM_DOCS-1, all dense.
        got_total = mentions_total(boot, pov, entity, 0, NUM_DOCS - 1)
        want_total = per_entity_mentions[entity]
        marker = "✓" if got_total == want_total else "✗"
        # Roll the provenance records up to distinct tags, annotating any
        # tag that more than one agent independently asserted (×N).
        tag_agents = {}
        for tag, ag in expected_tag_records[entity]:
            tag_agents.setdefault(tag, set()).add(ag)
        tag_str = ",".join(
            f"{t}×{len(ags)}" if len(ags) > 1 else t
            for t, ags in sorted(tag_agents.items()))
        print(f"  {entity:<14}  {got_total:>9}  {want_total:>9}  "
              f"{tag_str}  {marker}")
        if got_total != want_total:
            failures.append(
                f"mentions_total({entity}): got {got_total}, want {want_total}")

    print()
    print("=== top cooccurrence pairs ===")
    top_pairs = sorted(expected_cooccur_count.items(),
                       key=lambda kv: -kv[1])[:10]
    for (a, b), want in top_pairs:
        got = cooccur_count(boot, pov, a, b)
        marker = "✓" if got == want else "✗"
        print(f"  {a + ' & ' + b:<32}  {got:>4}  (want {want})  {marker}")

    print()
    print("=== the trick ===")
    print(f"  {NUM_AGENTS} concurrent agents all wrote into the same knowledge")
    print(f"  graph with zero coordination. Tag records (with per-agent")
    print(f"  provenance) union into one set per entity, and the per-entity")
    print(f"  / per-pair counters aggregate correctly: this is the shape")
    print(f"  multi-agent LLM extraction pipelines keep reinventing badly.")

    send(boot, "exit;")
    boot.close()

    if failures:
        print("\n=== self-check FAILED ===")
        for f in failures[:20]:
            print(f"  {f}")
        if len(failures) > 20:
            print(f"  ... and {len(failures) - 20} more")
        sys.exit(1)
    total_tag_records = sum(len(recs) for recs in expected_tag_records.values())
    print("\n=== self-check OK ===")
    print(f"  verified {total_tag_records} tag provenance records across "
          f"{len(expected_tag_records)} entities + "
          f"{len(expected_mention_count)} mention counters + "
          f"{len(expected_cooccur_count)} cooccurrence counters across "
          f"{NUM_AGENTS} concurrent agents")


if __name__ == "__main__":
    main()
