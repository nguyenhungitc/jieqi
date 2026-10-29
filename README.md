# Jieqi - *luck is a skill too*

Jieqi is a UCI engine for jieqi (揭棋, xiangqi with face-down pieces). It uses a
classical hand-written evaluation and no neural network. It is based on the jieqi engine of
Pikafish's [`jieqi_old`](https://github.com/official-pikafish/Pikafish/tree/jieqi_old) branch,
with a series of correctness fixes on top (see [Changes from base](#changes-from-base)). For
the NNUE engine, see [Maskfish](https://github.com/nguyenhungitc/maskfish).

## Build

```sh
make -C src -j build ARCH=x86-64-sse41-popcnt   # produces src/jieqi (src/jieqi.exe on Windows)
```

- Without `ARCH`, the Makefile uses `x86-64-modern`, which is currently `x86-64-sse41-popcnt`.
- `make -C src help` lists every target and architecture.
- `make -C src -j profile-build ARCH=…` builds with profile-guided optimization.
- `build-all.sh` builds several x86-64 architectures on Linux. It writes the binaries to
  `dist/jieqi-linux-<arch>`, one log per build to `dist/logs/`, and a `SHA256SUMS` file:

  ```sh
  bash build-all.sh                     # every architecture
  bash build-all.sh avx2 bmi2           # only some (short names allowed)
  COMP=clang JOBS=8 bash build-all.sh   # change compiler / parallel jobs
  TARGET=profile-build bash build-all.sh
  ```

  The evaluation is classical, so the wider SIMD builds are not expected to be much faster
  than `x86-64-sse41-popcnt`.

## Using it

The engine speaks UCI. It does not deal the hidden identities itself: the GUI (or match
runner) keeps them and tells the engine each identity when it is revealed.

### Position

```
position startpos
position fen xxxxkxxxx/9/1x5x1/x1x1x1x1x/9/9/X1X1X1X1X/1X5X1/9/XXXXKXXXX w R2A2C2P5N2B2r2a2c2p5n2b2 0 1
go movetime 4000
```

`startpos` is the FEN above. A jieqi FEN has five fields:

1. **Board.** Ranks 9 to 0, files a to i. Red (uppercase) starts on ranks 0–4. A face-down
   piece is `X` (red) or `x` (black) and may only stand on a starting point of its own side.
   Generals are never face-down.
2. **Side to move.** `w` is Red, `b` is Black.
3. **Pools.** The identities each side still has face-down, as `<piece><count>` pairs
   (`R` rook, `A` advisor, `C` cannon, `P` pawn, `N` knight, `B` bishop; lowercase for
   Black). The pairs may come in any order; the engine writes them as R A C P N B, Red
   first. An empty pool is written `-`. Each side needs at least one pool identity per
   face-down piece, and no more of a type than it starts with.
4. **60-move counter.** Plies counted by the 60-move rule (see
   [Changes from base](#changes-from-base)), 0–119.
5. **Move number.**

A standard xiangqi FEN with an extra `-` field (`w - - 0 1`) is also accepted. An invalid
FEN prints `info string error: invalid FEN (…); position not changed` and the current
position is kept.

### Moves

Moves are written `<from><to>`, for example `h2e2`. A face-down piece moves according to
the starting point it stands on and is revealed when it moves.

In a `position … moves` list, the GUI supplies the identities:

- A move by a face-down piece **must** carry its revealed identity as a 5th character:
  `e3e4P`.
- A capture of a face-down piece may carry the captured identity as the 6th character
  (`b2b9Cn`), or as the 5th character when the mover was already face-up (`e2e6p`). `x` or
  `X` there means "unknown".

Every identity is checked against the position (colour, pool, not a king). A move that
cannot be applied prints `info string error: move …: …; ignoring it and the rest of the
move list`.

The engine's own output (`bestmove`, `ponder`, `pv`) uses plain 4-character moves, because
the engine does not know the identity of a face-down piece. When the engine moves a
face-down piece, the GUI must add the revealed identity before sending the move back in
`position … moves`. For the same reason, no `ponder` move is printed when the best move or
the reply moves a face-down piece, or when the reply captures one, and a PV stops after its
first face-down move.

### `go`

All standard limits are supported (`wtime`, `btime`, `winc`, `binc`, `movestogo`, `depth`,
`nodes`, `movetime`, `mate`, `infinite`, `ponder`, `searchmoves`), plus `perft`.

- The engine always completes depth 1 before it stops, even with `wtime 0` or a tiny
  `movetime`.
- `banmoves <move> …`, sent as a separate command before `go`, excludes moves from the
  root of the next search only. The moves are matched against the position of that `go`,
  so `banmoves` may come before `position`.
- A `searchmoves` or `banmoves` token that is not a legal move prints
  `info string error: …` and is skipped.

### Options

| Option | Default | Range / type |
|---|---|---|
| `Threads` | 1 | 1–1024 |
| `Hash` | 16 | MB |
| `Clear Hash` | | button |
| `Ponder` | false | |
| `MultiPV` | 1 | 1–500 |
| `Move Overhead` | 10 | ms, 0–5000 |
| `Slow Mover` | 100 | 10–1000 |
| `nodestime` | 0 | 0–10000 |
| `UCI_AnalyseMode` | false | |
| `UCI_ShowWDL` | false | |
| `Debug Log File` | | path |

`setoption` ignores a spin value that is not a number.

### Other commands

These are for debugging and must not be sent during a search.

- `d` prints the board, the FEN, the hash key and the checkers.
- `eval` prints the evaluation terms of the current position (middlegame and endgame
  values).
- `flip` swaps the colours of the current position.
- `bench [hash] [threads] [limit] [fenfile|default|current] [depth|nodes|movetime|perft|eval]`
  searches a list of positions and prints the total node count. The defaults are
  `16 1 13 default depth`.
- `compiler` prints the compiler and build settings.

## Testing

- **Bench signature.** The default `bench` gives **8570091** nodes. Run from `src/`:

  ```sh
  cd src && bash ../tests/signature.sh 8570091
  ```

- **Matches.** `tests/match.py` plays two engines against each other through UCI. It deals
  the hidden identities from a seed, plays each opening twice with colours swapped, and
  adjudicates mate, illegal moves, timeouts, repetitions (perpetual check loses, perpetual
  chase is scored as a draw) and the 60-move rule.

  ```sh
  python3 tests/match.py ENGINE_A ENGINE_B [--pairs N] [--nodes N] [--jobs N] [--seed S] \
                                           [--book N] [--maxply N] [--timeout S]
  ```

## Changes from base

Search results differ from base. Base results are not reproducible in the first place: they
change with stack contents.

**Input and output**

- **FEN.** FENs are checked strictly (board shape, kings in their palaces, face-down
  pieces on their own starting points, pool sizes, side not to move not in check). Base
  could write outside the board or crash on a malformed FEN. `fen()` writes an empty pool
  as `-` instead of an empty field, and the 60-move counter is read and written.
- **Moves.** A move of a face-down piece must carry its identity; a 4-character dark move
  is rejected. Identities are checked against the position instead of being applied
  blindly.
- **`ponder` and PV.** See [Moves](#moves).
- **`go` limits.** `wtime`/`btime` count as a clock even when 0. Time limits (clock and
  `movetime`) apply only once depth 1 is complete. Invalid `searchmoves` and `banmoves`
  tokens are reported and skipped instead of producing `bestmove (none)`.
- **`setoption`** ignores a spin value that is not a number instead of terminating.
- **`d`.** `Key:` is the TT key. It differs from the repetition key once the 60-move counter
  reaches 14 or the position may have occurred before.
- **Bench.** All 11 bench positions are searched; base rejected 10 of them because of the
  FEN field order.

**Search and evaluation**

- **Black's flip nodes are scored exactly like Red's.** The pessimistic branch of `ScoreCalc`
  now applies to whichever side flips.
- **Mates at flip nodes.** A flip is scored as a mate only if every identity mates, and
  then in as many plies as the slowest one. Base claimed a mate when a single identity
  mated.
- **Flips at the dark-depth limit.** A flip that cannot be expanded there gets the static
  evaluation of the unrevealed position instead of being dropped.
- **`material[]`** no longer contains the `+69` / `+75` reveal and capture constants. It is
  used by `material_diff()` (search complexity).
- **Repetitions.** A position that occurred once at or before the root no longer ends a
  line: it must repeat after the root, or twice.
- **60-move rule.** At 120 plies the game is a draw. A side's checks beyond 10 since the
  last reset do not count, nor do the replies to them. The counter restarts after a capture
  and after a reveal; drop the reveal in `do_move()` if your rules only count captures.

## Known open issues

These were verified but are not fixed:

- **The evaluation is not colour-symmetric** because of `PSQT::psqCap`: at the symmetric
  start position `eval` gives +1.35 (middlegame) / −2.60 (endgame) pawns for Red. Fixing
  this requires re-tuning the table.
- **`ScoreCalc` averages per-identity results** that may be bounds of different kinds
  (upper, lower or exact). An average of mixed bounds is not a bound. Fixing this is a
  design change (for example Star1/Star2-style windows at chance nodes).
- **Face-down advisors are left out of the evaluation's attack maps**:
  `Evaluation::pieces<Us, ADVISOR>()` reads `pos.pieces(Us, ADVISOR)`, and a face-down
  advisor has type `ADVISOR_B`. Changing it changes the evaluation and needs a strength
  test.
- **The search is shallow in the opening.** A flip's children are searched at depth 0
  (qsearch) once `darkTypes`, the product of the identity counts of the flips on the
  line, exceeds `MAXDARKTYPES` (43), or from the fifth flip on (`MAXDARKDEPTH` = 4).
  With full pools that is the third flip: from the start position `go depth 22` reports
  `seldepth 6`, so the reported depth overstates the search there. Raising the limits
  costs nodes at every chance node and needs a strength test at a fixed time control.
- **`go perft` does not reveal**: after a dark move the piece stays face-down off its
  starting point and keeps moving like the piece type of that point, so the counts do not
  follow jieqi rules.
- **`eval` output** still ends with "[with scaled NNUE, …]", a leftover label: there is no
  NNUE.

## License

Jieqi is free software, distributed under the GNU General Public License version 3
or later (GPL-3.0-or-later). See `Copying.txt` and `AUTHORS`.