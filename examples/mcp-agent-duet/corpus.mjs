/**
 * The shared corpus, split between the two agents: "ada" ingests docs 0-5,
 * "byte" ingests docs 6-11. Deliberate structure:
 *
 *  - HUB entities (JWST, Hubble, Cassini, Vega) appear on BOTH sides of the
 *    split, so the two agents write the same hot keys concurrently.
 *  - Some tags are CORROBORATED across the split (JWST:telescope,
 *    JWST:infrared, Cassini:probe asserted by both agents from different
 *    docs) -- provenance must show two records, not a collapsed one.
 *  - Kepler-442b appears ONLY in ada's docs and TRAPPIST-1e / Proxima
 *    Centauri b ONLY in byte's -- so a `reach` traversal from Kepler-442b
 *    arrives at byte's entities only if BOTH halves of the graph landed.
 *
 * Each doc: { id, entities: [names co-occurring in the doc], tags: [[entity,
 * tag], ...] }. The extraction is scripted (this is the deterministic CI
 * stand-in for a real LLM extractor -- see README for the live-agent recipe).
 */

export const DOCS = [
  // -- ada's slice: docs 0-5 ------------------------------------------------
  { id: 0, entities: ["JWST", "Kepler-442b"], tags: [["JWST", "telescope"], ["Kepler-442b", "exoplanet"]] },
  { id: 1, entities: ["Hubble", "Andromeda"], tags: [["Hubble", "telescope"], ["Andromeda", "galaxy"]] },
  { id: 2, entities: ["JWST", "Hubble"], tags: [["JWST", "infrared"]] },
  { id: 3, entities: ["Kepler-442b", "JWST"], tags: [["Kepler-442b", "habitable-zone"]] },
  { id: 4, entities: ["Enceladus", "Cassini"], tags: [["Enceladus", "moon"], ["Cassini", "probe"]] },
  { id: 5, entities: ["Vega", "JWST"], tags: [["Vega", "star"]] },
  // -- byte's slice: docs 6-11 ----------------------------------------------
  { id: 6, entities: ["JWST", "TRAPPIST-1e"], tags: [["JWST", "telescope"], ["TRAPPIST-1e", "exoplanet"]] },
  { id: 7, entities: ["TRAPPIST-1e", "Proxima Centauri b"], tags: [["Proxima Centauri b", "exoplanet"]] },
  { id: 8, entities: ["Titan", "Cassini"], tags: [["Titan", "moon"], ["Cassini", "probe"]] },
  { id: 9, entities: ["Voyager 2", "Titan"], tags: [["Voyager 2", "probe"]] },
  { id: 10, entities: ["Hubble", "Vega"], tags: [["Hubble", "veteran"]] },
  { id: 11, entities: ["JWST", "Proxima Centauri b"], tags: [["JWST", "infrared"]] },
];

export const AGENTS = {
  ada: { from: 0, to: 5 },
  byte: { from: 6, to: 11 },
};

/** Canonical unordered pair, matching the agent-swarm driver convention:
    lexicographically smaller entity first. */
export const pair = (a, b) => (a < b ? [a, b] : [b, a]);

/** All unordered entity pairs co-occurring in one doc. */
export function docPairs(doc) {
  const out = [];
  for (let i = 0; i < doc.entities.length; i++) {
    for (let j = i + 1; j < doc.entities.length; j++) {
      out.push(pair(doc.entities[i], doc.entities[j]));
    }
  }
  return out;
}

/** Which agent owns a doc id (ground truth mirrors the split above). */
export function agentFor(id) {
  for (const [name, { from, to }] of Object.entries(AGENTS)) {
    if (id >= from && id <= to) return name;
  }
  throw new Error(`doc ${id} belongs to no agent`);
}
