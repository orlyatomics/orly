// Multi-agent knowledge-graph demo, in Go.
//
// Mirrors demo.py: same corpus, same scenario shape, same self-check,
// same output layout. Provided so readers can pick whichever language
// matches their environment. Each driver verifies its own derived
// ground truth, so the two pass independently.
//
// Two workload sizes (controlled by DEMO_SCALE, mirroring demo.py):
//
//	DEMO_SCALE=small  -- 4 agents over the first 20 docs (CI-friendly).
//	anything else     -- 8 agents over all 40 docs (showcase).
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
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

const wsURL = "ws://127.0.0.1:8082/"

// Per-call recv timeout. Mirrors wikipedia-pageviews/demo.go: normal
// latency is well under a second; 30s is generous-but-finite so a hung
// orlyi surfaces as a failed read rather than a multi-minute CI timeout.
const wsTimeout = 30 * time.Second

// doc represents one corpus entry: the human-readable text plus the
// "extraction" the demo treats as ground truth (entity -> tag list).
type doc struct {
	text     string
	entities map[string][]string
}

// corpus is the hand-written corpus shared with demo.py. Each entry's
// {entity: [tags]} dict IS the structured extraction; the sentence
// text is human-readable flavor only.
//
// Hot entities (Python, OpenAI, Anthropic, Claude, GPT-4, Microsoft,
// Google) appear in many docs to drive real contention on the
// counters and tag sets.
var corpus = []doc{
	{"Python and Rust dominate modern systems work at OpenAI.", map[string][]string{
		"Python": {"language", "popular"}, "Rust": {"language"},
		"OpenAI": {"company", "ai-lab"}}},
	{"Anthropic trained Claude on a custom Transformer architecture.", map[string][]string{
		"Anthropic": {"company", "ai-lab"}, "Claude": {"model", "llm"},
		"Transformer": {"model"}}},
	{"GitHub Copilot now offers Claude alongside GPT-4 for code.", map[string][]string{
		"GitHub": {"company", "tool"}, "Claude": {"model", "llm"},
		"GPT-4": {"model", "llm"}}},
	{"Linus Torvalds still maintains Linux from a laptop running Git.", map[string][]string{
		"Linus": {"person"}, "Linux": {"tool", "open-source"},
		"Git": {"tool", "open-source"}}},
	{"Guido van Rossum joined Microsoft to improve Python performance.", map[string][]string{
		"Guido": {"person"}, "Microsoft": {"company"},
		"Python": {"language", "popular"}}},
	{"Kubernetes manages workloads at Google, Meta, and Microsoft.", map[string][]string{
		"Kubernetes": {"tool"}, "Google": {"company"},
		"Meta": {"company"}, "Microsoft": {"company"}}},
	{"Docker simplified deployment for Python and JavaScript developers.", map[string][]string{
		"Docker": {"tool"}, "Python": {"language", "popular"},
		"JavaScript": {"language", "popular"}}},
	{"Meta released Llama as a competitive open-source LLM.", map[string][]string{
		"Meta":  {"company", "ai-lab"},
		"Llama": {"model", "llm", "open-source"}}},
	{"PostgreSQL remains the database of choice at OpenAI for analytics.", map[string][]string{
		"PostgreSQL": {"tool", "database"}, "OpenAI": {"company", "ai-lab"}}},
	{"Sam Altman discussed GPT-4 in a recent OpenAI announcement.", map[string][]string{
		"Sam": {"person"}, "GPT-4": {"model", "llm"},
		"OpenAI": {"company", "ai-lab"}}},
	{"Rust is gaining traction at Microsoft for systems-level libraries.", map[string][]string{
		"Rust": {"language"}, "Microsoft": {"company"}}},
	{"TypeScript powers most modern JavaScript codebases at Google.", map[string][]string{
		"TypeScript": {"language"}, "JavaScript": {"language", "popular"},
		"Google": {"company"}}},
	{"Java still dominates enterprise software at Microsoft and Google.", map[string][]string{
		"Java": {"language", "enterprise"}, "Microsoft": {"company"},
		"Google": {"company"}}},
	{"Ruby on Rails was created by a Danish developer; GitHub uses it.", map[string][]string{
		"Ruby": {"language"}, "GitHub": {"company", "tool"}}},
	{"Go was designed at Google to simplify backend programming.", map[string][]string{
		"Go": {"language"}, "Google": {"company"}}},
	{"Kubernetes orchestrates Docker containers in production at Meta.", map[string][]string{
		"Kubernetes": {"tool"}, "Docker": {"tool"},
		"Meta": {"company", "ai-lab"}}},
	{"Claude can analyze long documents with its large context window.", map[string][]string{
		"Claude": {"model", "llm"}}},
	{"GPT-4 powers many tools released by OpenAI in recent years.", map[string][]string{
		"GPT-4": {"model", "llm"}, "OpenAI": {"company", "ai-lab"}}},
	{"Linux servers run most of the infrastructure at Google.", map[string][]string{
		"Linux": {"tool", "open-source"}, "Google": {"company"}}},
	{"Git is the version control system at Microsoft, Google, and Anthropic.", map[string][]string{
		"Git": {"tool", "open-source"}, "Microsoft": {"company"},
		"Google": {"company"}, "Anthropic": {"company", "ai-lab"}}},
	{"Llama runs locally on a laptop with sufficient RAM.", map[string][]string{
		"Llama": {"model", "llm", "open-source"}}},
	{"PostgreSQL backs most production systems at GitHub and Anthropic.", map[string][]string{
		"PostgreSQL": {"tool", "database"}, "GitHub": {"company", "tool"},
		"Anthropic": {"company", "ai-lab"}}},
	{"Python's ecosystem includes scientific tools widely used at OpenAI.", map[string][]string{
		"Python": {"language", "popular"}, "OpenAI": {"company", "ai-lab"}}},
	{"Anthropic recently expanded the Claude API for enterprise customers.", map[string][]string{
		"Anthropic": {"company", "ai-lab"}, "Claude": {"model", "llm"}}},
	{"Guido and Linus both shaped modern open-source culture.", map[string][]string{
		"Guido": {"person"}, "Linus": {"person"}}},
	{"Sam Altman leads OpenAI through aggressive product launches.", map[string][]string{
		"Sam": {"person"}, "OpenAI": {"company", "ai-lab"}}},
	{"Transformers underpin both Claude and GPT-4 architectures.", map[string][]string{
		"Transformer": {"model"}, "Claude": {"model", "llm"},
		"GPT-4": {"model", "llm"}}},
	{"Meta engineers contribute heavily to PyTorch and the Python ecosystem.", map[string][]string{
		"Meta": {"company", "ai-lab"}, "Python": {"language", "popular"}}},
	{"Rust adoption is rising at Microsoft, Google, and Meta.", map[string][]string{
		"Rust": {"language"}, "Microsoft": {"company"},
		"Google": {"company"}, "Meta": {"company"}}},
	{"JavaScript remains the most-used language on GitHub by far.", map[string][]string{
		"JavaScript": {"language", "popular"}, "GitHub": {"company", "tool"}}},
	{"TypeScript brings type safety to JavaScript codebases at Microsoft.", map[string][]string{
		"TypeScript": {"language"}, "JavaScript": {"language", "popular"},
		"Microsoft": {"company"}}},
	{"Go's concurrency model influenced design choices at Anthropic.", map[string][]string{
		"Go": {"language"}, "Anthropic": {"company", "ai-lab"}}},
	{"Linux Foundation projects include Kubernetes, Docker, and Git.", map[string][]string{
		"Linux": {"tool", "open-source"}, "Kubernetes": {"tool"},
		"Docker": {"tool"}, "Git": {"tool", "open-source"}}},
	{"Java powers Android development; Google supports it heavily.", map[string][]string{
		"Java": {"language", "enterprise"}, "Google": {"company"}}},
	{"Ruby developers often migrate to Python or Go for performance.", map[string][]string{
		"Ruby": {"language"}, "Python": {"language", "popular"},
		"Go": {"language"}}},
	{"Claude can write and review Python, JavaScript, and Go code.", map[string][]string{
		"Claude": {"model", "llm"}, "Python": {"language", "popular"},
		"JavaScript": {"language", "popular"}, "Go": {"language"}}},
	{"GPT-4 fine-tunes are restricted compared to open Llama weights.", map[string][]string{
		"GPT-4": {"model", "llm"},
		"Llama": {"model", "llm", "open-source"}}},
	{"OpenAI and Anthropic compete in the enterprise LLM market.", map[string][]string{
		"OpenAI":    {"company", "ai-lab"},
		"Anthropic": {"company", "ai-lab"}}},
	{"Microsoft has invested heavily in OpenAI and integrated GPT-4.", map[string][]string{
		"Microsoft": {"company"}, "OpenAI": {"company", "ai-lab"},
		"GPT-4": {"model", "llm"}}},
	{"Docker images at Anthropic typically ship Python and Rust binaries.", map[string][]string{
		"Docker": {"tool"}, "Anthropic": {"company", "ai-lab"},
		"Python": {"language", "popular"}, "Rust": {"language"}}},
}

