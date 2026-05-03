#!/usr/bin/env bash
# scripts/extract-jsc-symbols.sh
#
# Pair every symbol in the Bun binary with its demangled form, then filter
# for JSC classes we care about. Output format (TSV, one row per symbol):
#
#   <dlsym_ready>\t<demangled>
#
# "dlsym_ready" means the form you pass to dlsym(RTLD_DEFAULT, ...):
#   macOS nm prints     "__ZN3JSC..."  (Mach-O leading _ + Itanium _Z)
#   dlsym expects       "_ZN3JSC..."   (just Itanium _Z)
#   Linux nm prints     "_ZN3JSC..."   (already dlsym-ready)
#
# So on macOS we strip exactly ONE leading underscore from "__Z..." symbols.
# "_Z..." passes through unchanged.
#
# If the binary is fully stripped, this prints nothing. That is itself a
# signal: use scripts/generate-candidates.sh as a fallback.

set -euo pipefail

BUN_BIN="${BUN_BIN:-$(command -v bun || true)}"
if [[ -z "$BUN_BIN" || ! -x "$BUN_BIN" ]]; then
  echo "bun not found on PATH and BUN_BIN not set" >&2
  exit 2
fi

# Classes we care about. Keep this list narrow so c++filt doesn't waste
# cycles on unrelated JSC symbols. Extend as the probe surface grows.
PATTERN='^JSC::(VM|SamplingProfiler|HeapProfiler|HeapSnapshotBuilder|Heap|ExecState|CallFrame|JSGlobalObject)(::|$)'

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Step 1: list every symbol name. nm output format varies across platforms,
# but the mangled symbol is always the last field.
nm -a "$BUN_BIN" 2>/dev/null \
  | awk 'NF >= 2 { print $NF }' \
  | sort -u > "$TMPDIR/nm_raw.txt"

# Step 2: demangle in a single batch (c++filt reads stdin line by line).
c++filt < "$TMPDIR/nm_raw.txt" > "$TMPDIR/demangled.txt" 2>/dev/null || true

# Step 3: pair up, filter by our JSC class pattern, normalize the mangled
# form for dlsym consumption. awk does the __Z -> _Z rewrite inline.
paste "$TMPDIR/nm_raw.txt" "$TMPDIR/demangled.txt" \
  | awk -F'\t' -v pat="$PATTERN" '
      $2 ~ pat {
        sym = $1
        # "__Z..." -> "_Z..." (strip Mach-O leading underscore only)
        if (substr(sym, 1, 3) == "__Z") { sym = substr(sym, 2) }
        print sym "\t" $2
      }' \
  | sort -u
