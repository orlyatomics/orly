// GRC-20-shaped knowledge graph on Orly, in Go.
//
// Mirrors demo.py: three phases (wiki biographical / stanford schools +
// relations / concurrent race), the same typed event model, the same
// self-check. The op is a sum type (op_t) and the replay -- latest-wins,
// tombstones, formatting, time-travel -- runs in the engine (grc20.orly);
// this driver streams typed events and reads resolved values.
//
// Run via the wrapper:
//
//	./run-go.sh
//
// Or directly, after starting orlyi separately:
//
//	go run demo.go
package main

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	orly "github.com/orlyatomics/orly/clients/go"
)


// A .ts beyond any real event -- "now / all events" (mirrors grc20.orly's
// `forever`).
const forever int64 = math.MaxInt64

type philosopher struct {
	id         string
	name       string
	born, died int
}

var philosophers = []philosopher{
	{"socrates", "Socrates", -470, -399},
	{"plato", "Plato", -428, -348},
	{"aristotle", "Aristotle", -384, -322},
	{"pythagoras", "Pythagoras", -570, -495},
	{"heraclitus", "Heraclitus", -535, -475},
	{"epicurus", "Epicurus", -341, -270},
}

// Editor 2 ("stanford") adds the school of thought for each philosopher,
// listed as parallel slices so the ingest order is stable.
var schoolEntities = []string{"socrates", "plato", "aristotle", "pythagoras", "heraclitus", "epicurus"}
var schoolValues = []string{"Classical", "Platonism", "Peripateticism", "Pythagoreanism", "Heracliteanism", "Epicureanism"}

type relation struct {
	subject, prop, target string
}

var relations = []relation{
	{"plato", "student_of", "socrates"},
	{"aristotle", "student_of", "plato"},
}

// Phase-3 race: both editors concurrently rewrite the same attribute on
// the same entity. The engine's `reduce` resolves the later .ts as the
// current value; the loser stays in history as an alternative-source claim.
const raceEntity = "pythagoras"
const raceProp = "born"
const raceWiki = -570
const raceStanford = -575

// ---------------------------------------------------------------------
// WS helpers.
// ---------------------------------------------------------------------


func die(msg string) {
	fmt.Fprintln(os.Stderr, msg)
	os.Exit(1)
}

func must[T any](v T, err error) T {
	if err != nil {
		die(err.Error())
	}
	return v
}

func check(err error) {
	if err != nil {
		die(err.Error())
	}
}

func asString(raw json.RawMessage) string {
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		die(fmt.Sprintf("expected string result, got %s", raw))
	}
	return s
}

// orlyi serialises numbers as JSON floats; cast to int via float64.
func asInt(raw json.RawMessage) int {
	var n float64
	if err := json.Unmarshal(raw, &n); err != nil {
		die(fmt.Sprintf("expected number result, got %s", raw))
	}
	return int(n)
}

func asBool(raw json.RawMessage) bool {
	var b bool
	if err := json.Unmarshal(raw, &b); err != nil {
		die(fmt.Sprintf("expected bool result, got %s", raw))
	}
	return b
}

// orlyi serialises sets as JSON arrays.
func asStringSet(raw json.RawMessage) []string {
	if string(raw) == "null" {
		return nil
	}
	var out []string
	if err := json.Unmarshal(raw, &out); err != nil {
		die(fmt.Sprintf("expected array result, got %s", raw))
	}
	sort.Strings(out)
	return out
}

func nowMs() int64 { return time.Now().UnixNano() / 1_000_000 }

func connect() *orly.Client {
	c, err := orly.Connect()
	if err != nil {
		die(err.Error())
	}
	must(c.NewSession())
	return c
}

// ---------------------------------------------------------------------
// Event provenance. The op is a typed variant resolved by the engine, so
// the driver only ever reads the .ts / .editor fields off the raw log --
// for per-editor analytics, never for replay.
// ---------------------------------------------------------------------

type eventMeta struct {
	ts     int64
	editor string
}

type eventWire struct {
	Ts     float64 `json:"ts"`
	Editor string  `json:"editor"`
}

func histMeta(c *orly.Client, pov, entity, prop string) []eventMeta {
	raw := must(c.Call(pov, "grc20", "hist_for", map[string]any{"entity": entity, "property": prop}))
	if string(raw) == "null" {
		return nil
	}
	var wire []eventWire
	if err := json.Unmarshal(raw, &wire); err != nil {
		die(fmt.Sprintf("expected array-of-objects result, got %s", raw))
	}
	out := make([]eventMeta, 0, len(wire))
	for _, w := range wire {
		out = append(out, eventMeta{ts: int64(w.Ts), editor: w.Editor})
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].ts != out[j].ts {
			return out[i].ts < out[j].ts
		}
		return out[i].editor < out[j].editor
	})
	return out
}

