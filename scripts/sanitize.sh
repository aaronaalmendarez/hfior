#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SAN_DIR="$ROOT/build-sanitize"
make -C "$ROOT" BUILD_DIR="$SAN_DIR" \
  CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fno-omit-frame-pointer -fsanitize=address,undefined' \
  LDFLAGS='-fsanitize=address,undefined' all
"$SAN_DIR/test-ring"
"$SAN_DIR/test-policy"
HFIOR_BIN_DIR="$SAN_DIR" "$ROOT/tests/process/run.sh"
