#!/usr/bin/env python3
"""TEST ONLY - never commit the result.

Prepares a copy of the Pikafish 'jieqi' source tree for the reproduction
script (repro.sh). Works on the original branch (9b963f7) and on the tree
with the fix series applied.

Changes made to <src>:
  1. nnue/network.cpp : skip the network check (no public Jieqi network).
  2. search.cpp       : replace the NNUE evaluation with a simple material
                        evaluation, so that searches can run.
  3. search.cpp       : count, around do_flip()/undo_flip() in flip_search(),
                        whether undo_flip() restores the hash key (printed to
                        stderr as [UNDO_FLIP]).
  4. perft.h          : if the environment variable PERFT_FILL is set, fill
                        the StateInfo with that byte before every do_move(),
                        to expose reads of uninitialized fields.
  5. search.cpp       : (original tree only) print chance nodes whose
                        outcomes mix wins and losses ([MIXED_DECISIVE]) and
                        statistics about bounds ([STATS]) at bestmove.

The bugs being reproduced are logic and memory errors, which do not depend
on the evaluation. The node counts of [STATS] do depend on it.

Usage: apply_test_harness.py <path-to-src>
"""
import re
import sys

src = sys.argv[1].rstrip("/")


def edit(path, old, new, required=True):
    p = f"{src}/{path}"
    s = open(p).read()
    if old not in s:
        if required:
            sys.exit(f"pattern not found in {path}:\n{old}")
        return False
    open(p, "w").write(s.replace(old, new, 1))
    return True


# 1. Skip the network check
edit("nnue/network.cpp",
     "                                        const std::function<void(std::string_view)>& f) const {\n"
     "    if (evalfilePath.empty())",
     "                                        const std::function<void(std::string_view)>& f) const {\n"
     "    return;  // TEST ONLY\n"
     "    if (evalfilePath.empty())")

# 2. Material evaluation
edit("search.cpp",
     """Value Search::Worker::evaluate(const Position& pos) {
    return Eval::evaluate(networks[numaAccessToken], pos, accumulatorStack, refreshTable,
                          optimism[pos.side_to_move()]);
}""",
     """Value Search::Worker::evaluate(const Position& pos) {
    // TEST ONLY: material evaluation instead of NNUE
    int v = 0;
    for (Square sq = SQ_A0; sq <= SQ_I9; ++sq)
    {
        Piece pc = pos.piece_on(sq);
        if (pc == NO_PIECE)
            continue;
        int pv = PieceValue[pc] + 7 * int(sq % 9) % 5;
        v += color_of(pc) == pos.side_to_move() ? pv : -pv;
    }
    return std::clamp(v, VALUE_MATED_IN_MAX_PLY + 1, VALUE_MATE_IN_MAX_PLY - 1);
}""")

# 3. undo_flip() key check
edit("search.cpp",
     "        Piece flipped_piece = pos.do_flip((ss - 1)->currentMove.to_sq(), piece, &dp, &tt);",
     "        Key   TEST_before   = pos.state()->key;\n"
     "        Piece flipped_piece = pos.do_flip((ss - 1)->currentMove.to_sq(), piece, &dp, &tt);")
edit("search.cpp",
     "        pos.undo_flip((ss - 1)->currentMove.to_sq(), flipped_piece);",
     """        pos.undo_flip((ss - 1)->currentMove.to_sq(), flipped_piece);
        {
            static uint64_t TEST_ok = 0, TEST_bad = 0;
            (pos.state()->key == TEST_before ? TEST_ok : TEST_bad)++;
            if (((TEST_ok + TEST_bad) & ((1 << 20) - 1)) == 0)
                std::cerr << "[UNDO_FLIP] key restored=" << TEST_ok << " not restored=" << TEST_bad << "\\n";
        }""")

# 4. PERFT_FILL hook
p = f"{src}/perft.h"
s = open(p).read()
s = s.replace('#include "position.h"', '#include <cstdlib>\n#include <cstring>\n#include "position.h"', 1)
s, n = re.subn(r"(\n(\s*)pos\.do_move\(m, st\);)",
               lambda m: "\n" + m.group(2) + 'if (const char* f = std::getenv("PERFT_FILL"))  // TEST ONLY'
               + "\n" + m.group(2) + "    std::memset(&st, std::atoi(f), sizeof(st));" + m.group(1),
               s, count=1)
if n != 1:
    sys.exit("perft.h: do_move() not found")
open(p, "w").write(s)

# 5. Original tree only: statistics on flip_search() results
if edit("search.cpp", "    bool all_decisive = true;", """    {   // TEST ONLY
        TEST_flipNodes++;
        bool lo = false, hi = false, win = false, loss = false;
        for (const auto& e : results)
        {
            lo |= e.value <= alpha;
            hi |= e.value >= beta;
            win |= is_win(e.value);
            loss |= is_loss(e.value);
        }
        TEST_mixedBounds += lo && hi;
        if (win && loss && ++TEST_mixedDecisive <= 3)
        {
            std::cerr << "[MIXED_DECISIVE] last move " << UCIEngine::move((ss - 1)->currentMove) << ", outcomes:";
            for (const auto& e : results)
                std::cerr << " " << e.value << " x" << e.count;
            std::cerr << "\\n";
        }
    }
    bool all_decisive = true;""", required=False):
    edit("search.cpp", "namespace Stockfish {",
         "namespace Stockfish {\nstatic std::atomic<uint64_t> TEST_flipNodes{0}, TEST_mixedBounds{0}, TEST_mixedDecisive{0};")
    edit("search.cpp", "    auto bestmove = UCIEngine::move(bestThread->rootMoves[0].pv[0]);",
         """    std::cerr << "[STATS] chance nodes with several outcomes=" << TEST_flipNodes
              << " mixing fail-low and fail-high=" << TEST_mixedBounds
              << " mixing wins and losses=" << TEST_mixedDecisive << "\\n";
    auto bestmove = UCIEngine::move(bestThread->rootMoves[0].pv[0]);""")

print(f"test harness applied to {src}")
