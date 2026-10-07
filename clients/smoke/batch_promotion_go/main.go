// Batch-promotion smoke, Go driver (#751); run by run-batch-promotion.sh. CallBatch and
// CallMany (sizes 1, 2, 8) with conditional writes, on all four POV flavours, checked after
// promotion through a fresh POV off global; each POV must still take writes afterwards.
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"reflect"
	"strconv"
	"strings"
	"time"

	orly "github.com/orlyatomics/orly/clients/go"
)

const pkg = "batch_promotion"

type want struct {
	getter, key string
	val         any
}

func fail(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "BATCH PROMOTION FAIL (go): "+format+"\n", args...)
	os.Exit(1)
}

func must[T any](v T, err error) T {
	if err != nil {
		fail("%v", err)
	}
	return v
}

var getter = map[string]string{"put_cond": "get_k", "put_ci": "get_c", "guard": "get_g", "put_plain": "get_p"}
var methods = []string{"put_cond", "put_ci", "guard", "put_plain"}

func argsFor(method, k string, n int) (map[string]any, any) {
	switch method {
	case "put_cond":
		return map[string]any{"k": k, "v": fmt.Sprintf("v%d", n)}, fmt.Sprintf("v%d", n)
	case "put_ci":
		v := 0.0
		if n > 5 {
			v = 1
		}
		return map[string]any{"k": k, "x": n}, v
	case "guard":
		return map[string]any{"k": k}, true
	}
	return map[string]any{"k": k, "v": fmt.Sprintf("p%d", n)}, fmt.Sprintf("p%d", n)
}

func waitPromoted(reader *orly.Client, expected []want, what string) {
	timeout := 60.0
	if s := os.Getenv("PROMOTION_TIMEOUT_S"); s != "" {
		timeout = must(strconv.ParseFloat(s, 64))
	}
	deadline := time.Now().Add(time.Duration(timeout * float64(time.Second)))
	for {
		pov := must(reader.SendString("new fast private pov;"))
		calls := make([]orly.Call, len(expected))
		for i, w := range expected {
			calls[i] = orly.Call{Pkg: pkg, Method: w.getter, Args: map[string]any{"k": w.key}}
		}
		var got []any
		if err := json.Unmarshal(must(reader.CallMany(pov, calls)), &got); err != nil {
			fail("%s: %v", what, err)
		}
		missing := 0
		for i, w := range expected {
			if !reflect.DeepEqual(got[i], w.val) {
				missing++
			}
		}
		if missing == 0 {
			fmt.Printf("go: %s: %d writes promoted\n", what, len(expected))
			return
		}
		if time.Now().After(deadline) {
			fail("%s: %d of %d acknowledged writes never reached global", what, missing, len(expected))
		}
		time.Sleep(500 * time.Millisecond)
	}
}

func main() {
	url := os.Getenv("ORLY_URL")
	c := must(orly.ConnectURL(url))
	defer c.Close()
	reader := must(orly.ConnectURL(url))
	defer reader.Close()
	must(c.NewSession())
	must(reader.NewSession())
	if err := c.Install(pkg, 1); err != nil {
		fail("install: %v", err)
	}
	for _, flav := range []string{"safe shared", "safe private", "fast shared", "fast private"} {
		tag := "go-" + strings.ReplaceAll(flav, " ", "-")
		pov := must(c.SendString("new " + flav + " pov;"))
		var expected []want
		n := 0
		for _, size := range []int{1, 2, 8} {
			for _, method := range []string{"put_cond", "put_ci"} {
				var argsList []map[string]any
				for i := 0; i < size; i++ {
					n++
					args, v := argsFor(method, fmt.Sprintf("%s-%s-%d-%d", tag, method, size, i), n)
					argsList = append(argsList, args)
					expected = append(expected, want{getter[method], args["k"].(string), v})
				}
				var res []any
				if err := json.Unmarshal(must(c.CallBatch(pov, pkg, method, argsList)), &res); err != nil || len(res) != size {
					fail("%s: CallBatch %s x%d returned %v (%v)", flav, method, size, res, err)
				}
			}
			var calls []orly.Call
			for i := 0; i < size; i++ {
				n++
				method := methods[i%len(methods)]
				args, v := argsFor(method, fmt.Sprintf("%s-mixed-%d-%d", tag, size, i), n)
				calls = append(calls, orly.Call{Pkg: pkg, Method: method, Args: args})
				expected = append(expected, want{getter[method], args["k"].(string), v})
			}
			var res []any
			if err := json.Unmarshal(must(c.CallMany(pov, calls)), &res); err != nil || len(res) != size {
				fail("%s: CallMany x%d returned %v (%v)", flav, size, res, err)
			}
		}
		waitPromoted(reader, expected, flav+" batches")
		k := tag + "-after"
		if _, err := c.Call(pov, pkg, "put_cond", map[string]any{"k": k, "v": "after"}); err != nil {
			fail("%s: the POV refused a write after its batches promoted: %v", flav, err)
		}
		waitPromoted(reader, []want{{"get_k", k, "after"}}, flav+" later write")
	}
	fmt.Println("go: batch promotion OK on all four POV flavours")
}
