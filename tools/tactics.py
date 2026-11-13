#!/usr/bin/env python3
"""Run an EPD tactical suite against a UCI engine."""
import argparse
import sys
import time

import chess
import chess.engine


def parse_epd(path):
    positions = []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(" bm ")
        if len(parts) != 2:
            continue
        fen_part, bm_part = parts
        bm_part = bm_part.split(";")[0].strip()
        # FEN may include operations before bm; strip trailing ops.
        fields = fen_part.split()
        fen = " ".join(fields[:6])
        try:
            board = chess.Board(fen)
        except Exception:
            continue
        bms = []
        for tok in bm_part.split():
            tok = tok.rstrip(";")
            m = None
            try:
                m = board.parse_san(tok)
            except Exception:
                try:
                    m = chess.Move.from_uci(tok)
                except Exception:
                    m = None
            if m is not None and m in board.legal_moves:
                bms.append(m)
        if bms:
            positions.append((fen, bms))
    return positions


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default="./lixian")
    ap.add_argument("--epd", default="/tmp/chess_infra/suites/wac.epd")
    ap.add_argument("--limit", default="0.5")
    ap.add_argument("--max", type=int, default=0)
    ap.add_argument("--option", action="append", default=[])
    ap.add_argument("--book", action="store_true")
    args = ap.parse_args()

    positions = parse_epd(args.epd)
    if args.max:
        positions = positions[: args.max]
    print(f"loaded {len(positions)} positions from {args.epd}")

    engine = chess.engine.SimpleEngine.popen_uci(args.engine)
    for opt in args.option:
        name, value = opt.split("=", 1)
        engine.configure({name: value})
    engine.configure({"Book": args.book})

    solved = 0
    start = time.time()
    try:
        for i, (fen, bms) in enumerate(positions, 1):
            board = chess.Board(fen)
            info = engine.analyse(board, chess.engine.Limit(time=float(args.limit)))
            pv = info.get("pv") or []
            move = pv[0] if pv else None
            ok = move in bms
            solved += ok
            if not ok:
                print(f"  miss {i}: {fen} -> {move} (expected {[str(m) for m in bms]})")
            if i % 25 == 0:
                print(f"{i}/{len(positions)} solved={solved} ({solved/i:.1%})", flush=True)
    finally:
        engine.quit()
    elapsed = time.time() - start
    print(f"RESULT {solved}/{len(positions)} = {solved/max(1,len(positions)):.1%} in {elapsed:.1f}s")


if __name__ == "__main__":
    main()
