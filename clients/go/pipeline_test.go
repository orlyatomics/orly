/*
   Copyright 2010-2026 Atomic Kismet Company

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

package orly

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	"github.com/gorilla/websocket"
)

type envelope struct {
	ID   int64  `json:"id"`
	Stmt string `json:"stmt"`
	Read bool   `json:"read"`
}

// pipelineServer answers envelopes. It collects `batch` of them, then answers
// them in reverse order (so replies arrive out of order); `fail` is the set of
// ids it answers with an exception. It records what it was sent and the most it
// saw unanswered at once.
type pipelineServer struct {
	mu          sync.Mutex
	sent        []envelope
	maxInFlight int
}

func newPipelineClient(t *testing.T, batch int, fail map[int64]bool, raw func(env envelope) (string, bool)) (*pipelineServer, *Client) {
	ps := &pipelineServer{}
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		var held []envelope
		flush := func() bool {
			for i := len(held) - 1; i >= 0; i-- {
				env := held[i]
				var reply string
				if custom, ok := raw(env); ok {
					reply = custom
				} else if fail[env.ID] {
					reply = fmt.Sprintf(`{"id": %d, "status": "exception", "result": "boom"}`, env.ID)
				} else {
					reply = fmt.Sprintf(`{"id": %d.000000, "status": "ok", "result": %q}`, env.ID, env.Stmt)
				}
				if conn.WriteMessage(websocket.TextMessage, []byte(reply)) != nil {
					return false
				}
			}
			held = nil
			return true
		}
		for {
			_, msg, err := conn.ReadMessage()
			if err != nil {
				return
			}
			var env envelope
			if json.Unmarshal(msg, &env) != nil {
				return
			}
			ps.mu.Lock()
			ps.sent = append(ps.sent, env)
			ps.mu.Unlock()
			held = append(held, env)
			ps.mu.Lock()
			if len(held) > ps.maxInFlight {
				ps.maxInFlight = len(held)
			}
			ps.mu.Unlock()
			if len(held) >= batch && !flush() {
				return
			}
		}
	}))
	t.Cleanup(s.Close)
	client, err := ConnectURL("ws" + strings.TrimPrefix(s.URL, "http"))
	if err != nil {
		t.Fatalf("ConnectURL: %v", err)
	}
	t.Cleanup(func() { client.Close() })
	return ps, client
}

func noRaw(envelope) (string, bool) { return "", false }

func TestPipelineResultsInOrderGiven(t *testing.T) {
	ps, c := newPipelineClient(t, 3, nil, noRaw)
	res, err := c.Pipeline([]PipelineItem{{Stmt: "a;", Read: true}, {Stmt: "b;", Read: true}, {Stmt: "c;"}}, PipelineOpts{})
	if err != nil {
		t.Fatal(err)
	}
	for i, want := range []string{`"a;"`, `"b;"`, `"c;"`} {
		if string(res[i]) != want {
			t.Fatalf("result %d = %s, want %s", i, res[i], want)
		}
	}
	if len(ps.sent) != 3 || !ps.sent[0].Read || ps.sent[2].Read || ps.sent[1].ID != 1 {
		t.Fatalf("envelopes = %+v", ps.sent)
	}
}

func TestPipelineWindowBoundsWhatIsInFlight(t *testing.T) {
	// The server answers once 2 are held, so a window of 2 never sees more than 2 at once.
	ps, c := newPipelineClient(t, 2, nil, noRaw)
	items := make([]PipelineItem, 6)
	for i := range items {
		items[i] = PipelineItem{Stmt: fmt.Sprintf("r%d;", i), Read: true}
	}
	res, err := c.Pipeline(items, PipelineOpts{Window: 2})
	if err != nil || len(res) != 6 {
		t.Fatalf("res=%v err=%v", res, err)
	}
	if ps.maxInFlight != 2 {
		t.Fatalf("max in flight = %d, want 2", ps.maxInFlight)
	}
}

func TestPipelineReadsEveryReplyThenReturnsTheFirstFailure(t *testing.T) {
	_, c := newPipelineClient(t, 4, map[int64]bool{1: true, 2: true}, noRaw)
	items := []PipelineItem{{Stmt: "a;", Read: true}, {Stmt: "b;", Read: true}, {Stmt: "c;", Read: true}, {Stmt: "d;", Read: true}}
	res, err := c.Pipeline(items, PipelineOpts{})
	if err == nil || !strings.Contains(err.Error(), "b;") {
		t.Fatalf("err = %v, want the failure of b;", err)
	}
	if string(res[0]) != `"a;"` || res[1] != nil || string(res[3]) != `"d;"` {
		t.Fatalf("results = %s", res)
	}
	// The connection is still in step.
	// (This server answers in batches of 4, so send another 4.)
	again := []PipelineItem{{Stmt: "e;"}, {Stmt: "f;"}, {Stmt: "g;"}, {Stmt: "h;"}}
	if got, err := c.Pipeline(again, PipelineOpts{}); err == nil || string(got[0]) != `"e;"` || string(got[3]) != `"h;"` {
		t.Fatalf("after a failure: %s %v", got, err)
	}
}

func TestPipelineReplyWithoutAnIDIsAnError(t *testing.T) {
	_, c := newPipelineClient(t, 1, nil, func(envelope) (string, bool) { return `{"status": "exception", "result": "bad"}`, true })
	if _, err := c.Pipeline([]PipelineItem{{Stmt: "a;", Read: true}}, PipelineOpts{}); err == nil || !strings.Contains(err.Error(), "matches no statement") {
		t.Fatalf("err = %v", err)
	}
}

func TestPipelineDroppedConnectionIsAConnError(t *testing.T) {
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		conn.ReadMessage()
		conn.Close()
	}))
	defer s.Close()
	c, err := ConnectURL("ws" + strings.TrimPrefix(s.URL, "http"))
	if err != nil {
		t.Fatal(err)
	}
	_, err = c.Pipeline([]PipelineItem{{Stmt: "a;", Read: true}, {Stmt: "b;", Read: true}}, PipelineOpts{})
	var ce *connError
	if err == nil || !asConnError(err, &ce) {
		t.Fatalf("err = %v, want a connError", err)
	}
}

func asConnError(err error, target **connError) bool {
	ce, ok := err.(*connError)
	if ok {
		*target = ce
	}
	return ok
}

func TestCallPipelinedBuildsOneTryPerRecord(t *testing.T) {
	ps, c := newPipelineClient(t, 2, nil, noRaw)
	res, err := c.CallPipelined("p1", "pkg/v1", "get", []map[string]any{{"k": 1}, {"k": 2}}, true, PipelineOpts{})
	if err != nil || len(res) != 2 {
		t.Fatalf("res=%v err=%v", res, err)
	}
	if !ps.sent[0].Read || ps.sent[0].Stmt != "try {p1} pkg/v1 get <{.k: 1}>;" {
		t.Fatalf("first envelope = %+v", ps.sent[0])
	}
}

func TestPipelineEmptyAndBadWindow(t *testing.T) {
	_, c := newPipelineClient(t, 1, nil, noRaw)
	if res, err := c.Pipeline(nil, PipelineOpts{}); err != nil || len(res) != 0 {
		t.Fatalf("empty: %v %v", res, err)
	}
	if _, err := c.Pipeline([]PipelineItem{{Stmt: "a;"}}, PipelineOpts{Window: -1}); err == nil {
		t.Fatal("a negative window must be refused")
	}
}