// Populated from DEMO_SCALE in initScale().
var (
	numAgents int
	numDocs   int
)

func initScale() {
	numAgents = 8
	numDocs = 40
	if strings.ToLower(os.Getenv("DEMO_SCALE")) == "small" {
		numAgents = 4
		numDocs = 20
	}
}

// canonicalPair returns (smaller, larger) so add_cooccur(A,B) and
// add_cooccur(B,A) collapse to the same key.
func canonicalPair(a, b string) (string, string) {
	if a < b {
		return a, b
	}
	return b, a
}

type mentionKey struct {
	entity string
	docID  int
}

type pairKey struct {
	a, b string
}

// tagRecord is one provenance fact: agent asserted tag. It's the set
// element behind <['entity', e]>::({<{.tag, .agent}>}).
type tagRecord struct {
	tag, agent string
}

func computeGroundTruth(docs []doc, numAgents int) (
	expectedTagRecords map[string]map[tagRecord]bool,
	expectedMentions map[mentionKey]int,
	expectedCooccur map[pairKey]int,
) {
	// Tags carry provenance: doc `docID` is processed by agent
	// `docID % numAgents` (the round-robin split in main), so the
	// expected tag set is the set of (tag, agent) records.
	expectedTagRecords = map[string]map[tagRecord]bool{}
	expectedMentions = map[mentionKey]int{}
	expectedCooccur = map[pairKey]int{}
	for docID, d := range docs {
		agentName := fmt.Sprintf("agent-%d", docID%numAgents)
		for entity, tags := range d.entities {
			if expectedTagRecords[entity] == nil {
				expectedTagRecords[entity] = map[tagRecord]bool{}
			}
			for _, t := range tags {
				expectedTagRecords[entity][tagRecord{t, agentName}] = true
			}
			expectedMentions[mentionKey{entity, docID}] = 1
		}
		names := make([]string, 0, len(d.entities))
		for n := range d.entities {
			names = append(names, n)
		}
		sort.Strings(names)
		for i, a := range names {
			for _, b := range names[i+1:] {
				ca, cb := canonicalPair(a, b)
				expectedCooccur[pairKey{ca, cb}]++
			}
		}
	}
	return
}

