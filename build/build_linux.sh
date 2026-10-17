#!/bin/sh
# Builds the Linux image tool (hpfstool) from the shared engine, for tests.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT="${1:-/tmp/hpfsnt-linux}"
mkdir -p "$OUT"
gcc -O1 -g -Wall -Wno-unused-parameter ${EXTRACFLAGS} -I"$ROOT/src" -I"$ROOT/tools" -I"$ROOT/fmt" -I"$ROOT/chk" \
    -o "$OUT/hpfstool" "$ROOT/tools/hpfstool.c" "$ROOT/tools/hpfsimg.c" "$ROOT/src/hpfssup.c" \
    "$ROOT/src/hpfsalc.c" "$ROOT/src/hpfsdnd.c" "$ROOT/src/hpfsfil.c" "$ROOT/fmt/hpfsfmtc.c" "$ROOT/chk/hpfschkc.c"
echo "Built: $OUT/hpfstool"