// ---------------------------------------------------------------------
// Writes -- typed appends; the engine builds the op_t variant.
// ---------------------------------------------------------------------

func registerEntity(c *orly.Client, pov, entity string) {
	must(c.Call(pov, "grc20", "register_entity", map[string]any{"entity": entity}))
}

func registerProp(c *orly.Client, pov, entity, prop string) {
	must(c.Call(pov, "grc20", "register_prop", map[string]any{"entity": entity, "property": prop}))
}

func createEntity(c *orly.Client, pov, entity string, ts int64, editor, kind string) {
	must(c.Call(pov, "grc20", "create_entity",
		map[string]any{"entity": entity, "ts": ts, "editor": editor, "kind": kind}))
}

func deleteEntity(c *orly.Client, pov, entity string, ts int64, editor string) {
	must(c.Call(pov, "grc20", "delete_entity",
		map[string]any{"entity": entity, "ts": ts, "editor": editor}))
}

func setText(c *orly.Client, pov, entity, prop string, ts int64, editor, text string) {
	must(c.Call(pov, "grc20", "set_text",
		map[string]any{"entity": entity, "property": prop, "ts": ts, "editor": editor, "text": text}))
}

func setNumber(c *orly.Client, pov, entity, prop string, ts int64, editor string, n int) {
	must(c.Call(pov, "grc20", "set_number",
		map[string]any{"entity": entity, "property": prop, "ts": ts, "editor": editor, "n": n}))
}

func setRelation(c *orly.Client, pov, entity, prop string, ts int64, editor, target string) {
	must(c.Call(pov, "grc20", "set_relation",
		map[string]any{"entity": entity, "property": prop, "ts": ts, "editor": editor, "target": target}))
}

// ---------------------------------------------------------------------
// Reads -- the engine resolves the value; the driver enumerates.
// ---------------------------------------------------------------------

func displayAsOf(c *orly.Client, pov, entity, prop string, asOf int64) string {
	return asString(must(c.Call(pov, "grc20", "display_as_of",
		map[string]any{"entity": entity, "property": prop, "as_of": asOf})))
}

func entityLiveAsOf(c *orly.Client, pov, entity string, asOf int64) bool {
	return asBool(must(c.Call(pov, "grc20", "entity_live_as_of",
		map[string]any{"entity": entity, "as_of": asOf})))
}

func allEntities(c *orly.Client, pov string) []string {
	return asStringSet(must(c.Call(pov, "grc20", "all_entities", nil)))
}

func propsOf(c *orly.Client, pov, entity string) []string {
	return asStringSet(must(c.Call(pov, "grc20", "props_of", map[string]any{"entity": entity})))
}

func entityCount(c *orly.Client, pov string) int {
	return asInt(must(c.Call(pov, "grc20", "entity_count", nil)))
}

// ---------------------------------------------------------------------
// GRC-20-style ops. Thin: pick the typed setter + register the property.
// ---------------------------------------------------------------------

func opCreateEntity(c *orly.Client, pov, editor, entity, typeName string) {
	createEntity(c, pov, entity, nowMs(), editor, typeName)
}

func opSetText(c *orly.Client, pov, editor, entity, prop, text string) {
	registerProp(c, pov, entity, prop)
	setText(c, pov, entity, prop, nowMs(), editor, text)
}

func opSetInt(c *orly.Client, pov, editor, entity, prop string, n int) {
	registerProp(c, pov, entity, prop)
	setNumber(c, pov, entity, prop, nowMs(), editor, n)
}

func opCreateRelation(c *orly.Client, pov, editor, entity, prop, target string) {
	registerEntity(c, pov, target)
	registerProp(c, pov, entity, prop)
	setRelation(c, pov, entity, prop, nowMs(), editor, target)
}

// ---------------------------------------------------------------------
// Reconstruct: enumerate the graph at `asOf`, asking the engine for each
// value. No latest-wins loop, no tombstone bookkeeping, no formatting --
// that all lives in grc20.orly now.
// ---------------------------------------------------------------------

