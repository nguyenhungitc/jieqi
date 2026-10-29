#!/usr/bin/env bash
# =============================================================================
# build-all.sh - Build the Jieqi engine for several x86-64 architectures on Linux
#
# Place this file in the repository root (next to src/), then run:
#   chmod +x build-all.sh
#   ./build-all.sh                         # build every architecture
#   ./build-all.sh avx2 bmi2               # build only some (short names allowed)
#   COMP=clang JOBS=8 ./build-all.sh       # change compiler / parallel jobs
#   TARGET=profile-build ./build-all.sh    # PGO build
#
# Output: dist/jieqi-linux-<arch>, one log per build in dist/logs/
# =============================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$ROOT/src"
OUT="${OUT:-$ROOT/dist}"
LOGDIR="$OUT/logs"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
TARGET="${TARGET:-build}"      # build | profile-build
COMP="${COMP:-}"               # gcc | clang | ... (empty = Makefile default)
NAME="${NAME:-jieqi}"          # binary name produced by the Makefile (src/jieqi)

DEFAULT_ARCHS=(
  x86-64-sse41-popcnt
  x86-64-avx2
  x86-64-bmi2
  x86-64-avxvnni
  x86-64-avx512
  x86-64-vnni512
  x86-64-avx512icl
)

# ----- Colored output (only when writing to a terminal) ----------------------
if [[ -t 1 ]]; then
  R=$'\e[31m'; G=$'\e[32m'; Y=$'\e[33m'; B=$'\e[1;34m'; N=$'\e[0m'
else
  R=; G=; Y=; B=; N=
fi
info() { echo "${B}==>${N} $*"; }
warn() { echo "${Y}[!]${N} $*" >&2; }
die()  { echo "${R}[x]${N} $*" >&2; exit 1; }

# ----- Architecture list (short names allowed: avx2 -> x86-64-avx2) ----------
if (( $# > 0 )); then
  ARCHS=()
  for a in "$@"; do
    [[ "$a" == x86-64-* ]] || a="x86-64-$a"
    ARCHS+=("$a")
  done
else
  ARCHS=("${DEFAULT_ARCHS[@]}")
fi

# ----- Environment checks ----------------------------------------------------
[[ -f "$SRC/Makefile" ]] || die "$SRC/Makefile not found - put this script in the repository root."
command -v make >/dev/null || die "make is not installed."

# x86-64-avx512icl needs the patched src/Makefile shipped with this script
if [[ " ${ARCHS[*]} " == *" x86-64-avx512icl "* ]] && ! grep -q 'avx512icl' "$SRC/Makefile"; then
  warn "src/Makefile does not know x86-64-avx512icl - that build will fail."
fi

mkdir -p "$OUT" "$LOGDIR"

info "Target : $TARGET"
info "Jobs   : $JOBS"
info "Comp   : ${COMP:-default}"
info "Archs  : ${ARCHS[*]}"
echo

# ----- Build loop ------------------------------------------------------------
declare -A RESULT
START_ALL=$SECONDS

for arch in "${ARCHS[@]}"; do
  short="${arch#x86-64-}"
  bin="$OUT/${NAME}-linux-${short}"
  log="$LOGDIR/${short}.log"

  info "Building ${arch} ..."
  t0=$SECONDS

  # Clean first so object files from another architecture are not reused
  make -C "$SRC" clean >/dev/null 2>&1

  make_args=( -C "$SRC" -j"$JOBS" "$TARGET" ARCH="$arch" EXE="$NAME" )
  [[ -n "$COMP" ]] && make_args+=( COMP="$COMP" )

  if make "${make_args[@]}" >"$log" 2>&1 && [[ -f "$SRC/$NAME" ]]; then
    install -m 755 "$SRC/$NAME" "$bin"
    command -v strip >/dev/null && strip "$bin" 2>/dev/null
    RESULT[$arch]="OK"
    echo "    ${G}OK${N}   -> ${bin#$ROOT/}  ($((SECONDS - t0))s)"
  else
    RESULT[$arch]="FAIL"
    echo "    ${R}FAIL${N} -> see ${log#$ROOT/}"
    tail -n 5 "$log" | sed 's/^/      /'
  fi
done

# Leave the source tree clean
make -C "$SRC" clean >/dev/null 2>&1

# ----- Checksums -------------------------------------------------------------
if compgen -G "$OUT/${NAME}-linux-*" >/dev/null; then
  ( cd "$OUT" && sha256sum "${NAME}"-linux-* > SHA256SUMS )
fi

# ----- Summary ---------------------------------------------------------------
echo
info "Summary ($((SECONDS - START_ALL))s):"
fail=0
for arch in "${ARCHS[@]}"; do
  if [[ "${RESULT[$arch]}" == "OK" ]]; then
    printf "    %-22s %s\n" "$arch" "${G}OK${N}"
  else
    printf "    %-22s %s\n" "$arch" "${R}FAIL${N}"
    fail=$((fail + 1))
  fi
done

# Suggest the best build for the CPU this script runs on
flags=$(grep -m1 '^flags' /proc/cpuinfo 2>/dev/null || true)
has() { [[ " $flags " == *" $1 "* ]]; }
best="x86-64-sse41-popcnt"
has avx2 && best="x86-64-avx2"
has avx2 && has bmi2 && best="x86-64-bmi2"
has avx_vnni && best="x86-64-avxvnni"
has avx512f && has avx512bw && best="x86-64-avx512"
has avx512_vnni && best="x86-64-vnni512"
has avx512_vnni && has avx512vbmi && has avx512_vbmi2 && has avx512_bitalg && best="x86-64-avx512icl"
echo
info "Best match for this CPU: ${best}"

exit $(( fail > 0 ))