#!/usr/bin/env bash
# scripts/generate-candidates.sh
#
# Compile src/candidates/jsc_stub.cc to an object file, read its undefined
# references, and print the mangled names we'd want to probe for.
#
# This is the fallback when extract-jsc-symbols.sh returns nothing because
# the Bun binary is fully stripped.
#
# Output: one mangled symbol per line, each preceded by its demangled
# form as a `# comment` for human inspection.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/script/jsc_stub.cc"
OBJ=$(mktemp -t jsc_stub.XXXXXX.o)
trap "rm -f $OBJ" EXIT

# Pick a C++ compiler. clang++ preferred (matches what the real build uses);
# g++ is fine as a fallback — Itanium mangling is compiler-independent.
CXX="${CXX:-}"
if [[ -z "$CXX" ]]; then
  if command -v clang++ >/dev/null; then CXX=clang++
  elif command -v g++ >/dev/null; then CXX=g++
  else echo "no C++ compiler found (need clang++ or g++)" >&2; exit 2
  fi
fi

# Compile only; linking would fail because the refs are intentionally undefined.
"$CXX" -c -std=c++17 -fno-rtti -O0 "$SRC" -o "$OBJ" 2>/dev/null

# Undefined references show up with type "U" in nm output. The last column
# is the symbol name.
TMP_SYMS=$(mktemp)
trap "rm -f $OBJ $TMP_SYMS" EXIT

nm "$OBJ" 2>/dev/null \
  | awk '$1 == "U" || $2 == "U" { print $NF }' \
  | grep -E '^_{1,2}Z' \
  | sort -u > "$TMP_SYMS"

# Emit each mangled symbol preceded by a comment showing the demangled form.
# dlsym on macOS expects the name WITHOUT the Mach-O leading underscore but
# WITH the Itanium "_Z" prefix. Linux uses "_Z..." directly. So: if the
# symbol starts with "__Z" (Mach-O + Itanium), drop exactly one underscore;
# otherwise keep as-is.
while read -r sym; do
  case "$sym" in
    __Z*) sym_portable="${sym#_}" ;;
    *)    sym_portable="$sym" ;;
  esac
  demangled=$(echo "$sym_portable" | c++filt 2>/dev/null || echo "$sym_portable")
  printf "# %s\n%s\n" "$demangled" "$sym_portable"
done < "$TMP_SYMS"