// reconstruct returns entity -> {"__type": typeStr, prop: displayStr}.
func reconstruct(c *orly.Client, pov string, asOf int64) map[string]map[string]string {
	out := map[string]map[string]string{}
	for _, eid := range allEntities(c, pov) {
		if !entityLiveAsOf(c, pov, eid, asOf) {
			continue
		}
		state := map[string]string{"__type": displayAsOf(c, pov, eid, "__entity", asOf)}
		for _, prop := range propsOf(c, pov, eid) {
			if v := displayAsOf(c, pov, eid, prop, asOf); v != "(absent)" {
				state[prop] = v
			}
		}
		out[eid] = state
	}
	return out
}

func printSnapshot(label string, state map[string]map[string]string) {
	fmt.Printf("\n=== %s ===\n", label)
	if len(state) == 0 {
		fmt.Println("  (empty graph)")
		return
	}
	eids := make([]string, 0, len(state))
	for eid := range state {
		eids = append(eids, eid)
	}
	sort.Strings(eids)
	for _, eid := range eids {
		attrs := state[eid]
		typeName := attrs["__type"]
		if typeName == "" {
			typeName = "?"
		}
		var props []string
		for p := range attrs {
			if p != "__type" {
				props = append(props, p)
			}
		}
		sort.Strings(props)
		var bits []string
		for _, p := range props {
			bits = append(bits, fmt.Sprintf("%s=%s", p, attrs[p]))
		}
		body := strings.Join(bits, ", ")
		if body == "" {
			body = "(no props)"
		}
		fmt.Printf("  %s (%s): %s\n", eid, typeName, body)
	}
}

// ---------------------------------------------------------------------
// Phases.
// ---------------------------------------------------------------------

func phase1Wiki(pov string) {
	c := connect()
	defer c.Close()
	for _, p := range philosophers {
		opCreateEntity(c, pov, "wiki", p.id, "Person")
		opSetText(c, pov, "wiki", p.id, "name", p.name)
		opSetInt(c, pov, "wiki", p.id, "born", p.born)
		opSetInt(c, pov, "wiki", p.id, "died", p.died)
	}
}

func phase2Stanford(pov string) {
	c := connect()
	defer c.Close()
	for i, eid := range schoolEntities {
		opSetText(c, pov, "stanford", eid, "school", schoolValues[i])
	}
	for _, r := range relations {
		opCreateRelation(c, pov, "stanford", r.subject, r.prop, r.target)
	}
}

func phase3Race(pov string) {
	var wg sync.WaitGroup
	writer := func(editor string, value int) {
		defer wg.Done()
		c := connect()
		defer c.Close()
			opSetInt(c, pov, editor, raceEntity, raceProp, value)
	}
	wg.Add(2)
	go writer("wiki", raceWiki)
	go writer("stanford", raceStanford)
	wg.Wait()
}

func editorialDiff(c *orly.Client, pov string) {
	byEditor := map[string]int{}
	entitiesByEditor := map[string]map[string]struct{}{}
	for _, eid := range allEntities(c, pov) {
		props := append(propsOf(c, pov, eid), "__entity")
		for _, prop := range props {
			for _, ev := range histMeta(c, pov, eid, prop) {
				byEditor[ev.editor]++
				if entitiesByEditor[ev.editor] == nil {
					entitiesByEditor[ev.editor] = map[string]struct{}{}
				}
				entitiesByEditor[ev.editor][eid] = struct{}{}
			}
		}
	}
	editors := make([]string, 0, len(byEditor))
	for e := range byEditor {
		editors = append(editors, e)
	}
	sort.Strings(editors)
	fmt.Println("\n=== editorial diff ===")
	for _, e := range editors {
		fmt.Printf("  %-10s %3d events on %2d entities\n", e, byEditor[e], len(entitiesByEditor[e]))
	}
	if len(editors) >= 2 {
		a, b := editors[0], editors[1]
		var overlap []string
		for eid := range entitiesByEditor[a] {
			if _, ok := entitiesByEditor[b][eid]; ok {
				overlap = append(overlap, eid)
			}
		}
		sort.Strings(overlap)
		fmt.Printf("  overlap: %d entities edited by both (%s ∩ %s): %v\n",
			len(overlap), a, b, overlap)
	}
}

// ---------------------------------------------------------------------
// main.
// ---------------------------------------------------------------------