func send(c *websocket.Conn, stmt string) (interface{}, error) {
	deadline := time.Now().Add(wsTimeout)
	if err := c.SetWriteDeadline(deadline); err != nil {
		return nil, err
	}
	if err := c.WriteMessage(websocket.TextMessage, []byte(stmt)); err != nil {
		return nil, err
	}
	if err := c.SetReadDeadline(deadline); err != nil {
		return nil, err
	}
	_, raw, err := c.ReadMessage()
	if err != nil {
		return nil, err
	}
	var r struct {
		Status string      `json:"status"`
		Result interface{} `json:"result"`
	}
	if err := json.Unmarshal(raw, &r); err != nil {
		return nil, fmt.Errorf("bad reply json: %w (raw: %s)", err, raw)
	}
	if r.Status != "ok" {
		return nil, fmt.Errorf("%s\n  -> %s", stmt, raw)
	}
	return r.Result, nil
}

func mustSend(c *websocket.Conn, stmt string) interface{} {
	r, err := send(c, stmt)
	if err != nil {
		panic(err)
	}
	return r
}

func addTag(c *websocket.Conn, pov, entity, tag, agent string) {
	mustSend(c, fmt.Sprintf(
		`try {%s} graph add_tag <{.e: %q, .t: %q, .agent: %q}>;`,
		pov, entity, tag, agent))
}

