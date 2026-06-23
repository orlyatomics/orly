// Wikimedia hourly pageviews demo, in Go.
//
// Mirrors demo.py: same scenario shape, same self-check, same output
// layout. Provided so readers can pick whichever language matches
// their environment. Because the two drivers seed their RNGs
// differently, the per-page totals between Python and Go runs differ
// in absolute value -- but each driver's self-check uses its own
// derived ground truth, so both pass independently.
//
// Two workload sizes (controlled by DEMO_SCALE env var, mirroring
// demo.py):
//
//	DEMO_SCALE=small  -- 4 writers x 200 events x 48 keys (CI-friendly).
//	anything else     -- 8 writers x 250 events x 96 keys (showcase).
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
	"math/rand"
	"os"
	"sort"
	"strings"
	"sync"
	"time"

	orly "github.com/orlyatomics/orly/clients/go"
)

var (
	pages = []string{"Donald_Trump", "Taylor_Swift", "ChatGPT", "Wikipedia"}

	// Populated from DEMO_SCALE in initScale().
	numWriters      int
	eventsPerWriter int
	hours           []int
)

func initScale() {
	numHours := 24
	numWriters = 8
	eventsPerWriter = 250
	if strings.ToLower(os.Getenv("DEMO_SCALE")) == "small" {
		numWriters = 4
		eventsPerWriter = 200
		numHours = 12
	}
	hours = make([]int, numHours)
	for i := 0; i < numHours; i++ {
		hours[i] = 2026060100 + i
	}
}

type event struct {
	page string
	hour int
	n    int
}

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

// asInt decodes a result orlyi serialises as a JSON float into int.
func asInt(raw json.RawMessage) int {
	var n float64
	if err := json.Unmarshal(raw, &n); err != nil {
		die(fmt.Sprintf("expected number result, got %s", raw))
	}
	return int(n)
}

func incrementView(c *orly.Client, pov, page string, hour, n int) {
	must(c.Call(pov, "views", "increment_view",
		map[string]any{"lang": "en", "page": page, "hour": hour, "n": n}))
}

func viewsInHour(c *orly.Client, pov, page string, hour int) int {
	return asInt(must(c.Call(pov, "views", "views_in_hour",
		map[string]any{"lang": "en", "page": page, "hour": hour})))
}

func viewsTotal(c *orly.Client, pov, page string, hStart, hEnd int) int {
	return asInt(must(c.Call(pov, "views", "views_total",
		map[string]any{"lang": "en", "page": page, "h_start": hStart, "h_end": hEnd})))
}

func generateEvents(seed int64, count int) []event {
	rng := rand.New(rand.NewSource(seed))
	out := make([]event, count)
	for i := 0; i < count; i++ {
		out[i] = event{
			page: pages[rng.Intn(len(pages))],
			hour: hours[rng.Intn(len(hours))],
			n:    1 + rng.Intn(5),
		}
	}
	return out
}

func writer(pov string, events []event, wg *sync.WaitGroup) {
	defer wg.Done()
	c, err := orly.Connect()
	if err != nil {
		die(err.Error())
	}
	defer c.Close()
	must(c.NewSession())
	for _, e := range events {
		incrementView(c, pov, e.page, e.hour, e.n)
	}
}

// commaize prints non-negative ints with thousands separators (123 -> "123", 1234 -> "1,234").
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

