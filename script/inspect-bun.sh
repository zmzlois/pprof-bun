#!/usr/bin/env bash
# scripts/inspect-bun.sh
#
# Probe A + B: static inspection of a Bun binary.
# Reports symbol table sizes, JSC text references, linked libraries,
# and any Bun/N-API prefixed exports.
#
# Usage:
#   bash scripts/inspect-bun.sh                 # probes `which bun`
#   BUN_BIN=/path/to/bun bash scripts/inspect-bun.sh
#
# Output is a single JSON object on stdout.

set -euo pipefail

BUN_BIN="${BUN_BIN:-$(command -v bun || true)}"
if [[ -z "$BUN_BIN" || ! -x "$BUN_BIN" ]]; then
  echo '{"error":"bun not found on PATH and BUN_BIN not set"}' >&2
  exit 2
fi

OS="$(uname -s)"
ARCH="$(uname -m)"

# Safe counter that always prints a number on stdout, even when pipeline fails.
count_lines() { wc -l | tr -d ' '; }

if [[ "$OS" == "Darwin" ]]; then
  NM_ALL=$( (nm -a "$BUN_BIN" 2>/dev/null || true) | count_lines)
  NM_EXPORTED=$( (nm -gU "$BUN_BIN" 2>/dev/null || true) | count_lines)
  BUN_PREFIX=$( (nm -a "$BUN_BIN" 2>/dev/null | c++filt 2>/dev/null | grep -E '^[0-9a-f]+ [A-Za-z] (Bun|napi_|node_api_|node_module_register)' || true) | count_lines)
  LINKED=$(otool -L "$BUN_BIN" 2>/dev/null | tail -n +2 | awk '{print $1}' | paste -sd, - || echo "")
  BINARY_SIZE=$(stat -f%z "$BUN_BIN" 2>/dev/null || echo 0)
elif [[ "$OS" == "Linux" ]]; then
  NM_ALL=$( (nm -a "$BUN_BIN" 2>/dev/null || true) | count_lines)
  NM_EXPORTED=$( (nm -D --defined-only "$BUN_BIN" 2>/dev/null || true) | count_lines)
  BUN_PREFIX=$( (nm -a "$BUN_BIN" 2>/dev/null | c++filt 2>/dev/null | grep -E ' [A-Za-z] (Bun|napi_|node_api_|node_module_register)' || true) | count_lines)
  LINKED=$(ldd "$BUN_BIN" 2>/dev/null | awk '{print $1}' | paste -sd, - || echo "")
  BINARY_SIZE=$(stat -c%s "$BUN_BIN" 2>/dev/null || echo 0)
else
  echo "{\"error\":\"unsupported OS: $OS\"}" >&2
  exit 2
fi

JSC_STRINGS=$( (strings "$BUN_BIN" 2>/dev/null | grep -c 'JSC::' || true) )
SAMPLER_STRINGS=$( (strings "$BUN_BIN" 2>/dev/null | grep -c 'SamplingProfiler' || true) )
HEAPPROF_STRINGS=$( (strings "$BUN_BIN" 2>/dev/null | grep -c 'HeapProfiler' || true) )

VERSION=$("$BUN_BIN" --version 2>/dev/null | head -1 || echo "unknown")

# Interpret the result into a classification.
# - red:    no JSC code present at all (should never happen with Bun)
# - dark:   JSC code present, zero symbols in nm -> fully stripped, need upstream PR
# - yellow: symbols present locally but not exported; may or may not be dlsym-reachable
# - green:  meaningful exported surface
CLASS="unknown"
if [[ "$JSC_STRINGS" -eq 0 ]]; then
  CLASS="red"
elif [[ "$NM_ALL" -lt 10 ]]; then
  CLASS="dark"
elif [[ "$NM_EXPORTED" -lt 10 && "$NM_ALL" -ge 10 ]]; then
  CLASS="yellow"
else
  CLASS="green"
fi

cat <<JSON
{
  "bun_binary": "$BUN_BIN",
  "bun_version": "$VERSION",
  "platform": "$OS",
  "arch": "$ARCH",
  "binary_size_bytes": $BINARY_SIZE,
  "symbol_counts": {
    "all_symbols_nm_a": $NM_ALL,
    "exported_symbols": $NM_EXPORTED,
    "bun_or_napi_prefixed": $BUN_PREFIX
  },
  "text_references": {
    "jsc_namespace": $JSC_STRINGS,
    "sampling_profiler": $SAMPLER_STRINGS,
    "heap_profiler": $HEAPPROF_STRINGS
  },
  "linked_libraries": "$LINKED",
  "classification": "$CLASS"
}
JSON