func addMention(c *websocket.Conn, pov, entity string, docID int) {
	mustSend(c, fmt.Sprintf(
		`try {%s} graph add_mention <{.e: %q, .d: %d}>;`,
		pov, entity, docID))
}

func addCooccur(c *websocket.Conn, pov, a, b string) {
	mustSend(c, fmt.Sprintf(
		`try {%s} graph add_cooccur <{.a: %q, .b: %q}>;`,
		pov, a, b))
}

func tagsForEntity(c *websocket.Conn, pov, entity string) map[tagRecord]bool {
	r := mustSend(c, fmt.Sprintf(
		`try {%s} graph tags_for_entity <{.e: %q}>;`,
		pov, entity))
	out := map[tagRecord]bool{}
	if r == nil {
		return out
	}
	// graph.orly returns a set of <{.tag, .agent}> records; WS wire form
	// is a JSON array of objects.
	for _, v := range r.([]interface{}) {
		rec := v.(map[string]interface{})
		out[tagRecord{rec["tag"].(string), rec["agent"].(string)}] = true
	}
	return out
}

func mentionCount(c *websocket.Conn, pov, entity string, docID int) int {
	r := mustSend(c, fmt.Sprintf(
		`try {%s} graph mention_count <{.e: %q, .d: %d}>;`,
		pov, entity, docID))
	return int(r.(float64))
}

func mentionsTotal(c *websocket.Conn, pov, entity string, dStart, dEnd int) int {
	r := mustSend(c, fmt.Sprintf(
		`try {%s} graph mentions_total <{.e: %q, .d_start: %d, .d_end: %d}>;`,
		pov, entity, dStart, dEnd))
	return int(r.(float64))
}

func cooccurCount(c *websocket.Conn, pov, a, b string) int {
	r := mustSend(c, fmt.Sprintf(
		`try {%s} graph cooccur_count <{.a: %q, .b: %q}>;`,
		pov, a, b))
	return int(r.(float64))
}

// agentDoc is one (docID, doc) pair assigned to an agent.
type agentDoc struct {
	id  int
	doc doc
}

// agent runs one writer goroutine: opens a fresh WS, opens a session,
// ingests its slice of (doc_id, extraction) pairs into the shared POV.
// Every tag it asserts is stamped with this agent's name for provenance.
func agent(id int, pov string, docs []agentDoc, wg *sync.WaitGroup) {
	defer wg.Done()
	agentName := fmt.Sprintf("agent-%d", id)
	dialer := *websocket.DefaultDialer
	dialer.HandshakeTimeout = wsTimeout
	c, _, err := dialer.Dial(wsURL, nil)
	if err != nil {
		panic(err)
	}
	defer c.Close()
	mustSend(c, "new session;")
	for _, ad := range docs {
		for entity, tags := range ad.doc.entities {
			for _, t := range tags {
				addTag(c, pov, entity, t, agentName)
			}
			addMention(c, pov, entity, ad.id)
		}
		names := make([]string, 0, len(ad.doc.entities))
		for n := range ad.doc.entities {
			names = append(names, n)
		}
		sort.Strings(names)
		for i, a := range names {
			for _, b := range names[i+1:] {
				ca, cb := canonicalPair(a, b)
				addCooccur(c, pov, ca, cb)
			}
		}
	}
}

// commaize prints non-negative ints with thousands separators.
func commaize(n int) string {
	s := fmt.Sprintf("%d", n)
	if len(s) <= 3 {
		return s
	}
	out := make([]byte, 0, len(s)+len(s)/3)
	for i := 0; i < len(s); i++ {
		if i > 0 && (len(s)-i)%3 == 0 {
			out = append(out, ',')
		}
		out = append(out, s[i])
	}
	return string(out)
}

