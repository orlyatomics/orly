# Orly runtime image (#530): a mem-sim `orlyi` with the example packages
# pre-compiled, plus `orlyc` and the source headers so user `.orly` packages
# compile INSIDE the container (orlyc shells out to g++ with -I$ORLY_SRC_ROOT).
#
#   docker build -t ghcr.io/orlyatomics/orly .
#   docker run --rm -p 8082:8082 ghcr.io/orlyatomics/orly
#
# The WebSocket + JSON protocol is then on ws://127.0.0.1:8082/ -- point any
# client driver (clients/{python,go,ts}) or the MCP server (clients/mcp) at it.
#
# Interactive orlyscript (#538): `docker run -it --rm ghcr.io/orlyatomics/orly repl`
# starts orlyi inside the container and drops straight into orly-repl.

# ---- build stage -----------------------------------------------------------

FROM ubuntu:24.04 AS build

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential gcc g++ uuid-dev libgmp-dev libaio-dev libsnappy-dev \
      libreadline-dev libboost-system-dev zlib1g-dev bison flex python3 \
      nodejs npm \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/orly
COPY . .

# Bootstrap jhm + nycr, then build just the two production binaries this
# image ships (jhm emits to /src/out_orly). `make version` degrades to
# "unknown" without git -- fine for an image whose tag carries the version.
ARG JHM_WORKER_COUNT=4
RUN make bootstrap version \
    && PATH="/src/orly/tools:$PATH" \
       jhm -c release --worker-count "$JHM_WORKER_COUNT" \
           orly/server/orlyi orly/orlyc

# Pre-compile the example packages with the just-built orlyc (each orlyc run
# stands up and tears down its embedded mem-sim server, so this also smokes
# the binaries during the image build). `sample` is the trivial write/read
# package the MCP smoke uses; `graph` is the agent knowledge-graph schema;
# `market` is the prediction market.
RUN mkdir -p /opt/orly-packages /tmp/pkgout && touch /opt/orly-packages/__orly__ \
    && cd /tmp/pkgout \
    && /src/out_orly/release/orly/orlyc -o /tmp/pkgout /src/orly/clients/mcp/smoke/sample.orly \
    && /src/out_orly/release/orly/orlyc -o /tmp/pkgout /src/orly/examples/agent-swarm/graph.orly \
    && /src/out_orly/release/orly/orlyc -o /tmp/pkgout /src/orly/examples/prediction-market/market.orly \
    && cp /tmp/pkgout/*.so /opt/orly-packages/

# Build the TS driver + orly-repl (#538); they reach the runtime stage inside
# the /src/orly -> /opt/orly/src copy, so prune dev deps (typescript et al.)
# down to what `node dist/index.js` needs. Order matters: the repl's npm
# install copies clients/ts (per its `files`), so ts builds first.
RUN cd /src/orly/clients/ts && npm install --silent && npx tsc \
    && cd /src/orly/clients/repl && npm install --silent && npx tsc \
    && npm prune --omit=dev \
    && cd /src/orly/clients/ts && npm prune --omit=dev

# ---- runtime stage ---------------------------------------------------------

FROM ubuntu:24.04

# Link the ghcr package to this repo (package page shows the source/README).
LABEL org.opencontainers.image.source="https://github.com/orlyatomics/orly" \
      org.opencontainers.image.description="Orly database: mem-sim orlyi + orlyc + example packages" \
      org.opencontainers.image.licenses="Apache-2.0"

# Runtime .so set verified via ldd on orlyi/orlyc, plus what in-container
# package compiles need: g++ (orlyc execs `g++ -I$ORLY_SRC_ROOT ...`) and
# uuid-dev (generated code includes base/uuid.h -> <uuid/uuid.h>; the lib
# alone is not enough -- verified by compiling a package in the container).
# nodejs (no npm) runs the pre-built orly-repl.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      g++ uuid-dev libgmp10 libgmpxx4ldbl libaio1t64 libboost-system1.83.0 \
      libreadline8t64 libsnappy1v5 zlib1g nodejs \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/out_orly/release/orly/server/orlyi /usr/local/bin/orlyi
COPY --from=build /src/out_orly/release/orly/orlyc /usr/local/bin/orlyc
# The source tree = the header root orlyc-generated C++ compiles against.
COPY --from=build /src/orly /opt/orly/src
COPY --from=build /opt/orly-packages /var/lib/orly/packages
COPY docker/entrypoint.sh /usr/local/bin/orly-entrypoint
# Shim so `docker exec -it <ctr> orly-repl ...` works against a running
# default-entrypoint container, not just the `repl` entrypoint mode.
RUN printf '#!/bin/sh\nexec node /opt/orly/src/clients/repl/dist/index.js "$@"\n' \
      > /usr/local/bin/orly-repl \
    && chmod +x /usr/local/bin/orly-repl

# Trailing slash matters: the compiler concatenates paths onto this directly.
ENV ORLY_SRC_ROOT=/opt/orly/src/

EXPOSE 8082
ENTRYPOINT ["/usr/local/bin/orly-entrypoint"]
