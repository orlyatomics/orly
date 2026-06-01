## web

Generic web-server utilities: `url_decode`, `daemonize`, `counter`,
`half_latch`. None of it Orly-specific -- it's the glue you'd reach
for in any C++ web binary.

Named `web/` to disambiguate from [`orly/server/`](../../orly/server/),
which is the actual `orlyi` daemon. The two used to both be called
`server/` (one at the root, one under `orly/`); see PR #39 for the
rename.

-----

README.md Copyright 2010-2026 Atomic Kismet Company

README.md is licensed under a Creative Commons Attribution-ShareAlike 4.0 International License.

You should have received a copy of the license along with this work. If not, see <http://creativecommons.org/licenses/by-sa/4.0/>.
