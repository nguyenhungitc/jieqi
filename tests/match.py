#!/usr/bin/env python3
"""Jieqi match runner: plays engine A against engine B through UCI.

The runner is the referee. Before each game it deals the hidden identities:
every face-down piece of the start position gets one identity from its side's
pool, shuffled with the game's seed. When a face-down piece moves, the runner
appends that identity to the move; when one is captured, it tells both engines
what it was (as the capturer would see it). Games come in pairs with the same
opening and the same identities, colours swapped.

A game ends when:
  - the side to move has no legal move: it loses (checkmate or stalemate);
  - an engine sends an illegal move, "(none)" with legal moves left, crashes or
    exceeds --timeout: it loses;
  - a position occurs for the third time since the last capture or reveal: the
    side that gave check with every one of its moves in the cycle loses
    (perpetual check), otherwise the game is drawn. Perpetual chase is not
    adjudicated: such a repetition is scored as a draw;
  - the 60-move counter that the engine keeps (FEN halfmove field, checks beyond
    10 not counted) reaches 120 plies: draw;
  - --maxply plies were played: draw.

The Elo interval is computed from the game pairs, which are not independent
games. Two identical deterministic engines play every pair as one game twice,
so A against itself always scores exactly 50%.

usage: match.py ENGINE_A ENGINE_B [--pairs N] [--nodes N] [--jobs N] [--seed S]
                                  [--book N] [--maxply N] [--timeout S]
"""
import argparse, math, os, queue, random, subprocess, sys, threading, time
from concurrent.futures import ThreadPoolExecutor

STARTFEN = "xxxxkxxxx/9/1x5x1/x1x1x1x1x/9/9/X1X1X1X1X/1X5X1/9/XXXXKXXXX w R2A2C2P5N2B2r2a2c2p5n2b2 0 1"
STARTPOOL = {"R": 2, "A": 2, "C": 2, "P": 5, "N": 2, "B": 2,
             "r": 2, "a": 2, "c": 2, "p": 5, "n": 2, "b": 2}


class EngineError(RuntimeError):
    pass


class Engine:
    def __init__(self, path, timeout):
        self.path, self.timeout = path, timeout
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)
        # A reader thread feeds a queue, so that every wait can time out instead of
        # blocking forever in readline() on a hung engine.
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.send("uci"); self.wait("uciok")
        self.send("setoption name Hash value 8")
        self.send("isready"); self.wait("readyok")

    def _read(self):
        for line in self.p.stdout:
            self.lines.put(line.rstrip("\n"))
        self.lines.put(None)  # EOF: the engine exited

    def send(self, s):
        try:
            self.p.stdin.write(s + "\n"); self.p.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            raise EngineError(f"{self.path}: engine died")

    def wait(self, prefix, timeout=None):
        deadline = time.monotonic() + (timeout or self.timeout)
        lines = []
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise EngineError(f"{self.path}: no '{prefix}' within {timeout or self.timeout} s")
            try:
                line = self.lines.get(timeout=left)
            except queue.Empty:
                continue
            if line is None:
                raise EngineError(f"{self.path}: engine died")
            lines.append(line)
            if line.startswith(prefix):
                return lines

    def quit(self):
        try:
            self.send("quit"); self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def board_of(fen):
    b = {}
    for i, row in enumerate(fen.split()[0].split("/")):
        f = 0
        for c in row:
            if c.isdigit(): f += int(c)
            else: b[(f, 9 - i)] = c; f += 1
    return b


def sq(s):
    return (ord(s[0]) - 97, int(s[1]))


def deal_identities(seed):
    """Square -> identity for every face-down piece of the start position.

    A face-down piece only leaves its square by moving, which reveals it, and is
    only removed by a capture, so its identity can be fixed by its start square.
    Drawing it when the piece moved, as before, gave the two games of a pair
    different identities as soon as they diverged."""
    rng = random.Random(seed)
    deal = {}
    for red in (True, False):
        squares = sorted(s for s, c in board_of(STARTFEN).items() if c == ("X" if red else "x"))
        idents = [k for k, v in STARTPOOL.items() if k.isupper() == red for _ in range(v)]
        rng.shuffle(idents)
        deal.update(zip(squares, idents))
    return deal


def referee_state(ref, moves):
    """FEN, side-to-move-in-check flag and legal moves of the position after 'moves'."""
    ref.send("position startpos moves " + " ".join(moves))
    ref.send("d")
    out = ref.wait("Checkers:")
    fen = next(l for l in out if l.startswith("Fen:")).split("Fen: ")[1]
    in_check = len(out[-1].split()) > 1
    ref.send("go perft 1")
    legal = [l.split(":")[0] for l in ref.wait("Nodes searched")
             if ":" in l and len(l.split(":")[0]) == 4 and l.split(":")[0][1].isdigit()]
    return fen, in_check, legal


def adjudicate_repetition(history):
    """history: (position, side to move in check, side to move "w"/"b") for every
    position since the last capture or reveal, the current one last. Returns
    (score for Red, reason) when the current position occurs for the third time,
    else None. A side that gave check with each of its moves since the first
    occurrence loses (perpetual check); otherwise the game is drawn."""
    key = history[-1][0]
    seen = [i for i, h in enumerate(history) if h[0] == key]
    if len(seen) < 3:
        return None
    cycle = history[seen[0] + 1:]
    red_perpetual = all(chk for _, chk, side in cycle if side == "b")    # Red moved into these
    black_perpetual = all(chk for _, chk, side in cycle if side == "w")
    if red_perpetual != black_perpetual:
        return (0 if red_perpetual else 1), "perpetual check"
    return 0.5, "repetition"


