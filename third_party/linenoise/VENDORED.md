# linenoise

Single-file C readline replacement. Used by `base/repl.h` / `base/repl.cc` for the interactive REPL in `orly_client`, and by `jhm/status_line.h` for terminal-aware status output.

## Vendored version

- **Version:** unversioned (upstream does not tag releases)
- **Snapshot date:** approximately 2013, based on the `Copyright (c) 2010-2013` line in `linenoise.c`. Best guess: the snapshot was taken when this codebase was first written.
- **Upstream:** https://github.com/antirez/linenoise
- **License:** BSD 2-Clause (`README.markdown`)
- **Size at vendoring:** 1156 lines across `linenoise.c` + `linenoise.h`

## Why vendored rather than system package

Linenoise's upstream model is "drop the two files in your project." It's never been packaged for distros and the maintainer (antirez, of Redis) treats it as a sample / reference rather than a library. Vendoring is the recommended distribution.

## Local modifications

None known. The vendored files appear to be verbatim antirez/linenoise from circa 2013.

## Bump notes

Linenoise's API has been remarkably stable — function signatures haven't changed in years. A drop-in replacement from current upstream HEAD should work without touching `base/repl.cc` or `jhm/status_line.h`. The upside is mostly hygiene (we'd pick up bug fixes from the intervening ~12 years); there's no functional pressure to bump.

## To update

Upstream doesn't tag releases. Pin to a commit:

```sh
cd /tmp
git clone --depth=1 https://github.com/antirez/linenoise.git
cd <REPO_ROOT>/third_party/linenoise
# Replace only the source files; keep README and Makefile in case ours have local tweaks
cp /tmp/linenoise/linenoise.c .
cp /tmp/linenoise/linenoise.h .
# verify `orly_client` interactive shell still works and jhm status line still renders
```

Then record the new upstream commit SHA in this file.
