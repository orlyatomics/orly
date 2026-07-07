#!/bin/bash
# Default container entrypoint (#530): a solo mem-sim orlyi with the baked
# example packages (sample, graph, market) installable out of the box, WS on
# 8082. Extra `docker run ... <flags>` are appended verbatim, so any orlyi
# flag can be overridden or added. Sizing knobs via env for the common case.
set -e

exec orlyi \
  --mem_sim \
  --mem_sim_mb="${ORLY_MEM_SIM_MB:-256}" \
  --mem_sim_slow_mb="${ORLY_MEM_SIM_SLOW_MB:-64}" \
  --create=true \
  --instance_name="${ORLY_INSTANCE_NAME:-orly}" \
  --starting_state=SOLO \
  --port_number=8080 \
  --slave_port_number=8081 \
  --ws_port_number=8082 \
  --reporting_port_number=8083 \
  --connection_backlog=32 \
  --package_dir=/var/lib/orly/packages \
  --max_parallel_frames 4000 \
  --page_cache_size 256 \
  --block_cache_size 64 \
  --le --log_info \
  "$@"
