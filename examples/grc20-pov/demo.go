// GRC-20-shaped knowledge graph on Orly, in Go.
//
// Mirrors demo.py: three phases (wiki biographical / stanford schools +
// relations / concurrent race), same encoding, same self-check.
// Provided so readers can pick whichever language matches their
// environment.
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
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

const wsURL = "ws://127.0.0.1:8082/"

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

// Editor 2 ("stanford") adds the school of thought for each
// philosopher. Map iteration order is randomised; the keys/values are
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
// the same entity. Whoever's event lands at the later ms wins on read;
// the loser stays in history as an alternative-source claim.
const raceEntity = "pythagoras"
const raceProp = "born"
const raceWiki = -570
const raceStanford = -575

// ---------------------------------------------------------------------
// WS helpers.
// ---------------------------------------------------------------------

type reply struct {
	Status string          `json:"status"`
	Result json.RawMessage `json:"result"`
}

func die(msg string) {
	fmt.Fprintln(os.Stderr, msg)
	os.Exit(1)
}

func dial() *websocket.Conn {
	c, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		die(fmt.Sprintf("dial %s: %v", wsURL, err))
	}
	return c
}

func send(c *websocket.Conn, stmt string) json.RawMessage {
	if err := c.WriteMessage(websocket.TextMessage, []byte(stmt)); err != nil {
		die(fmt.Sprintf("write %q: %v", stmt, err))
	}
	_, msg, err := c.ReadMessage()
	if err != nil {
		die(fmt.Sprintf("read after %q: %v", stmt, err))
	}
	var r reply
	if err := json.Unmarshal(msg, &r); err != nil {
		die(fmt.Sprintf("parse reply: %v\n  raw: %s", err, msg))
	}
	if r.Status != "ok" {
		die(fmt.Sprintf("%s: %s", stmt, string(msg)))
	}
	return r.Result
}

func sendString(c *websocket.Conn, stmt string) string {
	raw := send(c, stmt)
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		die(fmt.Sprintf("expected string result, got %s", raw))
	}
	return s
}

// orlyi serialises numbers as JSON floats; cast to int via float64.
func sendInt(c *websocket.Conn, stmt string) int {
	raw := send(c, stmt)
	var n float64
	if err := json.Unmarshal(raw, &n); err != nil {
		die(fmt.Sprintf("expected number result, got %s", raw))
	}
	return int(n)
}

