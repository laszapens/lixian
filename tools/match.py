#!/usr/bin/env python3
"""Parallel UCI match runner with opening diversity and Elo estimation."""
import argparse
import math
import os
import random
import re
import sys
import time
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed

import chess
import chess.engine
import chess.pgn

OPENING_DIR = "/tmp/chess_infra/openings"


def load_openings(limit=60, plies=8):
    openings = []
    if os.path.isdir(OPENING_DIR):
        for fn in sorted(os.listdir(OPENING_DIR)):
            if not fn.endswith(".tsv"):
                continue
            for line in open(os.path.join(OPENING_DIR, fn), encoding="utf-8"):
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 3:
                    continue
                pgn = re.sub(r"\d+\.(\.\.)?", " ", parts[2])
                tokens = [t for t in pgn.split() if t not in ("1-0", "0-1", "1/2-1/2", "*")]
                board = chess.Board()
                ok = True
                try:
                    for san in tokens[:plies]:
                        board.push_san(san)
                except Exception:
                    ok = False
                if ok and board.ply() >= 4:
                    openings.append(board.fen())
    if not openings:
        openings = [chess.Board().fen()]
    # De-duplicate positions and keep the list manageable.
    openings = list(dict.fromkeys(openings))
    random.shuffle(openings)
    return openings[:limit]


def elo_from_score(score):
    score = min(max(score, 1e-6), 1 - 1e-6)
    return -400.0 * math.log10(1.0 / score - 1.0)


def score_error(score, n):
    # Standard error of the score, then delta-method Elo error.
    se = math.sqrt(max(score * (1 - score), 1e-9) / max(n, 1))
    d = 400.0 / (math.log(10) * max(score * (1 - score), 1e-6))
    return se * d


def play_game(a_path, b_path, a_opts, b_opts, fen, tc_ms, inc_ms, max_plies=400, a_white=True, movetime_ms=0):
    board = chess.Board(fen)
    engines = {}
    try:
        engines[chess.WHITE] = chess.engine.SimpleEngine.popen_uci(
            a_path if a_white else b_path
        )
        engines[chess.BLACK] = chess.engine.SimpleEngine.popen_uci(
            b_path if a_white else a_path
        )
        engines[chess.WHITE].configure(a_opts if a_white else b_opts)
        engines[chess.BLACK].configure(b_opts if a_white else a_opts)
        clocks = {chess.WHITE: tc_ms, chess.BLACK: tc_ms}
        kind, winner = "end", None
        while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
            color = board.turn
            if movetime_ms > 0:
                limit = chess.engine.Limit(time=movetime_ms / 1000.0)
            else:
                limit = chess.engine.Limit(
                    white_clock=clocks[chess.WHITE] / 1000.0,
                    black_clock=clocks[chess.BLACK] / 1000.0,
                    white_inc=inc_ms / 1000.0,
                    black_inc=inc_ms / 1000.0,
                )
            t0 = time.time()
            result = engines[color].play(board, limit, game=board)
            elapsed = int((time.time() - t0) * 1000)
            if result.move is None:
                break
            if board.is_legal(result.move):
                board.push(result.move)
            else:
                return {"kind": "illegal", "fen": board.fen(), "winner": None,
                        "plies": board.ply(), "pgn": ""}
            if movetime_ms <= 0:
                clocks[color] = max(0, clocks[color] - elapsed + inc_ms)
                if clocks[color] <= 0:
                    # Time forfeit: the mover loses.
                    kind, winner = "time", (chess.BLACK if color == chess.WHITE else chess.WHITE)
                    break
        outcome = board.outcome(claim_draw=True)
        if outcome is not None:
            winner = outcome.winner
            kind = "draw" if winner is None else "win"
        elif kind != "time":
            kind, winner = "draw", None
        game = chess.pgn.Game.from_board(board)
        game.headers["Event"] = "Lixian test match"
        game.headers["White"] = "A" if a_white else "B"
        game.headers["Black"] = "B" if a_white else "A"
        game.headers["Result"] = "1/2-1/2" if winner is None else ("1-0" if winner == chess.WHITE else "0-1")
        return {"kind": kind, "fen": board.fen(), "winner": winner, "plies": board.ply(),
                "pgn": str(game)}
    finally:
        for e in engines.values():
            try:
                e.quit()
            except Exception:
                pass


def game_result_for_a(result, a_white):
    kind = result["kind"]
    winner = result["winner"]
    if kind == "illegal":
        return 0.0
    if kind == "time":
        a_color = chess.WHITE if a_white else chess.BLACK
        return 0.0 if winner != a_color else 1.0
    if winner is None:
        return 0.5
    a_color = chess.WHITE if a_white else chess.BLACK
    return 1.0 if winner == a_color else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="./lixian")
    ap.add_argument("--b", default="/tmp/chess_infra/stockfish/src/stockfish")
    ap.add_argument("--games", type=int, default=20)
    ap.add_argument("--tc", type=int, default=300, help="base time in ms")
    ap.add_argument("--inc", type=int, default=10, help="increment in ms")
    ap.add_argument("--concurrency", type=int, default=2)
    ap.add_argument("--openings", type=int, default=30)
    ap.add_argument("--a-option", action="append", default=[])
    ap.add_argument("--b-option", action="append", default=[])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--movetime", type=int, default=0, help="fixed ms per move instead of clocks")
    ap.add_argument("--pgn", default="")
    args = ap.parse_args()

    random.seed(args.seed)
    openings = load_openings(args.openings)
    a_opts = {}
    b_opts = {}
    for opt in args.a_option:
        k, v = opt.split("=", 1)
        a_opts[k] = v
    for opt in args.b_option:
        k, v = opt.split("=", 1)
        b_opts[k] = v

    jobs = []
    for g in range(args.games):
        fen = openings[g % len(openings)]
        a_white = (g % 2 == 0)
        jobs.append((fen, a_white, g + 1))

    print(f"match: A={args.a} B={args.b} games={args.games} tc={args.tc}+{args.inc}ms "
          f"concurrency={args.concurrency} openings={len(openings)}")
    results = []
    pgn_lock = threading.Lock()
    start = time.time()
    with ThreadPoolExecutor(max_workers=args.concurrency) as ex:
        futs = {
            ex.submit(
                play_game, args.a, args.b, a_opts, b_opts, fen, args.tc, args.inc,
                400, a_white, args.movetime
            ): (fen, a_white, num)
            for fen, a_white, num in jobs
        }
        for fut in as_completed(futs):
            fen, a_white, num = futs[fut]
            try:
                res = fut.result()
            except Exception as e:
                print(f"game {num}: ERROR {type(e).__name__}: {e}")
                res = {"kind": "illegal", "fen": fen, "winner": None, "plies": 0, "pgn": ""}
            score = game_result_for_a(res, a_white)
            results.append(score)
            if args.pgn:
                with pgn_lock:
                    with open(args.pgn, "a", encoding="utf-8") as pf:
                        pf.write(res["pgn"] + "\n\n")
            print(
                f"game {num:3d} A{'W' if a_white else 'B'} {str(res.get("kind","?")):7s} score={score} "
                f"plies={res.get('plies', '?')} [{len(results)}/{args.games}]",
                flush=True,
            )
    n = len(results)
    total = sum(results)
    score = total / max(n, 1)
    elo = elo_from_score(score)
    err = score_error(score, n)
    print()
    print(f"RESULT A {total:.1f}/{n} = {score:.3f}  Elo {elo:+.0f} +/- {err:.0f} (1 sigma)")
    print(f"wall {time.time()-start:.1f}s")


if __name__ == "__main__":
    main()