def play_game(eng, ref, opening, deal_seed, nodes, maxply):
    """eng[0] plays Red. Returns (score for Red, reason, index of a failed engine or None)."""
    deal = deal_identities(deal_seed)
    for e in eng:
        e.send("ucinewgame"); e.send("isready"); e.wait("readyok")
    moves = []
    history = []  # (FEN without counters, side to move in check, side to move) since the last irreversible move
    for ply in range(maxply):
        red_to_move = ply % 2 == 0
        lose = 0 if red_to_move else 1  # score for Red if the side to move loses
        fen, in_check, legal = referee_state(ref, moves)
        fields = fen.split()
        if not legal:
            return lose, "no legal move", None
        if int(fields[3]) >= 120:
            return 0.5, "60-move rule", None

        history.append((" ".join(fields[:3]), in_check, fields[1]))
        verdict = adjudicate_repetition(history)
        if verdict is not None:
            return verdict + (None,)

        if ply < len(opening):
            bm = legal[opening[ply] % len(legal)]
        else:
            idx = ply % 2
            e = eng[idx]
            try:
                e.send("position startpos moves " + " ".join(moves))
                e.send(f"go nodes {nodes}")
                bm = e.wait("bestmove")[-1].split()[1]
            except EngineError:
                return lose, "crash/timeout", idx
            if bm not in legal:
                return lose, f"illegal move {bm}", None

        b = board_of(fen)
        mover, target = b.get(sq(bm[:2])), b.get(sq(bm[2:4]))
        tok = bm
        if mover in ("X", "x"):
            tok += deal[sq(bm[:2])]
        if target in ("X", "x"):
            tok += deal[sq(bm[2:4])]
        moves.append(tok)
        # A capture or a reveal cannot be undone: earlier positions cannot repeat.
        if target is not None or mover in ("X", "x"):
            history = []
    return 0.5, "max plies", None


def worker(args, jobs):
    paths = (args.engine_a, args.engine_b)
    a, b = Engine(paths[0], args.timeout), Engine(paths[1], args.timeout)
    ref = Engine(args.engine_b, args.timeout)
    try:
        for (opening, seed) in jobs:
            pair = []
            for a_red in (True, False):
                players = (a, b) if a_red else (b, a)
                r, reason, failed = play_game(players, ref, opening, seed, args.nodes, args.maxply)
                if failed is not None:
                    # Replace the engine that crashed or hung, for the next games.
                    dead = players[failed]
                    dead.p.kill()
                    fresh = Engine(dead.path, args.timeout)
                    if dead is a: a = fresh
                    else: b = fresh
                pair.append(r if a_red else 1 - r)
                with lock:
                    reasons[reason] = reasons.get(reason, 0) + 1
            with lock:
                res.extend(pair)
                pairs.append(sum(pair) / 2)
                if len(pairs) % 10 == 0:
                    report(final=False)
    finally:
        for e in (a, b, ref): e.quit()


def report(final):
    n = len(res)
    if not n: return
    w = sum(1 for r in res if r == 1); l = sum(1 for r in res if r == 0); d = n - w - l
    s = sum(pairs) / len(pairs)
    var = sum((p - s) ** 2 for p in pairs) / len(pairs)
    se = math.sqrt(var / len(pairs)) if len(pairs) > 1 else 0
    elo = lambda x: -400 * math.log10(1 / x - 1) if 0 < x < 1 else float("inf") * (1 if x else -1)
    lo = min(max(1e-6, s - 1.96 * se), 1 - 1e-6)
    hi = min(max(1e-6, s + 1.96 * se), 1 - 1e-6)
    why = ", ".join(f"{k} {v}" for k, v in sorted(reasons.items(), key=lambda kv: -kv[1]))
    print(f"{'FINAL ' if final else ''}games {n}  A: +{w} ={d} -{l}  score {s:.3f}  "
          f"elo {elo(s):+.1f} [{elo(lo):+.1f}, {elo(hi):+.1f}]  ({time.time() - t0:.0f}s)  [{why}]",
          flush=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("engine_a"); ap.add_argument("engine_b")
    ap.add_argument("--pairs", type=int, default=50)
    ap.add_argument("--nodes", type=int, default=20000)
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--book", type=int, default=4, help="random opening plies")
    ap.add_argument("--maxply", type=int, default=300)
    ap.add_argument("--timeout", type=float, default=60, help="seconds an engine may take to answer")
    args = ap.parse_args()
    if os.path.realpath(args.engine_a) == os.path.realpath(args.engine_b):
        print("note: A and B are the same binary; each pair replays one game, so the score is 50%",
              file=sys.stderr)
    rng = random.Random(args.seed)
    jobs = [([rng.randrange(1 << 30) for _ in range(args.book)], rng.randrange(1 << 30))
            for _ in range(args.pairs)]
    lock = threading.Lock(); res = []; pairs = []; reasons = {}; t0 = time.time()
    chunks = [jobs[i::args.jobs] for i in range(args.jobs)]
    with ThreadPoolExecutor(args.jobs) as ex:
        list(ex.map(lambda c: worker(args, c), chunks))
    report(final=True)
