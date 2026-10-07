// Keyset-paging smoke, Go driver (#735); run by run-keyset-paging.sh after keyset_paging.mjs
// has written group 1's edges. Client.Pages walks them all, once each and in order: the cursor
// comes back as a JSON float and must go back as an int.
package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strconv"

	orly "github.com/orlyatomics/orly/clients/go"
)

const pkg = "keyset_paging"

func fail(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "KEYSET PAGING FAIL (go): "+format+"\n", args...)
	os.Exit(1)
}

func must[T any](v T, err error) T {
	if err != nil {
		fail("%v", err)
	}
	return v
}

func env(name string) int {
	return must(strconv.Atoi(os.Getenv(name)))
}

type row struct {
	E float64 `json:"e"`
	W float64 `json:"w"`
}

func main() {
	rowCount, page := env("ROWS"), env("PAGE")
	c := must(orly.ConnectURL(os.Getenv("ORLY_URL")))
	defer c.Close()
	must(c.NewSession())
	if err := c.Install(pkg, 1); err != nil {
		fail("%v", err)
	}
	pov := must(c.NewPov())

	next, pages := 0, 0
	err := c.Pages(pov, pkg, "page", map[string]any{"g": 1, "last": -1, "n": page}, orly.PageOptions{PageSize: page},
		func(rows []json.RawMessage) error {
			pages++
			for _, raw := range rows {
				var r row
				if err := json.Unmarshal(raw, &r); err != nil {
					return err
				}
				if int(r.E) != next {
					return fmt.Errorf("edge %v where %d was next", r.E, next)
				}
				next++
			}
			return nil
		})
	if err != nil {
		fail("%v", err)
	}
	if next != rowCount {
		fail("Pages saw %d edges, not %d", next, rowCount)
	}

	// ErrStopPages ends paging early without an error.
	stopped := 0
	err = c.Pages(pov, pkg, "page", map[string]any{"g": 1, "last": -1, "n": 3}, orly.PageOptions{},
		func(rows []json.RawMessage) error {
			stopped++
			if stopped == 2 {
				return orly.ErrStopPages
			}
			return nil
		})
	if err != nil || stopped != 2 {
		fail("stopping after 2 pages: %v after %d pages", err, stopped)
	}
	// Without a first cursor it refuses before calling.
	if err := c.Pages(pov, pkg, "page", map[string]any{"g": 1, "n": 3}, orly.PageOptions{},
		func([]json.RawMessage) error { return nil }); err == nil || errors.Is(err, orly.ErrStopPages) {
		fail("Pages without a first cursor: %v", err)
	}
	fmt.Printf("go: Pages ok: %d edges in %d pages\n", next, pages)
}
