#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
OUT="$ROOT/build/test-output/integrity"
"$ROOT/benchmarks/harness/run_once.sh" --synthetic-rate 8000 \
  --button-every 29 --policy hfior-late --out "$OUT" --duration 0.3 \
  --frame-hz 240 --base-work-us 200 --record-trace --ack-placement post-frame \
  --synthetic-timestamps publish --scenario integrity-test --rate-label 8000
python3 - "$OUT/client/records.csv" <<'PY'
import csv, pathlib, sys
rows = list(csv.DictReader(pathlib.Path(sys.argv[1]).open()))
seq = [int(row["sequence"]) for row in rows]
assert seq and all(b > a for a, b in zip(seq, seq[1:]))
assert any(int(row["flags"]) & 2 for row in rows)
PY
printf 'ordering and button-state integrity: PASS\n'
