#!/usr/bin/env bash
# Reproduction script for the issues described in BUGS.md.
#
# Build four binaries from trees prepared with apply_test_harness.py:
#   original tree (9b963f7) and tree with the fix series applied,
#   each in release and in debug+sanitizer mode:
#
#     make -j build ARCH=x86-64-sse41-popcnt
#     make -j build ARCH=x86-64-sse41-popcnt debug=yes sanitize="address undefined"
#
# Then run:
#   ORIG_REL=... ORIG_DBG=... FIXED_REL=... FIXED_DBG=... ./repro.sh
#
# Any binary can be left out; the corresponding runs are skipped.
# Searches use the material evaluation of the test harness, not NNUE.

set -u
export ASAN_OPTIONS=handle_abort=1
export UBSAN_OPTIONS=print_stacktrace=0

# run <binary> <uci commands> <seconds to wait> <grep pattern>
run() {
    local bin=$1 cmds=$2 wait=$3 pat=$4
    [ -n "$bin" ] && [ -x "$bin" ] || { echo "   (skipped)"; return; }
    (printf "$cmds"; sleep "$wait"; echo quit) | timeout $((wait + 120)) "$bin" 2>&1 \
        | cat -v | grep -E "$pat" | sed -E 's/ hashfull [0-9]+ tbhits [0-9]+//' | sort -u -s -k1,1 | head -8
}

both() {  # both <label> <use debug?> <cmds> <wait> <pattern>
    local label=$1 dbg=$2 cmds=$3 wait=$4 pat=$5
    if [ "$dbg" = 1 ]; then o=${ORIG_DBG:-}; f=${FIXED_DBG:-}; else o=${ORIG_REL:-}; f=${FIXED_REL:-}; fi
    echo "-- $label"
    echo " original:"; run "$o" "$cmds" "$wait" "$pat"
    echo " fixed:";    run "$f" "$cmds" "$wait" "$pat"
}

ERR='runtime error|AddressSanitizer|Assertion'

echo "== B1 dangling StateInfo in SearchManager::pv() (debug)"
both "PV with several non-dark moves" 1 \
  "position fen 2bakab2/9/9/9/9/9/9/9/9/3K1R3 w R0A0C0P0N0B0r0a0c0p0n0b0 0 1\ngo depth 8\n" 15 \
  "$ERR|^bestmove"

echo "== B2 mix of wins and losses reported as a forced result (release)"
both "reveal a0a9: 2/3 rook: white mates at once, 1/3 knight: black mates" 0 \
  "position fen 3k5/2P6/9/3P5/9/9/9/8r/7r1/X3K4 w R2A0C0P0N1B0r0a0c0p0n0b0 0 1\ngo depth 12 searchmoves a0a9\n" 10 \
  "MIXED_DECISIVE|info depth 12 "

echo "== B3 undo_flip() does not restore the hash key (release)"
both "start position, depth 8" 0 "position startpos\ngo depth 8\n" 30 "UNDO_FLIP"

echo "== B4 Zobrist keys of the pool collide with keys of pieces (release)"
for F in "3k5/9/9/9/9/9/9/9/9/R3K4 w R1A0C0P0N0B0r0a0c0p0n0b0 0 1" \
         "3k5/9/9/9/9/9/9/9/9/4K4 w R0A0C0P0N0B0r0a0c0p0n0b0 0 1" \
         "3k5/9/9/9/9/9/9/9/9/P3K4 w R0A0C0P1N0B0r0a0c0p0n0b0 0 1"; do
  both "$F" 0 "position fen $F\nd\n" 2 "^Key"
done

echo "== B5 key depends on how the position was set up (release)"
both "position startpos moves b2b9Cn" 0 "position startpos moves b2b9Cn\nd\n" 2 "^Fen|^Key"
both "position fen <same position>" 0 \
  "position fen xCxxkxxxx/9/1x5x1/x1x1x1x1x/9/9/X1X1X1X1X/7X1/9/XXXXKXXXX b R2A2C1P5N2B2r2a2c2p5n1b2 0 1\nd\n" 2 "^Key"

echo "== B6 empty pool with dark pieces on the board"
both "plain xiangqi FEN fields, release" 0 \
  "position fen xxxxkxxxx/9/1x5x1/x1x1x1x1x/9/9/X1X1X1X1X/1X5X1/9/XXXXKXXXX w - - 0 1\ngo depth 4\n" 5 \
  "info string Invalid|info depth 4 "
both "plain xiangqi FEN fields, debug" 1 \
  "position fen xxxxkxxxx/9/1x5x1/x1x1x1x1x/9/9/X1X1X1X1X/1X5X1/9/XXXXKXXXX w - - 0 1\ngo depth 4\n" 10 \
  "$ERR|^bestmove"

echo "== B7 FEN parsing writes out of bounds (debug, UBSan)"
both "dark piece on a non-starting square" 1 \
  "position fen 3k5/9/9/9/9/4X4/9/9/9/4K4 w R1A0C0P0N0B0r0a0c0p0n0b0 0 1\nd\n" 3 "$ERR|^Fen"
both "'X' in the pool field" 1 \
  "position fen 3k5/9/9/9/9/9/9/9/9/X3K4 w X1R1A0C0P0N0B0r0a0c0p0n0b0 0 1\nd\n" 3 "$ERR|^Fen"

echo "== B8 reveal letters are not validated (release)"
for mv in a3a4z a3a4r; do
  both "position startpos moves $mv" 0 "position startpos moves $mv\nd\ngo depth 3\n" 4 "info string Invalid|^Fen|^bestmove"
done
both "position startpos moves a3a4 (no reveal letter), debug" 1 \
  "position startpos moves a3a4\ngo depth 3\n" 5 "$ERR|info string Invalid|^bestmove"
both "letter after a capture of a revealed piece" 0 \
  "position fen 3k5/9/9/9/9/9/9/9/r8/R3K4 w R0A0C0P0N0B0r1a0c0p0n0b0 0 1 moves a0a1p\nd\n" 2 "info string Invalid|^Fen"

echo "== B9 perft reads uninitialized StateInfo fields (release)"
for fill in 0 255; do
  echo "-- go perft 2, StateInfo filled with $fill"
  for v in ORIG_REL FIXED_REL; do
    bin=${!v:-}; [ -x "$bin" ] || continue
    printf " %-9s " "$v"; (printf "position startpos\ngo perft 2\n"; sleep 3; echo quit) | PERFT_FILL=$fill "$bin" 2>/dev/null | grep Nodes
  done
done

echo "== B10 unsound averaging of bounds (original tree only, statistics)"
run "${ORIG_REL:-}" "position startpos\ngo depth 8\n" 20 "STATS"
