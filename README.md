# Jieqi

Jieqi is a UCI engine for jieqi (揭棋, xiangqi with face-down pieces). It uses a
classical hand-written evaluation and no neural network. It is based on the jieqi engine of
Pikafish's `jieqi_old` branch, with a series of correctness fixes on top. For the NNUE
engine, see [Maskfish](https://github.com/nguyenhungitc/maskfish).

Every fix addresses a defect that was reproduced and measured before and after the change.
Each commit message describes the defect, how it was measured and the effect of the fix. The
measurement scripts and raw results are not part of this repository, except the match
runner `tests/match.py`.

## Base

[`official-pikafish/Pikafish`](https://github.com/official-pikafish/Pikafish), branch
`jieqi_old`, commit `23b9466c`, imported here as `d95db86` ("Initial commit"). The import
leaves the engine source unchanged. It differs from upstream only in that:

- `src/position.cpp`, `src/uci.cpp` use LF line endings;
- the CI workflow (`.github/workflows/pikafish.yml`) and the empty `Pikafish` submodule
  entry are removed;
- `tests/*.sh` are no longer marked executable (run them with `bash tests/<script>.sh`).

## Commits

One commit per defect, in order:

| Commit | Change |
|---|---|
| `f837f6a1` | `Zobrist::psqDark` sized for the keys `init()` writes (out-of-bounds write; keys unchanged) |
| `8cb9b63d` | Material imbalance counts Red's dark cannons, knights and bishops as dark |
| `331ffe3e` | UCI `position`: `pCaptured` reset per move; a dark move without an identity is rejected |
| `e0f88e45` | FEN: a pool count without a piece letter is ignored |
| `4a7d2a78` | `set_check_info`: every `checkSquares` slot initialised |
| `d0aa3751` | `getDark`/`setDark`: canonical keys, no bloom-filter writes, revealed state initialised |
| `dabc00ff` | `materialKey`, `material[]` and `psq` stay equal to `set()` when an identity is fixed |
| `1df88ed9` | GUI reveal: the face-down piece is removed from the key |
| `82d16c5b` | `ScoreCalc`: same aggregation for Red and Black (mover's point of view) |
| `1ed94c09` | No false mate scores at the dark-depth limit (fallback to static evaluation) |
| `684fac7f` | A move's value is its last (re-)search; `newDepth` is reset per identity |
| `03d9dbc2` | The root PV only takes the line of the move it belongs to |
| `057ccfd3` | GUI capture: the pool change goes into the new key |
| `7959b6f` | `Reductions[]` sized for every depth (out-of-bounds read from depth 128) |
| `0a47d93` | No `ponder` move and no PV continuation after a face-down move |
| `4dddb93` | bench: FENs in the format `fen_is_valid()` accepts (10 of 11 were rejected) |
| `b46eabd` | Step 10: the TT-depth reduction requires a TT hit |
| `31231f7` | `fen()`: an empty pool is written as `-` (`flip` misread the move counters) |
| `21e6cd7` | Time management: no NaN when the side to move has no time left |
| `710e25b` | Repetition: scan only back to the last irreversible move; root-relative folds (Pikafish) |
| `9cd9bd8` | 60-move rule with `check10`, mate downgrade and TT key buckets (Pikafish) |
| `94177b6` | Correction history; the structure keys include face-down pieces and pools (Pikafish) |
| `6caa43d` | TT replacement and aging (Pikafish) |
| `fb003a5` | Low-ply history and main history aging (Pikafish) |
| `4ac1f9f` | `tests/match.py`: match runner |
| `bd3c179` | `setoption`: a non-numeric spin value is rejected (it terminated the engine) |
| `a2e9667` | `go wtime 0 btime 0` is time-managed (it searched until `stop`); time limits wait for depth 1 |
| `8358294` | `searchmoves` / `banmoves`: illegal tokens skipped; `banmoves` resolved at `go` |
| `296fa74` | Root: `likelyFailLow` requires a TT hit |
| `fbd45ab` | No `ponder` move that captures a face-down piece |
| `2785d5e` | The king-capture debug print no longer goes into the UCI stream |
| `a3e5f23` | `ChaseMap` reads its victims independently of byte order |
| `755ec08` | `tests/match.py`: repetition, perpetual check and 60-move adjudication, dealt identities, timeouts |

## Headline results

Base (upstream `23b9466c`) vs head (Jieqi). Counters were taken on 32 self-play
positions at 400k nodes each, using an instrumented build that compares the incremental
state with a from-scratch recomputation. The scripts are not included here.

| Check | Base | Head |
|---|---|---|
| valgrind (`go depth 6` from the start position) | 622,580 errors | 0 |
| Same best move and score with normal / zero-init / pattern-init builds | 1 of 32 positions | 32 of 32 |
| Debug build, `go depth 7` from the start position | assertion (false mate) | completes |
| Missed repetitions (bloom filter) | 538 | 0 |
| Revealed states whose key differs from a from-scratch key | 91.9% | 0 |
| Evaluations using a stale cached imbalance | 91.2% | 0 |
| qsearch mate scores in positions with legal moves | 8,378 | 0 |
| Exact PV scores overwritten by a lower bound | 3,099 | 0 |

## Search ports from Pikafish

`710e25b` to `fb003a5` port search techniques from Pikafish `master` (no NNUE). Each was
measured with `tests/match.py`, which plays two engines through UCI and acts as the
referee, and replays every opening with colours swapped. These matches used the runner of
`4ac1f9f`, which drew an identity when a face-down piece moved and did not adjudicate
repetitions, perpetual checks or the 60-move rule (games ended in mate or at 300 plies).
Since `755ec08` the identities are dealt before the game and those rules are applied, so
new measurements are not directly comparable with the table below.

| Match | Games | Nodes / move | Elo (95%) |
|---|---|---|---|
| repetition vs previous | 200 | 20000 | +20.9 [−27.0, +69.6] |
| 60-move rule vs previous | 400 | 20000 | −13.9 [−48.1, +20.0] |
| correction history vs previous | 400 | 20000 | +20.0 [−13.8, +54.2] |
| TT vs previous | 400 | 20000 | +0.0 [−33.9, +33.9] |
| repetition to TT vs `21e6cd7` | 600 | 20000 | +6.4 [−21.2, +34.1] |
| repetition to TT vs `21e6cd7` | 200 | 80000 | **+61.4 [+14.0, +111.2]** |
| history (`fb003a5`) vs previous | 400 | 20000 | −21.7 [−56.0, +12.1] |

The history commit measured negative (an earlier run of 280 games gave −8.7 [−49.5,
+31.9]); its constants come from Pikafish and probably need tuning here. These are
small matches at short searches; confirm with an SPRT at the time control you use.

## Behaviour changes

- **Search results differ from base.** Base results are not reproducible in the first place:
  they change with stack contents. Playing strength has **not** been measured. Run a match
  or an SPRT against base before relying on Jieqi.
- **Black's flip nodes are scored exactly like Red's.** The pessimistic branch of `ScoreCalc`
  now applies to whichever side flips.
- **Flips at the dark-depth limit.** A flip that cannot be expanded there gets the static
  evaluation of the unrevealed position instead of being dropped.
- **`material[]`** no longer contains the `+69` / `+75` reveal and capture constants. It is
  used by `material_diff()` (search complexity).
- **UCI `position ... moves`.** A move of a face-down piece must carry its identity as a 5th
  character. A 4-character dark move prints `info string error: …`, and the rest of the move
  list is ignored. Positions sent as a FEN are unaffected.
- **`ponder` and PV output.** No `ponder` move is printed when the best move or the reply
  moves a face-down piece, and a PV stops after its first face-down move: neither can be
  sent back by a GUI without the identity. Nor when the reply captures a face-down piece,
  whose identity is only known once the reply is played.
- **`go` limits.** `wtime`/`btime` count as a clock even when 0: the engine completes
  depth 1 and answers. Time limits (clock and `movetime`) apply only once depth 1 is
  complete. `searchmoves` and `banmoves` tokens that are not legal moves print
  `info string error: …` and are skipped; `banmoves` is matched against the position of
  the next `go`, so it may come before `position`.
- **`setoption`** ignores a spin value that is not a number instead of terminating.
- **`fen()`** writes an empty pool as `-` instead of an empty field.
- **Bench signature.** All 11 bench positions are searched now, and the search changes
  below change the search: the default `bench` gives 8570091.
- **Repetitions.** A position that occurred once at or before the root no longer ends a
  line: it must repeat after the root, or twice.
- **60-move rule.** The FEN halfmove clock is read and written. At 120 plies (a side's
  checks beyond 10 do not count) the game is a draw. The counter restarts after a capture
  and after a reveal; drop the reveal in `do_move()` if your rules only count captures.
- **Hash key in `d`.** `Key:` is the TT key, which differs from the repetition key once
  the 60-move counter reaches 14 or the position may have occurred before.

## Known open issues

These were verified but are not fixed:

- **The evaluation is not colour-symmetric** because of `PSQT::psqCap`: at the symmetric start
  position it gives +1.35 / −2.60 pawns for Red. Fixing this requires re-tuning the table.
- **`ScoreCalc` averages per-identity results** that may be bounds of different kinds
  (upper, lower or exact). An average of mixed bounds is not a bound. Fixing this is a
  design change (for example Star1/Star2-style windows at chance nodes).
- **Face-down advisors are left out of the evaluation's attack maps**: `Evaluation` uses
  `pieces(Us, ADVISOR)`, and a face-down advisor has type `ADVISOR_B`. Changing it changes
  the evaluation and needs a strength test.
- **The search is shallow in the opening.** A flip's children are searched at depth 0
  (qsearch) once `darkTypes`, the product of the identity counts of the flips on the
  line, exceeds `MAXDARKTYPES` (43), or from the fifth flip on (`MAXDARKDEPTH` = 4).
  With full pools that is the third flip: from the start position `go depth 22` reports
  `seldepth 6`, so the reported depth overstates the search there. Raising the limits
  costs nodes at every chance node and needs a strength test at a fixed time control.
- **`go perft` does not reveal**: after a dark move the piece stays face-down off its
  starting point and keeps moving like the piece type of that point, so the counts do not
  follow jieqi rules.

## Build

```sh
make -C src -j build ARCH=x86-64-sse41-popcnt   # src/jieqi
```

## Using it

The engine speaks UCI. A jieqi position is a FEN with five fields:

1. **Board.** A face-down piece is `X` (red) or `x` (black). Generals are never dark.
2. **Side to move.** `w` is red.
3. **Pools.** Each side's identities still face-down, as `<piece><count>` pairs in the order
   R A C P N B.
4. **No-capture ply count.**
5. **Move number.**

```
position fen xxxxkxxxx/9/1x5x1/x1x1x1x1x/9/9/X1X1X1X1X/1X5X1/9/XXXXKXXXX w R2A2C2P5N2B2r2a2c2p5n2b2 0 1
go movetime 4000
```

A dark piece moves according to the point it starts on and reveals when it moves.

In a move list:

- A move by a face-down piece carries the revealed identity as a 5th character: `e3e4P`.
- A capture of a face-down piece may carry the captured identity as the 6th character
  (`b2b9Cn`), or as the 5th character when the mover was already face-up.

## License

Jieqi is free software, distributed under the GNU General Public License version 3
or later (GPL-3.0-or-later).