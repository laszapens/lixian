#!/usr/bin/env python3
"""Self-play data generator for Lixian NNUE.

Each worker owns two Stockfish processes: one plays the game, one labels the
positions with a fixed-node search.  Positions are written in a 96-byte record
that stores the full board, so the C++ trainer can carry on without re-parsing
anything.

Record layout (little endian, 120 bytes incl. tail padding):
    u64 occ[2]      occupancy per side
    u64 piece[12]   bitboards per piece index (W_PAWN..B_KING, matching Piece)
    i16 cp          stockfish score from the side to move's view
    i8  result      +1 win / 0 draw / -1 loss for the side to move
    i8  turn        0 = white to move, 1 = black to move
    u8  padding[4]  keeps sizeof(struct Record) == 120 in C++
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import time
from multiprocessing import Process, Queue

import chess

HERE = os.path.dirname(os.path.abspath(__file__))
SF = os.path.join(HERE, "..", "sf", "stockfish", "stockfish-ubuntu-x86-64-avx2")
REC = struct.Struct("<14Qhbb")  # 14 bitboards + cp + result + turn
assert REC.size == 116, REC.size
PAD = 4
RECORD_SIZE = 120


class Engine:
    def __init__(self, threads=1, hash_mb=64):
        self.p = subprocess.Popen([SF], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)
        self.send("uci")
        self.wait_for("uciok")
        self.send(f"setoption name Threads value {threads}")
        self.send(f"setoption name Hash value {hash_mb}")
        self.ready()

    def send(self, s):
        self.p.stdin.write(s + "\n")
        self.p.stdin.flush()

    def wait_for(self, token):
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("stockfish died")
            if line.startswith(token):
                return line.strip()

    def ready(self):
        self.send("isready")
        self.wait_for("readyok")

    def bestmove(self, board, nodes):
        self.send("position fen " + board.fen())
        self.send(f"go nodes {nodes}")
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("stockfish died")
            if line.startswith("bestmove"):
                mv = line.split()[1]
                if mv == "(none)":
                    return None
                return chess.Move.from_uci(mv)

    def label(self, board, nodes):
        self.send("position fen " + board.fen())
        self.send(f"go nodes {nodes}")
        score = None
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("stockfish died")
            if line.startswith("info") and " score " in line:
                parts = line.split()
                try:
                    i = parts.index("score")
                except ValueError:
                    continue
                kind, val = parts[i + 1], parts[i + 2]
                if kind == "cp":
                    score = int(val)
                elif kind == "mate":
                    m = int(val)
                    score = 20000 - abs(m) * 10 if m > 0 else -(20000 - abs(m) * 10)
            if line.startswith("bestmove"):
                return score

    def close(self):
        try:
            self.send("quit")
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


PIECE_CODE = {
    (chess.PAWN, chess.WHITE): 0, (chess.KNIGHT, chess.WHITE): 1,
    (chess.BISHOP, chess.WHITE): 2, (chess.ROOK, chess.WHITE): 3,
    (chess.QUEEN, chess.WHITE): 4, (chess.KING, chess.WHITE): 5,
    (chess.PAWN, chess.BLACK): 6, (chess.KNIGHT, chess.BLACK): 7,
    (chess.BISHOP, chess.BLACK): 8, (chess.ROOK, chess.BLACK): 9,
    (chess.QUEEN, chess.BLACK): 10, (chess.KING, chess.BLACK): 11,
}


def encode(board, cp, result):
    turn = 0 if board.turn == chess.WHITE else 1
    occ = [int(board.occupied_co[chess.WHITE]), int(board.occupied_co[chess.BLACK])]
    pieces = [0] * 12
    for pt in (chess.PAWN, chess.KNIGHT, chess.BISHOP, chess.ROOK, chess.QUEEN, chess.KING):
        for color in (chess.WHITE, chess.BLACK):
            pieces[PIECE_CODE[(pt, color)]] = int(board.pieces(pt, color))
    cp = max(-30000, min(30000, int(cp)))
    return REC.pack(occ[0], occ[1], *pieces, cp, result, turn) + b"\0" * PAD


def load_openings(path):
    lines = []
    with open(path, encoding="utf-8") as f:
        for ln in f:
            parts = ln.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            moves, ok, b = [], True, chess.Board()
            for tok in parts[2].split():
                if tok.endswith(".") or tok[0].isdigit():
                    continue
                try:
                    m = b.parse_san(tok)
                except Exception:
                    ok = False
                    break
                moves.append(m)
                b.push(m)
            if ok and 4 <= len(moves) <= 14:
                lines.append(moves)
    return lines


def result_value(board):
    if board.is_checkmate():
        return -1 if board.turn == chess.WHITE else 1
    if (board.is_stalemate() or board.is_insufficient_material()
            or board.is_seventyfive_moves() or board.is_fivefold_repetition()):
        return 0
    return None


def worker(wid, openings, args, q):
    rng = random.Random(args.seed + wid * 7919)
    player = Engine(1, 64)
    labeler = Engine(1, 128)
    out = open(os.path.join(args.out, f"shard{wid:03d}.bin"), "wb")
    games = positions = 0
    last_report = time.time()
    try:
        while games < args.games:
            book = rng.choice(openings)
            board = chess.Board()
            trunc = rng.randint(max(4, len(book) - 4), len(book))
            for m in book[:trunc]:
                if m in board.legal_moves:
                    board.push(m)
            record_from = len(board.move_stack)
            plies = record_from
            stored = []
            while plies < args.max_plies:
                if result_value(board) is not None or board.is_game_over(claim_draw=False):
                    break
                moves = list(board.legal_moves)
                if plies >= record_from:
                    cp = labeler.label(board, args.label_nodes)
                    if cp is not None and abs(cp) <= args.cp_cap:
                        stored.append((board.copy(stack=False), cp))
                mv = player.bestmove(board, args.play_nodes + rng.randint(0, args.play_jitter))
                if mv is None or mv not in moves:
                    mv = rng.choice(moves)
                board.push(mv)
                plies += 1

            res = result_value(board)
            if res is None:
                diff = 0
                for pt, v in ((chess.PAWN, 1), (chess.KNIGHT, 3), (chess.BISHOP, 3),
                              (chess.ROOK, 5), (chess.QUEEN, 9)):
                    diff += v * (len(board.pieces(pt, chess.WHITE)) -
                                 len(board.pieces(pt, chess.BLACK)))
                res = (1 if diff > 0 else -1) if abs(diff) >= 5 else 0

            for b, cp in stored:
                side_result = res if b.turn == chess.WHITE else -res
                out.write(encode(b, cp, side_result))
                positions += 1
            games += 1
            if time.time() - last_report > 15:
                q.put((wid, games, positions))
                last_report = time.time()
                out.flush()
    finally:
        out.close()
        player.close()
        labeler.close()
        q.put((wid, games, positions))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "data"))
    ap.add_argument("--openings", default=os.path.join(HERE, "..", "eco"))
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--games", type=int, default=4000)
    ap.add_argument("--play-nodes", type=int, default=2000)
    ap.add_argument("--play-jitter", type=int, default=4000)
    ap.add_argument("--label-nodes", type=int, default=8000)
    ap.add_argument("--cp-cap", type=int, default=2500)
    ap.add_argument("--max-plies", type=int, default=260)
    ap.add_argument("--seed", type=int, default=12345)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    openings = []
    for f in sorted(os.listdir(args.openings)):
        if f.endswith(".tsv"):
            openings.extend(load_openings(os.path.join(args.openings, f)))
    if not openings:
        print("no openings found", file=sys.stderr)
        return 1
    print(f"openings: {len(openings)}", flush=True)

    q = Queue()
    procs = [Process(target=worker, args=(i, openings, args, q)) for i in range(args.workers)]
    for p in procs:
        p.start()
    finished = 0
    t0 = time.time()
    while finished < len(procs):
        try:
            wid, g, pos = q.get(timeout=10)
        except Exception:
            continue
        print(f"[{time.time()-t0:7.0f}s] worker {wid}: {g} games, {pos} pos", flush=True)
        if g >= args.games:
            finished += 1
    for p in procs:
        p.join()
    print("done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
