#!/usr/bin/env python3
"""Find large centipawn losses in a PGN for one side, judged by Stockfish."""
import argparse
import chess
import chess.engine
import chess.pgn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pgn", required=True)
    ap.add_argument("--side", default="A")
    ap.add_argument("--engine", default="/tmp/chess_infra/stockfish/src/stockfish")
    ap.add_argument("--depth", type=int, default=12)
    ap.add_argument("--threshold", type=int, default=90)
    ap.add_argument("--max", type=int, default=25)
    ap.add_argument("--game", type=int, default=0, help="only this game index (1-based)")
    args = ap.parse_args()

    sf = chess.engine.SimpleEngine.popen_uci(args.engine)
    sf.configure({"Threads": 2, "Hash": 256})

    records = []
    game_no = 0
    with open(args.pgn, encoding="utf-8") as f:
        while True:
            game = chess.pgn.read_game(f)
            if game is None:
                break
            game_no += 1
            if args.game and game_no != args.game:
                continue
            white = game.headers.get("White", "")
            black = game.headers.get("Black", "")
            if white == args.side:
                lix_color = chess.WHITE
            elif black == args.side:
                lix_color = chess.BLACK
            else:
                continue
            board = game.board()
            node = game
            move_no = 0
            for move in game.mainline_moves():
                move_no += 1
                if board.turn == lix_color:
                    info = sf.analyse(board, chess.engine.Limit(depth=args.depth))
                    best_score = info["score"].pov(lix_color)
                    best_move = info.get("pv", [None])[0]
                    before = board.copy()
                    board.push(move)
                    after_info = sf.analyse(board, chess.engine.Limit(depth=args.depth))
                    actual_score = after_info["score"].pov(lix_color)
                    if best_score.is_mate() or actual_score.is_mate():
                        continue
                    bs = best_score.score()
                    if abs(bs) > 600:
                        continue
                    loss = bs - actual_score.score()
                    if loss >= args.threshold:
                        records.append({
                            "game": game_no,
                            "move_no": move_no,
                            "fen": before.fen(),
                            "played": move.uci(),
                            "best": best_move.uci() if best_move else "?",
                            "loss": loss,
                            "eval": actual_score,
                        })
                    continue
                board.push(move)
    sf.quit()

    records.sort(key=lambda r: -r["loss"])
    print(f"flagged {len(records)} moves (threshold {args.threshold} cp)")
    for r in records[: args.max]:
        print(f"g{r['game']:02d} move {r['move_no']:3d} loss {r['loss']:5d} "
              f"played {r['played']} best {r['best']} eval {r['eval']} fen {r['fen']}")


if __name__ == "__main__":
    main()
