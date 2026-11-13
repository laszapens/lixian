// Lixian (离弦) - board representation and position state.
#pragma once

#include "bitboard.h"
#include "types.h"
#include <string>
#include <vector>
#include <ostream>

namespace Zobrist {
extern uint64_t psq[PIECE_NB][64];
extern uint64_t side;
extern uint64_t castling[16];
extern uint64_t epFile[8];
void init();
}  // namespace Zobrist

struct Undo {
    uint64_t key = 0;
    int castling = 0;
    Square ep = NO_SQ;
    int halfmove = 0;
    Piece captured = NO_PIECE;
    Square capturedSq = NO_SQ;
    Square kingSq[2] = {NO_SQ, NO_SQ};
};

class Position {
public:
    Bitboard byType[PT_NB] = {};
    Bitboard byColor[COLOR_NB] = {};
    Bitboard occupied = 0;
    Piece board[64] = {};
    int side = WHITE;
    int castling = 0;  // 1 = WK, 2 = WQ, 4 = BK, 8 = BQ
    Square ep = NO_SQ;
    int halfmove = 0;
    int fullmove = 1;
    uint64_t key = 0;
    Square kingSq[COLOR_NB] = {NO_SQ, NO_SQ};
    std::vector<uint64_t> repKeys;

    Position();

    void startpos();
    void setFen(const std::string& fen);
    std::string fen() const;
    void clear();
    bool isValid() const;

    // Board queries.
    Square kingSquare(Color c) const { return kingSq[c]; }
    Piece pieceOn(Square s) const { return board[s]; }
    PieceType typeOn(Square s) const { return typeOf(board[s]); }
    Color colorOn(Square s) const { return colorOf(board[s]); }
    bool isEmpty(Square s) const { return board[s] == NO_PIECE; }
    bool isOccupied(Square s) const { return board[s] != NO_PIECE; }
    Bitboard pieces(PieceType pt) const { return byType[pt]; }
    Bitboard pieces(Color c) const { return byColor[c]; }
    Bitboard pieces(Color c, PieceType pt) const { return byColor[c] & byType[pt]; }
    Bitboard pieces(PieceType a, PieceType b) const { return byType[a] | byType[b]; }
    Bitboard pieces(PieceType a, PieceType b, PieceType c) const {
        return byType[a] | byType[b] | byType[c];
    }
    int count(Color c, PieceType pt) const { return popcount(byColor[c] & byType[pt]); }

    bool isSquareAttacked(Square sq, Color by) const;
    bool inCheck(Color c) const { return isSquareAttacked(kingSq[c], Color(c ^ 1)); }
    Bitboard attackersTo(Square sq, Bitboard occ) const;

    Bitboard pawnAttacks(Color c) const;
    Bitboard attackedSquares(Color c) const;

    // Move handling.
    void makeMove(Move m, Undo& u);
    void unmakeMove(Move m, const Undo& u);
    void makeNullMove(Undo& u);
    void unmakeNullMove(const Undo& u);
    bool moveIsLegal(Move m) const;
    Move uciToMove(const std::string& text) const;

    bool isRepetition() const;
    bool isDraw() const;
    bool isInsufficientMaterial() const;
    bool hasNonPawnMaterial(Color c) const;
    int nonPawnMaterial(Color c) const;

    bool seeGE(Move m, int threshold) const;
    void print(std::ostream& os) const;

private:
    void addPiece(Square s, Piece p);
    void removePiece(Square s, Piece p);
    void movePiece(Square from, Square to, Piece p);
    uint64_t computeKey() const;
};
