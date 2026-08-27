#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
OUT="$ROOT/build/test-output/process"
"$ROOT/benchmarks/harness/run_once.sh" --synthetic-rate 8000 \
  --policy hfior-late-latch --out "$OUT" --duration 0.25 --frame-hz 240 \
  --base-work-us 200 --integration-work-us 50 --ack-placement post-frame \
  --synthetic-timestamps publish --scenario process-test --rate-label 8000
printf 'separate-process transport: PASS\n'
