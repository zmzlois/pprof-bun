#!/usr/bin/env bash
# scripts/probe-all.sh
#
# Run the whole probe pipeline against one Bun binary:
#   1. static inspection           -> results/latest-static.json
#   2. extract JSC symbols         -> candidates/extracted.txt
#   3. generate candidate symbols  -> candidates/generated.txt
#   4. build addon if needed
#   5. functional probe            -> results/latest-functional.json
#
# Environment:
#   BUN_BIN  override which Bun to probe; defaults to `which bun`.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p results script/candidates

BUN_BIN="${BUN_BIN:-$(command -v bun || true)}"
if [[ -z "$BUN_BIN" ]]; then
  echo "bun not found; set BUN_BIN" >&2
  exit 2
fi
echo "==> target: $BUN_BIN"

echo "==> [1/5] static inspection"
BUN_BIN="$BUN_BIN" bash script/inspect-bun.sh | tee results/latest-static.json
echo

echo "==> [2/5] extract JSC symbols from binary"
BUN_BIN="$BUN_BIN" bash script/extract-jsc-symbols.sh > script/candidates/extracted.txt || true
lines=$(wc -l < script/candidates/extracted.txt | tr -d ' ')
echo "    $lines JSC symbol(s) found in binary"
echo

echo "==> [3/5] generate candidates from forward-decls"
bash script/generate-candidates.sh > script/candidates/generated.txt 2>/dev/null || true
lines=$(grep -cv '^#' script/candidates/generated.txt 2>/dev/null || echo 0)
echo "    $lines candidate(s) generated"
echo

echo "==> [4/5] build addon (if needed)"
if [[ ! -f script/probe_lazy.node ]]; then
  make
fi

echo "==> [5/5] functional probe in $(basename "$BUN_BIN")"
"$BUN_BIN" script/probe.js | tee results/latest-functional.json