func main() {
	initScale()

	// Generate all events deterministically, then split per writer.
	allEvents := make([][]event, numWriters)
	totalEvents := 0
	for w := 0; w < numWriters; w++ {
		allEvents[w] = generateEvents(int64(w)*17+3, eventsPerWriter)
		totalEvents += len(allEvents[w])
	}

	// Ground truth: what each (page, hour) counter should be after ingest.
	type key struct {
		page string
		hour int
	}
	ground := map[key]int{}
	for _, batch := range allEvents {
		for _, e := range batch {
			ground[key{e.page, e.hour}] += e.n
		}
	}

	// Bootstrap connection: install package + create the shared POV.
	boot, err := orly.Connect()
	if err != nil {
		die(err.Error())
	}
	must(boot.NewSession())
	check(boot.Install("views", 1))
	pov := must(boot.NewPov())
	fmt.Printf("pov: %s\n", pov)
	fmt.Printf("writers: %d, events per writer: %d, total events: %s\n",
		numWriters, eventsPerWriter, commaize(totalEvents))
	fmt.Printf("hot keyspace: %d pages × %d hours = %d keys\n",
		len(pages), len(hours), len(pages)*len(hours))

	// Pre-initialise every (page, hour) so writers all take the += branch.
	fmt.Println("\npre-initialising all hot keys to 0...")
	for _, p := range pages {
		for _, h := range hours {
			incrementView(boot, pov, p, h, 0)
		}
	}

	// Ingest in parallel.
	var wg sync.WaitGroup
	t0 := time.Now()
	for w := 0; w < numWriters; w++ {
		wg.Add(1)
		go writer(pov, allEvents[w], &wg)
	}
	wg.Wait()
	elapsed := time.Since(t0)
	rate := float64(totalEvents) / elapsed.Seconds()
	fmt.Printf("\ningest done in %.2fs (%s events/sec end-to-end with %d writers)\n",
		elapsed.Seconds(), commaize(int(rate)), numWriters)

	// Per-key verification.
	fmt.Println()
	fmt.Println("=== verifying counters ===")
	var failures []string
	keys := make([]key, 0, len(ground))
	for k := range ground {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].page != keys[j].page {
			return keys[i].page < keys[j].page
		}
		return keys[i].hour < keys[j].hour
	})
	for _, k := range keys {
		got := viewsInHour(boot, pov, k.page, k.hour)
		if got != ground[k] {
			failures = append(failures, fmt.Sprintf("%s @ %d: got %d, want %d", k.page, k.hour, got, ground[k]))
		}
	}

	// Per-page summary (one line per page instead of 96 lines).
	dash16 := strings.Repeat("-", 16)
	dash12 := strings.Repeat("-", 12)
	fmt.Printf("  %-16s  %12s  %12s\n", "page", "total", "expected")
	fmt.Printf("  %-16s  %12s  %12s\n", dash16, dash12, dash12)
	grandGot, grandWant := 0, 0
	for _, page := range pages {
		gotTotal := viewsTotal(boot, pov, page, hours[0], hours[len(hours)-1])
		wantTotal := 0
		for k, v := range ground {
			if k.page == page {
				wantTotal += v
			}
		}
		marker := "✓"
		if gotTotal != wantTotal {
			marker = "✗"
			failures = append(failures, fmt.Sprintf("%s total: got %d, want %d", page, gotTotal, wantTotal))
		}
		fmt.Printf("  %-16s  %12s  %12s  %s\n", page, commaize(gotTotal), commaize(wantTotal), marker)
		grandGot += gotTotal
		grandWant += wantTotal
	}
	fmt.Printf("  %-16s  %12s  %12s\n", dash16, dash12, dash12)
	fmt.Printf("  %-16s  %12s  %12s\n", "TOTAL", commaize(grandGot), commaize(grandWant))

	fmt.Println()
	fmt.Println("=== the trick ===")
	fmt.Printf("  %d concurrent WebSocket writers all `+=` into the same\n", numWriters)
	fmt.Printf("  %d hot keys, with zero locking. Field calls\n", len(pages)*len(hours))
	fmt.Println("  commute; the merge machinery aggregates safely.")

	check(boot.Exit())
	boot.Close()

	if len(failures) > 0 {
		fmt.Println("\n=== self-check FAILED ===")
		for i, f := range failures {
			if i >= 20 {
				break
			}
			fmt.Println("  " + f)
		}
		os.Exit(1)
	}
	fmt.Println("\n=== self-check OK ===")
	fmt.Printf("  verified %d per-key counters + %d per-page totals across %d concurrent writers\n",
		len(ground), len(pages), numWriters)
}
