#!/bin/bash
# Publish the TS stack to npm (#540): @orlyatomics/orly (the driver), then
# orly-mcp and orly-repl, which depend on it.
#
# In-repo, the dependents reference the driver as `"orly": "file:../ts"` (an
# npm alias: the key is the import specifier, the target's real name is
# @orlyatomics/orly). A file: dep cannot ship to the registry, so this script
# stages each package into a temp dir, rewrites the alias to
# `npm:@orlyatomics/orly@^<driver-version>`, builds, and publishes.
#
#   tools/npm_publish.sh --dry-run    # everything except the actual publish
#   tools/npm_publish.sh              # the real thing (needs `npm login` and
#                                     # membership in the orlyatomics org)
#
# Versions come from each package.json; bump them there (and commit) before
# publishing. npm refuses to republish an existing version, so a stale bump
# fails loudly rather than clobbering.
set -euo pipefail

cd "$(dirname "$0")/.."
REPO_ROOT="$PWD"
DRY_RUN=""
[ "${1:-}" = "--dry-run" ] && DRY_RUN="--dry-run"

DRIVER_VERSION="$(cd clients/ts && npm pkg get version | tr -d '"')"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

stage() { # <src-dir> <name>
  local src="$1" name="$2"
  cp -r "$REPO_ROOT/clients/$src" "$STAGE/$name"
  rm -rf "$STAGE/$name/node_modules" "$STAGE/$name/dist" \
         "$STAGE/$name/package-lock.json" "$STAGE/$name/smoke"
}

build_and_publish() { # <dir> [rewrite-alias]
  local dir="$STAGE/$1"
  if [ "${2:-}" = "rewrite-alias" ]; then
    if [ -n "$DRY_RUN" ]; then
      # The registry spec can't resolve until the driver is actually
      # published, so a dry run builds against the staged driver and just
      # reports what the real run would set.
      echo "   (dry run: would set dependencies.orly=npm:@orlyatomics/orly@^$DRIVER_VERSION)"
      (cd "$dir" && npm pkg set "dependencies.orly=file:../driver")
    else
      (cd "$dir" && npm pkg set "dependencies.orly=npm:@orlyatomics/orly@^$DRIVER_VERSION")
    fi
  fi
  (cd "$dir" && npm install --silent && npx tsc && npm publish --access public $DRY_RUN)
}

stage ts driver
stage mcp mcp
stage repl repl

echo "== publish @orlyatomics/orly@$DRIVER_VERSION"
build_and_publish driver

echo "== publish orly-mcp"
build_and_publish mcp rewrite-alias

echo "== publish orly-repl"
build_and_publish repl rewrite-alias

echo "done${DRY_RUN:+ (dry run)}"
