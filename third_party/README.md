## third_party

Third party libraries that ship in this repo as source-tree snapshots. Each subdirectory has a `VENDORED.md` describing what version is pinned, when it was last bumped, why we vendor it rather than pulling from a system package, and how to update.

| Library | Version | Used by |
| --- | --- | --- |
| [`linenoise`](linenoise/VENDORED.md) | ~2013 snapshot (antirez/linenoise) | `base/repl`, `jhm/status_line` |
| [`mongoose`](mongoose/VENDORED.md) | 3.0 (last MIT release) | `orly/spa/spa.cc` |
| [`websocketpp`](websocketpp/VENDORED.md) | 0.8.2 | `orly/server/ws.cc` |

When bumping any of these, update the corresponding `VENDORED.md`. When adding a new vendored dep, write one before opening the PR.

-----

README.md Copyright 2010-2026 Atomic Kismet Company

README.md is licensed under a Creative Commons Attribution-ShareAlike 4.0 International License.

You should have received a copy of the license along with this work. If not, see <http://creativecommons.org/licenses/by-sa/4.0/>.
