// Lixian (离弦) - core types and constants.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <string>

using Bitboard = uint64_t;
using Square = int;
using Move = uint32_t;

constexpr Square NO_SQ = 64;
constexpr int MAX_PLY = 128;
constexpr int MAX_MOVES = 256;
constexpr int MAX_HISTORY = 20000;

constexpr int INF = 32000;
constexpr int MATE = 31000;
constexpr int MATE_THRESHOLD = MATE - 2 * MAX_PLY;
constexpr int VALUE_DRAW = 0;

enum Color : int { WHITE = 0, BLACK = 1, COLOR_NB = 2 };

enum PieceType : int {
    PAWN = 0, KNIGHT = 1, BISHOP = 2, ROOK = 3, QUEEN = 4, KING = 5,
    PT_NB = 6, NO_PT = 6
};

// Piece = color * 6 + type.
enum Piece : int {
    W_PAWN = 0, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
    B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
    PIECE_NB = 12, NO_PIECE = 12
};

constexpr Piece makePiece(Color c, PieceType pt) {
    return Piece(int(c) * 6 + int(pt));
}
constexpr Color colorOf(Piece p) { return Color(p / 6); }
constexpr PieceType typeOf(Piece p) { return PieceType(p % 6); }

constexpr Square makeSquare(int file, int rank) { return rank * 8 + file; }
constexpr int fileOf(Square s) { return s & 7; }
constexpr int rankOf(Square s) { return s >> 3; }
constexpr Square flipRank(Square s) { return s ^ 56; }
constexpr Square relativeSquare(Color c, Square s) { return c == WHITE ? s : flipRank(s); }
constexpr bool isValidSquare(Square s) { return s >= 0 && s < 64; }
constexpr bool isValidPiece(Piece p) { return p >= 0 && p < PIECE_NB; }

// Move flags. Keep values 0..13 so they fit in four bits.
enum MoveFlag : int {
    QUIET = 0,
    DOUBLE_PUSH = 1,
    KING_CASTLE = 2,
    QUEEN_CASTLE = 3,
    CAPTURE = 4,
    EP_CAPTURE = 5,
    PROMO_N = 8,
    PROMO_B = 9,
    PROMO_R = 10,
    PROMO_Q = 11,
    PROMO_CAP_N = 12,
    PROMO_CAP_B = 13,
    PROMO_CAP_R = 14,
    PROMO_CAP_Q = 15
};

constexpr Move makeMove(Square from, Square to, int flags) {
    return Move(from) | (Move(to) << 6) | (Move(flags) << 12);
}
constexpr Square fromSq(Move m) { return Square(m & 63); }
constexpr Square toSq(Move m) { return Square((m >> 6) & 63); }
constexpr int moveFlags(Move m) { return int((m >> 12) & 15); }
constexpr int moveScore(Move m) { return int(int32_t(m)) >> 16; }
constexpr Move withScore(Move m, int s) {
    return (m & 0xFFFFu) | (Move(uint16_t(int16_t(s))) << 16);
}
constexpr bool isCapture(Move m) {
    const int f = moveFlags(m);
    return f == CAPTURE || f == EP_CAPTURE || f >= PROMO_CAP_N;
}
constexpr bool isPromotion(Move m) { return moveFlags(m) >= PROMO_N; }
constexpr PieceType promotionType(Move m) {
    return PieceType(KNIGHT + (moveFlags(m) & 3));
}
constexpr bool isCastle(Move m) {
    const int f = moveFlags(m);
    return f == KING_CASTLE || f == QUEEN_CASTLE;
}
constexpr Move MOVE_NONE = 0;

inline int signOf(Color c) { return c == WHITE ? 1 : -1; }

// Mate scores are stored from the point of view of the side to move, adjusted by ply.
constexpr int mateIn(int ply) { return MATE - ply; }
constexpr int matedIn(int ply) { return -MATE + ply; }
inline bool isMateScore(int score) { return std::abs(score) >= MATE_THRESHOLD; }

inline int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::string squareToString(Square s);
Square parseSquare(const std::string& s);
std::string moveToString(Move m);

// A fixed-size move list with independent ordering scores.
struct MoveList {
    Move moves[MAX_MOVES];
    int scores[MAX_MOVES];
    int size = 0;

    void clear() { size = 0; }
    void add(Move m, int score = 0) {
        moves[size] = m;
        scores[size] = score;
        ++size;
    }
    bool empty() const { return size == 0; }

    // Selection sort step: bring the best remaining move to index i.
    void pickBest(int i) {
        int best = i;
        for (int j = i + 1; j < size; ++j)
            if (scores[j] > scores[best]) best = j;
        if (best != i) {
            std::swap(moves[i], moves[best]);
            std::swap(scores[i], scores[best]);
        }
    }
};
