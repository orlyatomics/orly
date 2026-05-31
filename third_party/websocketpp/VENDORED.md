# websocketpp

Header-only C++ WebSocket library. Used by `orly/server/ws.cc` for the WebSocket protocol surface that `orlyi` exposes on port 8082.

## Vendored version

- **Version:** 0.8.2
- **Upstream:** https://github.com/zaphoyd/websocketpp
- **Tarball:** https://github.com/zaphoyd/websocketpp/archive/refs/tags/0.8.2.tar.gz
- **Vendored:** 2026-05-29 (PR #5), bumped from 0.3.0 (2013)
- **License:** BSD 3-Clause (`COPYING`)

## Why vendored rather than system package

Not packaged in Ubuntu / Debian / Fedora repositories. Upstream's recommended distribution model is exactly this — header-only source drop. Some distros ship `libwebsocketpp-dev` but the packaging is inconsistent across versions and lags upstream by years.

## Local modifications

None. The vendored tree is the upstream 0.8.2 tarball contents, verbatim.

## Bump notes

- The 0.3.0 → 0.8.2 bump in #5 was forced: 0.3.0 used `boost::asio::strand(io_service)` which no longer exists in boost ≥ 1.74. `Ubuntu 24.04` ships boost 1.83, so 0.3.0 wouldn't compile at all.
- 0.8.2 is the last header-only release before 0.9.0 introduced larger restructuring. No reason to bump further unless a CVE shows up.
- `orly/server/ws.cc` was adjusted alongside the bump (#8) because `set_socket_init_handler`'s callback timing changed between 0.3.0 and 0.8.2.

## To update

```sh
cd /tmp
wget https://github.com/zaphoyd/websocketpp/archive/refs/tags/<NEW_TAG>.tar.gz
tar -xzf <NEW_TAG>.tar.gz
rm -rf <REPO_ROOT>/third_party/websocketpp
mv websocketpp-<NEW_TAG> <REPO_ROOT>/third_party/websocketpp
# verify `make debug` and `orly/server/ws.test` still pass
```