func main() {
	boot := connect()
	defer boot.Close()
	check(boot.Install("grc20", 1))
	pov := must(boot.NewPov())
	fmt.Printf("pov: %s\n", pov)
	fmt.Printf("corpus: %d entities, %d schools, %d relations\n",
		len(philosophers), len(schoolEntities), len(relations))

	// --- Phase 1: wiki streams biographical facts ---
	fmt.Println("\n[phase 1] wiki -> biographical facts (name, born, died)")
	t0 := time.Now()
	phase1Wiki(pov)
	ts1 := nowMs()
	fmt.Printf("  %d entities registered in %.2fs\n",
		entityCount(boot, pov), time.Since(t0).Seconds())
	printSnapshot("snapshot 1: end of phase 1 (wiki only)",
		reconstruct(boot, pov, ts1))

	time.Sleep(50 * time.Millisecond)

	// --- Phase 2: stanford streams schools + relations ---
	fmt.Println("\n[phase 2] stanford -> school + student_of relations")
	t0 = time.Now()
	phase2Stanford(pov)
	ts2 := nowMs()
	fmt.Printf("  done in %.2fs\n", time.Since(t0).Seconds())
	printSnapshot("snapshot 2: end of phase 2 (wiki + stanford)",
		reconstruct(boot, pov, ts2))

	time.Sleep(50 * time.Millisecond)

	// --- Phase 3: concurrent race ---
	fmt.Printf("\n[phase 3] wiki + stanford concurrently overwrite %s.%s\n",
		raceEntity, raceProp)
	fmt.Printf("  wiki     -> %d\n", raceWiki)
	fmt.Printf("  stanford -> %d\n", raceStanford)
	phase3Race(pov)
	ts3 := nowMs()
	raceEvents := histMeta(boot, pov, raceEntity, raceProp)
	fmt.Printf("  history for %s.%s after race (%d events, ts-sorted):\n",
		raceEntity, raceProp, len(raceEvents))
	for _, ev := range raceEvents {
		fmt.Printf("    %d  %s\n", ev.ts, ev.editor)
	}
	winning := displayAsOf(boot, pov, raceEntity, raceProp, ts3)
	fmt.Printf("  -> engine-resolved current value: %s.%s = %s\n",
		raceEntity, raceProp, winning)

	// --- Final state + editorial diff ---
	printSnapshot("snapshot 3: final state", reconstruct(boot, pov, ts3))
	editorialDiff(boot, pov)

	fmt.Println("\n=== the trick ===")
	fmt.Println("  Each op is a typed variant")
	fmt.Println("    op_t is <| Text(str) | Number(int) | Relation(str) | Deleted |>")
	fmt.Println("  unioned into a per-(entity, prop) set of <{.ts,.editor,.op}>")
	fmt.Println("  records (storable variants, #96). Multi-editor |= never")
	fmt.Println("  drops events (post-#50 fold). The REPLAY -- sort by .ts,")
	fmt.Println("  reduce to the latest, when over the op to format/tombstone,")
	fmt.Println("  filtered by an as_of cutoff for time-travel -- runs in the")
	fmt.Println("  engine. This driver just enumerates and reads.")

	// --- Self-check (mirrors demo.py) ---
	var failures []string
	s1 := reconstruct(boot, pov, ts1)
	for _, p := range philosophers {
		e := s1[p.id]
		if e["name"] != p.name {
			failures = append(failures, fmt.Sprintf("snapshot 1: %s.name = %q", p.id, e["name"]))
		}
		if e["born"] != strconv.Itoa(p.born) {
			failures = append(failures, fmt.Sprintf("snapshot 1: %s.born = %q (want %d)", p.id, e["born"], p.born))
		}
		if _, hasSchool := e["school"]; hasSchool {
			failures = append(failures, fmt.Sprintf("snapshot 1: %s.school leaked from phase 2", p.id))
		}
	}
	s2 := reconstruct(boot, pov, ts2)
	for i, eid := range schoolEntities {
		if s2[eid]["school"] != schoolValues[i] {
			failures = append(failures, fmt.Sprintf("snapshot 2: %s.school = %q", eid, s2[eid]["school"]))
		}
	}
	for _, r := range relations {
		if got := s2[r.subject][r.prop]; got != "-> "+r.target {
			failures = append(failures, fmt.Sprintf("snapshot 2: %s.%s = %q", r.subject, r.prop, got))
		}
	}
	if len(raceEvents) < 2 {
		failures = append(failures,
			fmt.Sprintf("phase 3: only %d events in race history -- expected both writers to land",
				len(raceEvents)))
	}

	check(boot.Exit())

	if len(failures) > 0 {
		fmt.Println("\n=== self-check FAILED ===")
		for _, f := range failures {
			fmt.Println("  " + f)
		}
		os.Exit(1)
	}
	fmt.Println("\n=== self-check OK ===")
	fmt.Println("  verified 3 time-travel snapshot invariants + multi-editor race")
}