func sortedTagList(m map[string]bool) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func recordSetsEqual(a, b map[tagRecord]bool) bool {
	if len(a) != len(b) {
		return false
	}
	for k := range a {
		if !b[k] {
			return false
		}
	}
	return true
}

// sortedRecords renders a record set as sorted "tag@agent" strings, for
// readable failure messages.
func sortedRecords(m map[tagRecord]bool) []string {
	out := make([]string, 0, len(m))
	for rec := range m {
		out = append(out, rec.tag+"@"+rec.agent)
	}
	sort.Strings(out)
	return out
}

// tagSummary rolls a provenance record set up to distinct tags,
// annotating any tag more than one agent independently asserted (×N).
func tagSummary(records map[tagRecord]bool) string {
	agentsByTag := map[string]map[string]bool{}
	for rec := range records {
		if agentsByTag[rec.tag] == nil {
			agentsByTag[rec.tag] = map[string]bool{}
		}
		agentsByTag[rec.tag][rec.agent] = true
	}
	tags := make([]string, 0, len(agentsByTag))
	for t := range agentsByTag {
		tags = append(tags, t)
	}
	sort.Strings(tags)
	parts := make([]string, 0, len(tags))
	for _, t := range tags {
		if n := len(agentsByTag[t]); n > 1 {
			parts = append(parts, fmt.Sprintf("%s×%d", t, n))
		} else {
			parts = append(parts, t)
		}
	}
	return strings.Join(parts, ",")
}

