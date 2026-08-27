#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
OUT="$ROOT/build/test-output/overflow"
set +e
"$ROOT/benchmarks/harness/run_once.sh" --synthetic-rate 8000 \
  --policy hfior-late --out "$OUT" --duration 0.5 --frame-hz 240 \
  --base-work-us 200 --stall-after-ms 100 --stall-ms 150 --capacity 64 \
  --ack-placement post-frame --synthetic-timestamps publish \
  --scenario explicit-overflow-test --rate-label 8000
rc=$?
set -e
(( rc != 0 ))
python3 - "$OUT" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
summary = json.loads((p / "client/summary.json").read_text())
bridge = dict(line.split("=", 1) for line in (p / "bridge.env").read_text().splitlines() if "=" in line)
assert int(bridge["ring_drops"]) > 0
assert int(summary["sequence_gaps"]) > 0
assert int(summary["duplicate_or_reordered"]) == 0
PY
printf 'explicit detectable overflow: PASS\n'
