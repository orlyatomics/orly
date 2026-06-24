#!/bin/bash
# Type-check app.ts and bundle it (with the orly TS client) into app.js for
# the browser. esbuild leaves the SDK's Node-only `import("ws")` external --
# in a browser the global WebSocket is used and that import never runs.
set -e
cd "$(dirname "$0")"
SDK="$(cd ../../.. && pwd)/clients/ts/src/index.ts"
npm install --silent
echo "[1/2] type-check (tsc)"
npx tsc --noEmit
echo "[2/2] bundle (esbuild) -> app.js"
npx esbuild app.ts --bundle --format=esm --platform=browser \
  --external:ws --alias:orly="$SDK" --outfile=app.js
echo "built app.js"