func main() {
	initScale()

	docs := corpus[:numDocs]
	expectedTagRecords, expectedMentions, expectedCooccur := computeGroundTruth(docs, numAgents)

	// Round-robin assignment so hot entities collide across agents.
	perAgent := make([][]agentDoc, numAgents)
	for id, d := range docs {
		perAgent[id%numAgents] = append(perAgent[id%numAgents], agentDoc{id, d})
	}

	addTagCalls := 0 // one per (doc, entity, tag)
	for _, d := range docs {
		for _, tags := range d.entities {
			addTagCalls += len(tags)
		}
	}
	totalWrites := addTagCalls
	totalWrites += len(expectedMentions) // add_mention calls
	for _, n := range expectedCooccur {
		totalWrites += n // add_cooccur calls
	}

	// Bootstrap: install graph + create the shared POV.
	bootDialer := *websocket.DefaultDialer
	bootDialer.HandshakeTimeout = wsTimeout
	boot, _, err := bootDialer.Dial(wsURL, nil)
	if err != nil {
		panic(err)
	}
	mustSend(boot, "new session;")
	mustSend(boot, "install graph.1;")
	pov := mustSend(boot, "new safe shared pov;").(string)
	fmt.Printf("pov: %s\n", pov)
	fmt.Printf("agents: %d, docs: %d, entities: %d, unordered pairs: %d\n",
		numAgents, numDocs, len(expectedTagRecords), len(expectedCooccur))
	fmt.Printf("total writes from agents: %s\n", commaize(totalWrites))

	// Pre-initialise every key so concurrent agents take the
	// commutative |= / += branch (never the `new <-` create branch).
	fmt.Println("\npre-initialising all keys via the bootstrap session...")
	for entity, records := range expectedTagRecords {
		// Seed with the lex-first tag for determinism, stamped with a
		// dedicated "seed" agent; the agents union the rest.
		tagset := map[string]bool{}
		for rec := range records {
			tagset[rec.tag] = true
		}
		first := sortedTagList(tagset)[0]
		addTag(boot, pov, entity, first, "seed")
		records[tagRecord{first, "seed"}] = true
	}
	for mk := range expectedMentions {
		addMention(boot, pov, mk.entity, mk.docID)
	}
	for pk := range expectedCooccur {
		addCooccur(boot, pov, pk.a, pk.b)
	}

	// Pre-init seeded each key with one of the agent ops; adjust expectations.
	expectedMentionCount := map[mentionKey]int{}
	for k, v := range expectedMentions {
		expectedMentionCount[k] = v + 1
	}
	expectedCooccurCount := map[pairKey]int{}
	for k, v := range expectedCooccur {
		expectedCooccurCount[k] = v + 1
	}

	// Read-back barrier (issue #143): the pre-init writes committed on the
	// bootstrap session, but the agents are *different* sessions. If an agent's
	// `is known` predicate read runs before a seed is visible to it, it takes
	// the `new <- 1` create branch instead of the commutative `+= 1`, and that
	// stray Assign masks the commutative run on read -> lost increments. To
	// exclude that create-race (which the pre-init is meant to design out), open
	// a FRESH session and confirm every seeded key is visible from it before
	// fanning agents out; a fresh session seeing them means other fresh agent
	// sessions will too. Poll briefly to absorb any promotion-visibility lag.
	fmt.Println("read-back barrier: confirming all seeds are visible to a fresh session...")
	{
		vDialer := *websocket.DefaultDialer
		vDialer.HandshakeTimeout = wsTimeout
		verifier, _, err := vDialer.Dial(wsURL, nil)
		if err != nil {
			panic(err)
		}
		mustSend(verifier, "new session;")
		deadline := time.Now().Add(30 * time.Second)
		for {
			pending := 0
			for e := range expectedTagRecords {
				if len(tagsForEntity(verifier, pov, e)) == 0 {
					pending++
				}
			}
			for mk := range expectedMentions {
				if mentionCount(verifier, pov, mk.entity, mk.docID) < 1 {
					pending++
				}
			}
			for pk := range expectedCooccur {
				if cooccurCount(verifier, pov, pk.a, pk.b) < 1 {
					pending++
				}
			}
			if pending == 0 {
				break
			}
			if time.Now().After(deadline) {
				verifier.Close()
				fmt.Fprintf(os.Stderr, "read-back barrier timed out; %d seed(s) never became visible to a fresh session\n", pending)
				os.Exit(1)
			}
			time.Sleep(50 * time.Millisecond)
		}
		verifier.Close()
	}

	// Fan out: each agent on its own WS, all concurrent.
	var wg sync.WaitGroup
	t0 := time.Now()
	for i := 0; i < numAgents; i++ {
		wg.Add(1)
		go agent(i, pov, perAgent[i], &wg)
	}
	wg.Wait()
	elapsed := time.Since(t0)
	rate := float64(totalWrites) / elapsed.Seconds()
	fmt.Printf("\ningest done in %.2fs (%s writes/sec end-to-end with %d agents)\n",
		elapsed.Seconds(), commaize(int(rate)), numAgents)

	// ------------------------------------------------------------------
	// Verify against ground truth.
	// ------------------------------------------------------------------
	fmt.Println()
	fmt.Println("=== verifying knowledge graph ===")
	var failures []string

	for entity, wantRecords := range expectedTagRecords {
		gotRecords := tagsForEntity(boot, pov, entity)
		if !recordSetsEqual(gotRecords, wantRecords) {
			failures = append(failures, fmt.Sprintf(
				"tags(%s): got %v, want %v",
				entity, sortedRecords(gotRecords), sortedRecords(wantRecords)))
		}
	}
	for mk, want := range expectedMentionCount {
		got := mentionCount(boot, pov, mk.entity, mk.docID)
		if got != want {
			failures = append(failures, fmt.Sprintf(
				"mention(%s, %d): got %d, want %d",
				mk.entity, mk.docID, got, want))
		}
	}
	for pk, want := range expectedCooccurCount {
		got := cooccurCount(boot, pov, pk.a, pk.b)
		if got != want {
			failures = append(failures, fmt.Sprintf(
				"cooccur(%s, %s): got %d, want %d",
				pk.a, pk.b, got, want))
		}
	}

	// Per-entity rollup, sorted hottest-first.
	perEntityMentions := map[string]int{}
	for mk, v := range expectedMentionCount {
		perEntityMentions[mk.entity] += v
	}
	entitiesByHeat := make([]string, 0, len(perEntityMentions))
	for e := range perEntityMentions {
		entitiesByHeat = append(entitiesByHeat, e)
	}
	sort.Slice(entitiesByHeat, func(i, j int) bool {
		if perEntityMentions[entitiesByHeat[i]] != perEntityMentions[entitiesByHeat[j]] {
			return perEntityMentions[entitiesByHeat[i]] > perEntityMentions[entitiesByHeat[j]]
		}
		return entitiesByHeat[i] < entitiesByHeat[j]
	})

	dash14 := strings.Repeat("-", 14)
	dash9 := strings.Repeat("-", 9)
	dash30 := strings.Repeat("-", 30)
	fmt.Printf("  %-14s  %9s  %9s  %s\n", "entity", "mentions", "expected", "tags (×contributing agents)")
	fmt.Printf("  %-14s  %9s  %9s  %s\n", dash14, dash9, dash9, dash30)
	for _, entity := range entitiesByHeat {
		gotTotal := mentionsTotal(boot, pov, entity, 0, numDocs-1)
		wantTotal := perEntityMentions[entity]
		marker := "✓"
		if gotTotal != wantTotal {
			marker = "✗"
			failures = append(failures, fmt.Sprintf(
				"mentions_total(%s): got %d, want %d",
				entity, gotTotal, wantTotal))
		}
		tagStr := tagSummary(expectedTagRecords[entity])
		fmt.Printf("  %-14s  %9d  %9d  %s  %s\n",
			entity, gotTotal, wantTotal, tagStr, marker)
	}

	// Top-10 cooccurrence pairs (by expected count).
	fmt.Println()
	fmt.Println("=== top cooccurrence pairs ===")
	pairsSorted := make([]pairKey, 0, len(expectedCooccurCount))
	for pk := range expectedCooccurCount {
		pairsSorted = append(pairsSorted, pk)
	}
	sort.Slice(pairsSorted, func(i, j int) bool {
		if expectedCooccurCount[pairsSorted[i]] != expectedCooccurCount[pairsSorted[j]] {
			return expectedCooccurCount[pairsSorted[i]] > expectedCooccurCount[pairsSorted[j]]
		}
		if pairsSorted[i].a != pairsSorted[j].a {
			return pairsSorted[i].a < pairsSorted[j].a
		}
		return pairsSorted[i].b < pairsSorted[j].b
	})
	topN := 10
	if len(pairsSorted) < topN {
		topN = len(pairsSorted)
	}
	for _, pk := range pairsSorted[:topN] {
		want := expectedCooccurCount[pk]
		got := cooccurCount(boot, pov, pk.a, pk.b)
		marker := "✓"
		if got != want {
			marker = "✗"
		}
		label := pk.a + " & " + pk.b
		fmt.Printf("  %-32s  %4d  (want %d)  %s\n", label, got, want, marker)
	}

	fmt.Println()
	fmt.Println("=== the trick ===")
	fmt.Printf("  %d concurrent agents all wrote into the same knowledge\n", numAgents)
	fmt.Println("  graph with zero coordination. Tag records (with per-agent")
	fmt.Println("  provenance) union into one set per entity, and the per-entity")
	fmt.Println("  / per-pair counters aggregate correctly: this is the shape")
	fmt.Println("  multi-agent LLM extraction pipelines keep reinventing badly.")

	mustSend(boot, "exit;")
	boot.Close()

	if len(failures) > 0 {
		fmt.Println("\n=== self-check FAILED ===")
		for i, f := range failures {
			if i >= 20 {
				break
			}
			fmt.Println("  " + f)
		}
		if len(failures) > 20 {
			fmt.Printf("  ... and %d more\n", len(failures)-20)
		}
		os.Exit(1)
	}
	totalTagRecords := 0
	for _, recs := range expectedTagRecords {
		totalTagRecords += len(recs)
	}
	fmt.Println("\n=== self-check OK ===")
	fmt.Printf("  verified %d tag provenance records across %d entities + %d mention counters + %d cooccurrence counters across %d concurrent agents\n",
		totalTagRecords, len(expectedTagRecords), len(expectedMentionCount), len(expectedCooccurCount), numAgents)
}
