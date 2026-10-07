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
	"strconv"
	"strings"
	"testing"
)

func TestKeywordFields(t *testing.T) {
	for _, name := range []string{"id", "to", "from", "start", "after", "if", "true",
		"int", "and", "keys", "try", "safe", "list_packages"} {
		t.Run(name, func(t *testing.T) {
			got, err := Lit(map[string]any{name: 1})
			if err != nil || got != "<{."+name+": 1}>" {
				t.Fatalf("Lit: %q, %v", got, err)
			}
		})
	}
	for _, tc := range []struct {
		value map[string]any
		want  string
	}{
		{map[string]any{"to": map[string]any{"id": 7}}, "<{.to: <{.id: 7}>}>"},
		{map[string]any{}, "<{}>"},
		{map[string]any{"_id2": true}, "<{._id2: true}>"},
	} {
		got, err := Lit(tc.value)
		if err != nil || got != tc.want {
			t.Fatalf("Lit: %q, %v; want %q", got, err, tc.want)
		}
	}
}

func TestInvalidFields(t *testing.T) {
	for _, name := range []string{"", "1st", "first-name", "a.b", "é", "id\n"} {
		t.Run(strconv.Quote(name), func(t *testing.T) {
			_, err := Lit(map[string]any{name: 1})
			if err == nil || !strings.Contains(err.Error(), strconv.Quote(name)) || !strings.Contains(err.Error(), "rename") {
				t.Fatalf("expected field name and rename suggestion, got %v", err)
			}
			_, err = Lit(map[string]any{"to": map[string]any{name: 1}})
			if err == nil || !strings.Contains(err.Error(), "invalid record field name") {
				t.Fatalf("expected nested field error, got %v", err)
			}
		})
	}
}