// orlyi serialises sets as JSON arrays.
func sendStringSet(c *websocket.Conn, stmt string) []string {
	raw := send(c, stmt)
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

// ---------------------------------------------------------------------
// Op encoding: "<13-digit ms ts>:<editor>:<kind>:<value>".
// Kind ∈ {L text, I integer, R relation target id, D tombstone}.
// ---------------------------------------------------------------------

func nowMs() int64 { return time.Now().UnixNano() / 1_000_000 }

func encode(editor, kind, value string) string {
	return fmt.Sprintf("%013d:%s:%s:%s", nowMs(), editor, kind, value)
}

type parsed struct {
	ts             int64
	editor, kind   string
	value          string
}

func parseEntry(entry string) parsed {
	parts := strings.SplitN(entry, ":", 4)
	if len(parts) != 4 {
		die(fmt.Sprintf("bad entry %q", entry))
	}
	ts, err := strconv.ParseInt(parts[0], 10, 64)
	if err != nil {
		die(fmt.Sprintf("bad ts in %q: %v", entry, err))
	}
	return parsed{ts: ts, editor: parts[1], kind: parts[2], value: parts[3]}
}

func orlyStr(s string) string {
	// Corpus has no quotes/backslashes, but be defensive.
	s = strings.ReplaceAll(s, `\`, `\\`)
	s = strings.ReplaceAll(s, `"`, `\"`)
	return `"` + s + `"`
}

// ---------------------------------------------------------------------
// Wrappers for the grc20 package's three commutative funcs.
// ---------------------------------------------------------------------

func appendOp(c *websocket.Conn, pov, entity, prop, entry string) {
	send(c, fmt.Sprintf(
		`try {%s} grc20 append_op <{.entity: %s, .property: %s, .entry: %s}>;`,
		pov, orlyStr(entity), orlyStr(prop), orlyStr(entry)))
}

func registerEntity(c *websocket.Conn, pov, entity string) {
	send(c, fmt.Sprintf(
		`try {%s} grc20 register_entity <{.entity: %s}>;`,
		pov, orlyStr(entity)))
}

func registerProp(c *websocket.Conn, pov, entity, prop string) {
	send(c, fmt.Sprintf(
		`try {%s} grc20 register_prop <{.entity: %s, .property: %s}>;`,
		pov, orlyStr(entity), orlyStr(prop)))
}

func histFor(c *websocket.Conn, pov, entity, prop string) []string {
	return sendStringSet(c, fmt.Sprintf(
		`try {%s} grc20 hist_for <{.entity: %s, .property: %s}>;`,
		pov, orlyStr(entity), orlyStr(prop)))
}

func allEntities(c *websocket.Conn, pov string) []string {
	return sendStringSet(c, fmt.Sprintf(`try {%s} grc20 all_entities <{}>;`, pov))
}

func propsOf(c *websocket.Conn, pov, entity string) []string {
	return sendStringSet(c, fmt.Sprintf(
		`try {%s} grc20 props_of <{.entity: %s}>;`,
		pov, orlyStr(entity)))
}

func entityCount(c *websocket.Conn, pov string) int {
	return sendInt(c, fmt.Sprintf(`try {%s} grc20 entity_count <{}>;`, pov))
}

// ---------------------------------------------------------------------
// GRC-20-style ops (the driver owns the op vocabulary; the engine just
// stores events).
// ---------------------------------------------------------------------

func opCreateEntity(c *websocket.Conn, pov, editor, entity, typeName string) {
	registerEntity(c, pov, entity)
	registerProp(c, pov, entity, "__type")
	appendOp(c, pov, entity, "__type", encode(editor, "L", typeName))
}

func opSetText(c *websocket.Conn, pov, editor, entity, prop, text string) {
	registerEntity(c, pov, entity)
	registerProp(c, pov, entity, prop)
	appendOp(c, pov, entity, prop, encode(editor, "L", text))
}

func opSetInt(c *websocket.Conn, pov, editor, entity, prop string, n int) {
	registerEntity(c, pov, entity)
	registerProp(c, pov, entity, prop)
	appendOp(c, pov, entity, prop, encode(editor, "I", strconv.Itoa(n)))
}

func opCreateRelation(c *websocket.Conn, pov, editor, entity, prop, target string) {
	registerEntity(c, pov, entity)
	registerEntity(c, pov, target)
	registerProp(c, pov, entity, prop)
	appendOp(c, pov, entity, prop, encode(editor, "R", target))
}

// ---------------------------------------------------------------------
// Replay: build the current state of every (entity, property) by
// walking its history set in timestamp order. Optional `asOf` cutoff
// drops events with ts > cutoff -- that's the time-travel knob.
// ---------------------------------------------------------------------

type fact struct {
	kind, value, editor string
	ts                  int64
}

// nil asOf means "no cutoff" -- pass &someInt64 to filter.
func reconstruct(c *websocket.Conn, pov string, asOf *int64) map[string]map[string]fact {
	out := map[string]map[string]fact{}
	for _, eid := range allEntities(c, pov) {
		deleted := false
		state := map[string]fact{}
		for _, prop := range propsOf(c, pov, eid) {
			entries := histFor(c, pov, eid, prop) // already sorted
			if asOf != nil {
				filtered := entries[:0]
				for _, e := range entries {
					if parseEntry(e).ts <= *asOf {
						filtered = append(filtered, e)
					}
				}
				entries = filtered
			}
			if len(entries) == 0 {
				continue
			}
			p := parseEntry(entries[len(entries)-1])
			if prop == "__deleted" && p.kind == "D" {
				deleted = true
				break
			}
			state[prop] = fact{kind: p.kind, value: p.value, editor: p.editor, ts: p.ts}
		}
		if !deleted {
			out[eid] = state
		}
	}
	return out
}

func formatValue(f fact) string {
	switch f.kind {
	case "L", "I":
		return f.value
	case "R":
		return "→ " + f.value
	}
	return "?" + f.kind + ":" + f.value
}

func printSnapshot(label string, state map[string]map[string]fact) {
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
		typeName := "?"
		if t, ok := attrs["__type"]; ok {
			typeName = t.value
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
			bits = append(bits, fmt.Sprintf("%s=%s [%s]", p, formatValue(attrs[p]), attrs[p].editor))
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
	c := dial()
	defer c.Close()
	send(c, "new session;")
	for _, p := range philosophers {
		opCreateEntity(c, pov, "wiki", p.id, "Person")
		opSetText(c, pov, "wiki", p.id, "name", p.name)
		opSetInt(c, pov, "wiki", p.id, "born", p.born)
		opSetInt(c, pov, "wiki", p.id, "died", p.died)
	}
}

func phase2Stanford(pov string) {
	c := dial()
	defer c.Close()
	send(c, "new session;")
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
		c := dial()
		defer c.Close()
		send(c, "new session;")
		opSetInt(c, pov, editor, raceEntity, raceProp, value)
	}
	wg.Add(2)
	go writer("wiki", raceWiki)
	go writer("stanford", raceStanford)
	wg.Wait()
}

func editorialDiff(c *websocket.Conn, pov string) {
	byEditor := map[string]int{}
	entitiesByEditor := map[string]map[string]struct{}{}
	for _, eid := range allEntities(c, pov) {
		for _, prop := range propsOf(c, pov, eid) {
			for _, entry := range histFor(c, pov, eid, prop) {
				p := parseEntry(entry)
				byEditor[p.editor]++
				if entitiesByEditor[p.editor] == nil {
					entitiesByEditor[p.editor] = map[string]struct{}{}
				}
				entitiesByEditor[p.editor][eid] = struct{}{}
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
	boot := dial()
	defer boot.Close()
	sendString(boot, "new session;")
	send(boot, "install grc20.1;")
	pov := sendString(boot, "new safe shared pov;")
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
		reconstruct(boot, pov, &ts1))

	time.Sleep(50 * time.Millisecond)

	// --- Phase 2: stanford streams schools + relations ---
	fmt.Println("\n[phase 2] stanford -> school + student_of relations")
	t0 = time.Now()
	phase2Stanford(pov)
	ts2 := nowMs()
	fmt.Printf("  done in %.2fs\n", time.Since(t0).Seconds())
	printSnapshot("snapshot 2: end of phase 2 (wiki + stanford)",
		reconstruct(boot, pov, &ts2))

	time.Sleep(50 * time.Millisecond)

	// --- Phase 3: concurrent race ---
	fmt.Printf("\n[phase 3] wiki + stanford concurrently overwrite %s.%s\n",
		raceEntity, raceProp)
	fmt.Printf("  wiki     -> %d\n", raceWiki)
	fmt.Printf("  stanford -> %d\n", raceStanford)
	phase3Race(pov)
	ts3 := nowMs()
	raceEntries := histFor(boot, pov, raceEntity, raceProp)
	fmt.Printf("  history for %s.%s after race (%d events, ts-sorted):\n",
		raceEntity, raceProp, len(raceEntries))
	for _, e := range raceEntries {
		p := parseEntry(e)
		fmt.Printf("    %d  %-10s %s  %s\n", p.ts, p.editor, p.kind, p.value)
	}
	finalState := reconstruct(boot, pov, &ts3)
	winning := finalState[raceEntity][raceProp]
	fmt.Printf("  -> winning event: %s [%s, ts=%d]\n",
		winning.value, winning.editor, winning.ts)

	// --- Final state + editorial diff ---
	printSnapshot("snapshot 3: final state", finalState)
	editorialDiff(boot, pov)

	fmt.Println("\n=== the trick ===")
	fmt.Println("  Every editor |= a packed event into a per-(entity, prop)")
	fmt.Println("  history set. Multi-editor writes never drop events (post-#50")
	fmt.Println("  deferred-mutation fold). Reads replay the timestamp-sorted")
	fmt.Println("  log -- pass a cutoff for time-travel, take the latest for")
	fmt.Println("  the current value. The schema is three commutative funcs;")
	fmt.Println("  GRC-20's op vocabulary lives entirely in the driver.")

	// --- Self-check (mirrors demo.py) ---
	var failures []string
	s1 := reconstruct(boot, pov, &ts1)
	for _, p := range philosophers {
		e := s1[p.id]
		if name, ok := e["name"]; !ok || name.kind != "L" || name.value != p.name {
			failures = append(failures, fmt.Sprintf("snapshot 1: %s.name = %+v", p.id, name))
		}
		if born, ok := e["born"]; !ok || born.kind != "I" {
			failures = append(failures, fmt.Sprintf("snapshot 1: %s.born missing/wrong kind", p.id))
		} else if n, err := strconv.Atoi(born.value); err != nil || n != p.born {
			failures = append(failures, fmt.Sprintf("snapshot 1: %s.born = %s (want %d)", p.id, born.value, p.born))
		}
		if _, hasSchool := e["school"]; hasSchool {
			failures = append(failures, fmt.Sprintf("snapshot 1: %s.school leaked from phase 2", p.id))
		}
	}
	s2 := reconstruct(boot, pov, &ts2)
	for i, eid := range schoolEntities {
		e := s2[eid]
		if school, ok := e["school"]; !ok || school.kind != "L" || school.value != schoolValues[i] {
			failures = append(failures, fmt.Sprintf("snapshot 2: %s.school = %+v", eid, school))
		}
	}
	for _, r := range relations {
		e := s2[r.subject]
		if rel, ok := e[r.prop]; !ok || rel.kind != "R" || rel.value != r.target {
			failures = append(failures, fmt.Sprintf("snapshot 2: %s.%s = %+v", r.subject, r.prop, rel))
		}
	}
	if len(raceEntries) < 2 {
		failures = append(failures,
			fmt.Sprintf("phase 3: only %d events in race history -- expected both writers to land",
				len(raceEntries)))
	}

	send(boot, "exit;")

	if len(failures) > 0 {
		fmt.Println("\n=== self-check FAILED ===")
		for _, f := range failures {
			fmt.Println("  " + f)
		}
		os.Exit(1)
	}
	fmt.Println("\n=== self-check OK ===")
	fmt.Println("  verified 3 snapshot invariants + multi-editor race")
}
