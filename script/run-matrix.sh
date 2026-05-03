#!/usr/bin/env bash
# scripts/run-matrix.sh
#
# Download multiple Bun versions into .bun-versions/<ver>/bun and run the
# static + symbol-extraction probes against each. Also runs the functional
# probe in each Bun version (requires the addon to already be built).
#
# Outputs per-version JSON and TSV into results/.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSIONS=(
  "1.1.0"
  "1.1.20"
  "1.1.38"
  "1.2.0"
  "1.2.10"
  "1.3.0"
  "1.3.5"
  "canary"
)

OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64) BUN_ARCH="x64" ;;
  arm64|aarch64) BUN_ARCH="aarch64" ;;
  *) echo "unsupported arch: $ARCH" >&2; exit 2 ;;
esac

mkdir -p .bun-versions results script/candidates

# Build addon once (N-API is version-stable across Bun versions so one build
# works everywhere).
if [[ ! -f script/probe_lazy.node ]]; then
  echo "==> building addon"
  make
fi

# Try to download a specific Bun release into a local dir. Returns 0 on success.
download_bun() {
  local ver="$1" dir="$2"
  local tag="bun-v$ver"
  [[ "$ver" == "canary" ]] && tag="canary"
  local url="https://github.com/oven-sh/bun/releases/download/$tag/bun-$OS-$BUN_ARCH.zip"
  mkdir -p "$dir"
  (
    cd "$dir"
    curl -fsSL -o bun.zip "$url" || return 1
    unzip -qo bun.zip
    mv "bun-$OS-$BUN_ARCH/bun" ./bun
    rm -rf bun.zip "bun-$OS-$BUN_ARCH"
    chmod +x bun
  )
}

echo "version	all_syms	exported	jsc_strings	extracted_syms	verdict" > results/matrix.tsv

for ver in "${VERSIONS[@]}"; do
  dir=".bun-versions/$ver"
  bin="$dir/bun"

  if [[ ! -x "$bin" ]]; then
    echo "==> fetching bun $ver"
    if ! download_bun "$ver" "$dir"; then
      echo "!! failed to fetch $ver, skipping"
      continue
    fi
  fi

  echo "==> probing bun $ver"

  BUN_BIN="$bin" bash script/inspect-bun.sh > "results/$ver-static.json" 2>&1 || true
  BUN_BIN="$bin" bash script/extract-jsc-symbols.sh > "results/$ver-symbols.tsv" 2>&1 || true

  # Point the functional probe at the version-specific extracted symbols
  # alongside the base candidates.
  cp -f "results/$ver-symbols.tsv" script/candidates/extracted.txt || true
  "$bin" script/probe.js > "results/$ver-functional.json" 2>&1 || true

  # Summarize one row into the matrix TSV
  if command -v jq >/dev/null 2>&1; then
    all=$(jq -r '.symbol_counts.all_symbols_nm_a // 0' "results/$ver-static.json" 2>/dev/null || echo 0)
    exp=$(jq -r '.symbol_counts.exported_symbols // 0' "results/$ver-static.json" 2>/dev/null || echo 0)
    jsc=$(jq -r '.text_references.jsc_namespace // 0' "results/$ver-static.json" 2>/dev/null || echo 0)
    ext=$(wc -l < "results/$ver-symbols.tsv" | tr -d ' ')
    ver_dict=$(jq -r '.verdict // "unknown"' "results/$ver-functional.json" 2>/dev/null || echo unknown)
    echo "$ver	$all	$exp	$jsc	$ext	$ver_dict" >> results/matrix.tsv
  fi
done

echo
echo "==> matrix complete"
echo
if [[ -f results/matrix.tsv ]]; then
  column -t -s$'\t' results/matrix.tsv
fi
