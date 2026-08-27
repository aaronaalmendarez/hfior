#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
make -C "$ROOT" all
"$ROOT/build/test-ring"
"$ROOT/build/test-policy"
"$ROOT/tests/process/run.sh"
"$ROOT/tests/integrity/run.sh"
"$ROOT/tests/overflow/run.sh"
