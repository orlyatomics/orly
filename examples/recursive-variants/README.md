# Recursive sum types — storage & client marshaling

A WebSocket smoke test for **issue #115**: storing recursive sum-type values
and marshaling them back to a client. It stores a fixed value of each recursive
shape under a key, reads it back, and self-checks the JSON the client receives.

Recursive variants used to be package-internal — `<-` was a compile error and
returning one to a client threw at runtime. They are now fully first-class:
they store, read back, and marshal over the wire. This demo exercises that
**dynamic-var / WebSocket marshaling path**, which the C++ `lang_test` suite (a
native-read harness) can't reach — so it's the regression guard for it.

## The shapes

`recursive.orly` declares one type per recursive shape:

```
tree   is <| Leaf(int) | Branch(<{.l: tree, .r: tree}>) |>;        # self-ref in a record arm   (#103)
doc    is <| Null | Num(int) | Str(str) | Arr([doc]) |>;           # self-ref under a list       (#120)
mtree  is <| MLeaf(int) | MNode(forest) |>;                        # mutual recursion            (#116)
forest is <| FEmpty | FCons(<{.head: mtree, .tail: forest}>) |>;
ntree  is <| NLeaf(int) | NBranch(<| Un(ntree) | Nil |>) |>;       # nested-variant arm          (#125)
```

For each, a `put_X(.k)` method stores a fixed value with `new <[...]> <- ...`,
and a `get_X(.k)` method reads it back with `*<[...]>`. The driver calls them
over the WebSocket protocol and checks the result.

## Marshaling

A variant value marshals to JSON as `{ "Tag": <payload> }`; a tag-only arm as
`{ "Tag": {} }`; numbers come back as JSON floats. So the stored `tree`

```
tree.Branch(<{.l: tree.Leaf(1), .r: tree.Branch(<{.l: tree.Leaf(2), .r: tree.Leaf(3)}>)}>)
```

reads back as

```json
{"Branch": {"l": {"Leaf": 1.0}, "r": {"Branch": {"l": {"Leaf": 2.0}, "r": {"Leaf": 3.0}}}}}
```

## Running

Build the project first (`make debug` from the repo root), then:

```
./run.sh        # Python driver (demo.py)
./run-go.sh     # Go driver (demo.go) — needs the `go` toolchain
```

Each wrapper compiles `recursive.orly`, starts a fresh `orlyi`, runs the driver
against it over `ws://127.0.0.1:8082/`, and tears `orlyi` down. The Python and
Go drivers run the identical scenario and self-check; they're paired so they
can be diffed in spirit.

Expected output:

```
  [ok] tree  (self-reference in a record arm, #103)
  [ok] doc   (self-reference under a list, #120)
  [ok] mtree (mutual recursion, #116)
  [ok] ntree (nested-variant arm, #125)

All recursive-variant values round-tripped through storage and marshaled correctly.
```
