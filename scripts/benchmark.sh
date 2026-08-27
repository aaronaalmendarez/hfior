#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:-"$ROOT/benchmark-output/synthetic-smoke"}
make -C "$ROOT" all
"$ROOT/benchmarks/harness/run_once.sh" --synthetic-rate 8000 \
  --policy hfior-late-latch --out "$OUT" --duration 2 --frame-hz 240 \
  --base-work-us 1700 --integration-work-us 100 --ack-placement post-frame \
  --synthetic-timestamps publish --scenario public-synthetic-smoke \
  --rate-label 8000
printf 'Synthetic result written to %s\n' "$OUT"
