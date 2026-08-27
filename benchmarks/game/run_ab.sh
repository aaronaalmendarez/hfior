#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${1:-"$ROOT/benchmark-output/game/physical-heavy"}
REPS=${2:-5}

if ! [[ $REPS =~ ^[1-9][0-9]*$ ]]; then
  printf 'repetitions must be a positive integer\n' >&2
  exit 2
fi
if compgen -G "$OUT/*.summary.json" >/dev/null; then
  printf 'output directory already contains summaries: %s\n' "$OUT" >&2
  exit 2
fi

make -C "$ROOT" game-benchmark
mkdir -p "$OUT"

for ((rep = 1; rep <= REPS; ++rep)); do
  if ((rep % 2 == 1)); then
    modes=(eager hfior)
  else
    modes=(hfior eager)
  fi
  for mode in "${modes[@]}"; do
    printf 'Starting mode=%s repetition=%d/%d\n' "$mode" "$rep" "$REPS"
    SDL_VIDEODRIVER=wayland "$ROOT/build/hfior-game" \
      --mode "$mode" \
      --warmup 2 \
      --seconds 8 \
      --objects 65536 \
      --reaction-objects 8192 \
      --draw-repeats 128 \
      --requested-rate 8000 \
      --output "$OUT/$mode-$rep.csv"
  done
done

python3 "$ROOT/benchmarks/game/analyze.py" \
  "$OUT"/*.summary.json >"$OUT/aggregate.json"
printf 'Aggregate written to %s/aggregate.json\n' "$OUT"
