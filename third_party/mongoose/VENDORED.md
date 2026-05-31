# mongoose

Small embeddable C HTTP server. Used by `orly/spa/spa.cc` for the HTTP front-end of the `spa` (single-process app) server.

## Vendored version

- **Version:** 3.0 (`#define MONGOOSE_VERSION "3.0"` in `mongoose.c`)
- **Upstream (then):** https://github.com/valenok/mongoose (original maintainer Sergey Lyubka)
- **Upstream (now):** https://github.com/cesanta/mongoose (forked + relicensed)
- **Vendored:** pre-2014, exact date unknown (predates the Apache foundation migration in this repo's history)
- **License at vendoring time:** MIT (`LICENSE`, `mongoose.c` header)

## Why vendored rather than system package

Mongoose 3.0 is *only* available as this vendored snapshot. The upstream project was forked to cesanta/mongoose and relicensed to GPLv2 / commercial dual-license starting around v3.1. None of the major distros package the MIT-licensed 3.x branch. Even if they did, upstream's API changed dramatically in 5.x and again in 7.x, so a system package would not satisfy `orly/spa/spa.cc`'s expectations of the v3-era callback model (`mg_event`, `mg_request_info`, `OnEvent`).

## Local modifications

None to the source. The build adds several `-Wno-*` suppressions in `root.jhm`'s `gcc` section (because gcc 13 flags things gcc 4.9 didn't — `-Wcast-function-type`, `-Wmaybe-uninitialized`, etc.) but the C code is verbatim 3.0.

## Bump notes

This snapshot is essentially frozen. Bumping options, none of them attractive:

1. **Stay on 3.0** (current): compiles, links, works. License compatible with this repo. Aging gracefully behind warning suppressions.
2. **Bump to a newer cesanta/mongoose**: triggers the GPLv2/commercial license question, and requires rewriting `orly/spa/spa.cc` against the v7.x API (which is `mg_http_serve_dir`, `MG_EV_HTTP_MSG`, etc. — completely different model). Effectively rewriting the SPA HTTP layer.
3. **Replace with a different lib**: `boost::beast` (boost is already a dep) or `cpphttplib` would let us drop this directory entirely. Same scope of work as option 2.

Recommendation: leave on 3.0 until the SPA HTTP layer needs other changes anyway, at which point options 2 or 3 become incremental.

## To update (if option 1)

There is no newer 3.x to update to — the MIT branch ended at 3.x. This entry exists for completeness.
