#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if (($#)); then
  make -C "$ROOT" "$@"
else
  make -C "$ROOT" all
fi
