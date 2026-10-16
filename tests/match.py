#!/usr/bin/env python3
"""Jieqi match runner: plays engine A against engine B through UCI.

The runner is the referee: it knows nothing the engines don't, except that it
draws the identity of every face-down piece when it moves (uniformly from that
side's pool, which has the same distribution as shuffling the identities at the
start) and, when a face-down piece is captured, tells both engines what it was
(as the capturer would see it). Games come in pairs with the same opening and
the same identity draws, colours swapped.

usage: match.py ENGINE_A ENGINE_B [--pairs N] [--nodes N] [--jobs N] [--seed S]
"""
import argparse, math, random, subprocess, sys, threading, time
from concurrent.futures import ThreadPoolExecutor

STARTPOOL = {"R": 2, "A": 2, "C": 2, "P": 5, "N": 2, "B": 2,
             "r": 2, "a": 2, "c": 2, "p": 5, "n": 2, "b": 2}


class Engine:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.send("uci"); self.wait("uciok")
        self.send("setoption name Hash value 8")
        self.send("isready"); self.wait("readyok")

    def send(self, s):
        self.p.stdin.write(s + "\n"); self.p.stdin.flush()

    def wait(self, prefix):
        lines = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("engine died")
            lines.append(line.rstrip("\n"))
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


def play_game(eng, ref, opening, rng_seed, nodes, maxply):
    """eng[0] plays Red. Returns 1 (Red wins), 0 (Black wins) or 0.5."""
    rng = random.Random(rng_seed)
    pool = dict(STARTPOOL)
    moves = []
    for e in eng:
        e.send("ucinewgame"); e.send("isready"); e.wait("readyok")
    for ply in range(maxply):
        ref.send("position startpos moves " + " ".join(moves))
        ref.send("d")
        fen = next(l for l in ref.wait("Checkers:") if l.startswith("Fen:")).split("Fen: ")[1]
        if ply < len(opening):
            ref.send("go perft 1")
            legal = [l.split(":")[0] for l in ref.wait("Nodes searched")
                     if len(l.split(":")[0]) == 4 and l.split(":")[0][1].isdigit()]
            if not legal:
                return 0 if ply % 2 == 0 else 1
            bm = legal[opening[ply] % len(legal)]
        else:
            e = eng[ply % 2]
            e.send("position startpos moves " + " ".join(moves))
            e.send(f"go nodes {nodes}")
            bm = e.wait("bestmove")[-1].split()[1]
        if bm == "(none)":
            return 0 if ply % 2 == 0 else 1
        b = board_of(fen)
        mover, target = b.get(sq(bm[:2])), b.get(sq(bm[2:4]))
        tok = bm
        if mover in ("X", "x"):
            red = mover == "X"
            cands = sorted(k for k, v in pool.items() if v > 0 and k.isupper() == red)
            ident = rng.choice(cands); pool[ident] -= 1
            tok += ident
        if target in ("X", "x"):
            red = target == "X"
            cands = sorted(k for k, v in pool.items() if v > 0 and k.isupper() == red)
            ident = rng.choice(cands); pool[ident] -= 1
            tok += ident
        moves.append(tok)
    return 0.5


def worker(args, jobs):
    a, b, ref = Engine(args.engine_a), Engine(args.engine_b), Engine(args.engine_b)
    out = []
    try:
        for (opening, seed) in jobs:
            r1 = play_game((a, b), ref, opening, seed, args.nodes, args.maxply)   # A red
            r2 = play_game((b, a), ref, opening, seed, args.nodes, args.maxply)   # A black
            out.append((r1, 1 - r2))
            with lock:
                done[0] += 1
                res.extend([r1, 1 - r2])
                if done[0] % 10 == 0:
                    report(final=False)
    finally:
        for e in (a, b, ref): e.quit()
    return out


def report(final):
    n = len(res)
    if not n: return
    w = sum(1 for r in res if r == 1); l = sum(1 for r in res if r == 0); d = n - w - l
    s = (w + d / 2) / n
    var = (w * (1 - s) ** 2 + l * s ** 2 + d * (0.5 - s) ** 2) / n
    se = math.sqrt(var / n) if n > 1 else 0
    elo = lambda x: -400 * math.log10(1 / x - 1) if 0 < x < 1 else float("inf") * (1 if x else -1)
    lo, hi = max(1e-6, s - 1.96 * se), min(1 - 1e-6, s + 1.96 * se)
    print(f"{'FINAL ' if final else ''}games {n}  A: +{w} ={d} -{l}  score {s:.3f}  "
          f"elo {elo(s):+.1f} [{elo(lo):+.1f}, {elo(hi):+.1f}]  ({time.time() - t0:.0f}s)", flush=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("engine_a"); ap.add_argument("engine_b")
    ap.add_argument("--pairs", type=int, default=50)
    ap.add_argument("--nodes", type=int, default=20000)
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--book", type=int, default=4, help="random opening plies")
    ap.add_argument("--maxply", type=int, default=300)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    jobs = [([rng.randrange(1 << 30) for _ in range(args.book)], rng.randrange(1 << 30))
            for _ in range(args.pairs)]
    lock = threading.Lock(); done = [0]; res = []; t0 = time.time()
    chunks = [jobs[i::args.jobs] for i in range(args.jobs)]
    with ThreadPoolExecutor(args.jobs) as ex:
        list(ex.map(lambda c: worker(args, c), chunks))
    report(final=True)
