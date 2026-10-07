// String-escape smoke, Go driver (#756); run by run-string-escapes.sh. Strings holding control
// characters go in through Call and must read back equal.
package main

import (
	"encoding/json"
	"fmt"
	"os"

	orly "github.com/orlyatomics/orly/clients/go"
)

const pkg = "string_escapes"

var values = []string{
	"plain",
	"quote \" and backslash \\",
	"a\nb",
	"a\tb",
	"a\rb",
	"a\fb",
	"a\x01b",
	"a\x1fb",
	"a\x7fb",
	"line one\r\nline two\n\n\ttabbed",
	"literal backslash-n: a\\nb",
}

func fail(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "STRING ESCAPES FAIL (go): "+format+"\n", args...)
	os.Exit(1)
}

func must[T any](v T, err error) T {
	if err != nil {
		fail("%v", err)
	}
	return v
}

func main() {
	c := must(orly.ConnectURL(os.Getenv("ORLY_URL")))
	defer c.Close()
	must(c.NewSession())
	if err := c.Install(pkg, 1); err != nil {
		fail("%v", err)
	}
	pov := must(c.NewPov())
	for n, value := range values {
		must(c.Call(pov, pkg, "ps", map[string]any{"k": n, "s": value}))
		var got string
		if err := json.Unmarshal(must(c.Call(pov, pkg, "gs", map[string]any{"k": n})), &got); err != nil {
			fail("read %d: %v", n, err)
		}
		if got != value {
			fail("wrote %q, read back %q", value, got)
		}
	}
	fmt.Printf("go: %d strings round-tripped\n", len(values))
}
