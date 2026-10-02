# Pikafish `jieqi_old`: verified fixes

This branch is the jieqi (揭棋, xiangqi with face-down pieces) engine from Pikafish's
`jieqi_old` branch, with a series of correctness fixes on top. It uses a classical
hand-written evaluation and no neural network.

Every fix addresses a defect that was reproduced and measured before and after the change.
Each commit message describes the defect, how it was measured and the effect of the fix. The
measurement scripts and raw results are not part of this repository.

## Base

[`official-pikafish/Pikafish`](https://github.com/official-pikafish/Pikafish), branch
`jieqi_old`, commit `23b9466c`, imported here as `d95db86` ("Initial commit"). The import
leaves the engine source unchanged. It differs from upstream only in that:

- `src/position.cpp`, `src/uci.cpp`, `Copying.txt` and `NNUE-License.txt` use LF line endings;
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

## Headline results

Base (upstream `23b9466c`) vs head (this branch). Counters were taken on 32 self-play
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

## Behaviour changes

- **Search results differ from base.** Base results are not reproducible in the first place:
  they change with stack contents. Playing strength has **not** been measured. Run a match
  or an SPRT against base before relying on this branch.
- **Black's flip nodes are scored exactly like Red's.** The pessimistic branch of `ScoreCalc`
  now applies to whichever side flips.
- **Flips at the dark-depth limit.** A flip that cannot be expanded there gets the static
  evaluation of the unrevealed position instead of being dropped.
- **`material[]`** no longer contains the `+69` / `+75` reveal and capture constants. It is
  used by `material_diff()` (search complexity).
- **UCI `position ... moves`.** A move of a face-down piece must carry its identity as a 5th
  character. A 4-character dark move prints `info string error: …`, and the rest of the move
  list is ignored. Positions sent as a FEN are unaffected.

## Known open issues

Both were verified but are not fixed:

- **The evaluation is not colour-symmetric** because of `PSQT::psqCap`: at the symmetric start
  position it gives +1.35 / −2.60 pawns for Red. Fixing this requires re-tuning the table.
- **`ScoreCalc` averages per-identity results** that may be bounds of different kinds
  (upper, lower or exact). An average of mixed bounds is not a bound. Fixing this is a
  design change (for example Star1/Star2-style windows at chance nodes).

## Build

```sh
make -C src -j build ARCH=x86-64-sse41-popcnt   # src/PikaJieQi
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

GPL-3.0-or-later, as upstream. See `Copying.txt` and `AUTHORS` for the Pikafish authors.
