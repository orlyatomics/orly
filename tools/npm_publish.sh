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
# publishing. The script is idempotent: a package whose version is already on
# the registry is skipped, so rerunning after a partial failure picks up where
# it stopped. A freshly published package takes a few minutes to become
# visible, so before building orly-mcp/orly-repl the script waits (bounded,
# with progress output) until `npm view @orlyatomics/orly@<version> version`
# resolves.
#
# Every publish needs an npm 2FA one-time code (account 2FA is
# auth-and-writes), prompted for per package, so run this from an interactive
# terminal; it cannot work from a non-interactive shell or CI. The wait is
# tunable: WAIT_SECS (default 600) and WAIT_INTERVAL (default 15).
set -euo pipefail

cd "$(dirname "$0")/.."
REPO_ROOT="$PWD"
DRY_RUN=""
[ "${1:-}" = "--dry-run" ] && DRY_RUN="--dry-run"

DRIVER_VERSION="$(cd clients/ts && npm pkg get version | tr -d '"')"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

WAIT_SECS="${WAIT_SECS:-600}"
WAIT_INTERVAL="${WAIT_INTERVAL:-15}"

# Is <pkg>@<version> on the registry? (npm view prints the version if so and
# exits non-zero with E404 if not.)
published() { # <pkg> <version>
  [ "$(npm view "$1@$2" version 2>/dev/null || true)" = "$2" ]
}

# Block until <pkg>@<version> resolves, or give up after WAIT_SECS.
wait_for() { # <pkg> <version>
  local waited=0
  until published "$1" "$2"; do
    if [ "$waited" -ge "$WAIT_SECS" ]; then
      echo "!! $1@$2 still not visible after ${WAIT_SECS}s; rerun this script later" >&2
      exit 1
    fi
    echo "   waiting for $1@$2 to appear on the registry (${waited}s/${WAIT_SECS}s)..."
    sleep "$WAIT_INTERVAL"
    waited=$((waited + WAIT_INTERVAL))
  done
  echo "   $1@$2 is on the registry"
}

stage() { # <src-dir> <name>
  local src="$1" name="$2"
  cp -r "$REPO_ROOT/clients/$src" "$STAGE/$name"
  rm -rf "$STAGE/$name/node_modules" "$STAGE/$name/dist" \
         "$STAGE/$name/package-lock.json" "$STAGE/$name/smoke"
}

build_and_publish() { # <dir> <pkg> <version> [rewrite-alias]
  local dir="$STAGE/$1" pkg="$2" version="$3"
  if published "$pkg" "$version"; then
    echo "   $pkg@$version is already published; skipping"
    return 0
  fi
  if [ "${4:-}" = "rewrite-alias" ]; then
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

MCP_VERSION="$(cd clients/mcp && npm pkg get version | tr -d '"')"
REPL_VERSION="$(cd clients/repl && npm pkg get version | tr -d '"')"

stage ts driver
stage mcp mcp
stage repl repl

echo "== publish @orlyatomics/orly@$DRIVER_VERSION"
build_and_publish driver @orlyatomics/orly "$DRIVER_VERSION"

# The dependents install the driver from the registry, so it must resolve
# first. A dry run builds against the staged driver and has nothing to wait for.
if [ -z "$DRY_RUN" ]; then
  echo "== wait for @orlyatomics/orly@$DRIVER_VERSION"
  wait_for @orlyatomics/orly "$DRIVER_VERSION"
fi

echo "== publish orly-mcp@$MCP_VERSION"
build_and_publish mcp orly-mcp "$MCP_VERSION" rewrite-alias

echo "== publish orly-repl@$REPL_VERSION"
build_and_publish repl orly-repl "$REPL_VERSION" rewrite-alias

echo "done${DRY_RUN:+ (dry run)}"
