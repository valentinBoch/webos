#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
OUT="$(mktemp)"; trap 'rm -f "$OUT"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/tests/cwquality/test_resume_quality.c" "$ROOT/src/ui/resample_hq.c" -o "$OUT" -lm
"$OUT"
