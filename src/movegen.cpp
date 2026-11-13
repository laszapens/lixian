#include "movegen.h"

namespace {

inline void addPawnPromotions(MoveList& ml, Square from, Square to, bool capture) {
    const int base = capture ? PROMO_CAP_N : PROMO_N;
    ml.add(makeMove(from, to, base + 0));  // knight
    ml.add(makeMove(from, to, base + 1));  // bishop
    ml.add(makeMove(from, to, base + 2));  // rook
    ml.add(makeMove(from, to, base + 3));  // queen
}

inline void addPawnPromotionQueen(MoveList& ml, Square from, Square to, bool capture) {
    ml.add(makeMove(from, to, (capture ? PROMO_CAP_N : PROMO_N) + 3));
}

void generatePawnMoves(const Position& pos, MoveList& ml, bool capturesOnly) {
    const Color us = Color(pos.side);
    const Color them = Color(us ^ 1);
    const Bitboard pawns = pos.pieces(us, PAWN);
    const Bitboard empty = ~pos.occupied;
    const Bitboard enemies = pos.pieces(them);

    if (us == WHITE) {
        const Bitboard rank8 = BB::RANK_8;
        if (!capturesOnly) {
            Bitboard push = (pawns << 8) & empty;
            Bitboard promoPush = push & rank8;
            push &= ~rank8;
            while (push) {
                const Square to = popLsb(push);
                ml.add(makeMove(Square(to - 8), to, QUIET));
            }
            while (promoPush) {
                const Square to = popLsb(promoPush);
                addPawnPromotions(ml, Square(to - 8), to, false);
            }
            Bitboard dbl = ((pawns & BB::RANK_2) << 16) & empty & (empty << 8);
            while (dbl) {
                const Square to = popLsb(dbl);
                ml.add(makeMove(Square(to - 16), to, DOUBLE_PUSH));
            }
        }

        Bitboard capL = ((pawns & ~BB::FILE_A) << 7) & enemies;
        Bitboard capR = ((pawns & ~BB::FILE_H) << 9) & enemies;
        while (capL) {
            const Square to = popLsb(capL);
            const Square from = Square(to - 7);
            if (to >= 56)
                addPawnPromotions(ml, from, to, true);
            else
                ml.add(makeMove(from, to, CAPTURE));
        }
        while (capR) {
            const Square to = popLsb(capR);
            const Square from = Square(to - 9);
            if (to >= 56)
                addPawnPromotions(ml, from, to, true);
            else
                ml.add(makeMove(from, to, CAPTURE));
        }
        if (pos.ep != NO_SQ) {
            Bitboard epAttackers = PawnAttacks[BLACK][pos.ep] & pawns;
            while (epAttackers) {
                const Square from = popLsb(epAttackers);
                ml.add(makeMove(from, pos.ep, EP_CAPTURE));
            }
        }
    } else {
        const Bitboard rank1 = BB::RANK_1;
        if (!capturesOnly) {
            Bitboard push = (pawns >> 8) & empty;
            Bitboard promoPush = push & rank1;
            push &= ~rank1;
            while (push) {
                const Square to = popLsb(push);
                ml.add(makeMove(Square(to + 8), to, QUIET));
            }
            while (promoPush) {
                const Square to = popLsb(promoPush);
                addPawnPromotions(ml, Square(to + 8), to, false);
            }
            Bitboard dbl = ((pawns & BB::RANK_7) >> 16) & empty & (empty >> 8);
            while (dbl) {
                const Square to = popLsb(dbl);
                ml.add(makeMove(Square(to + 16), to, DOUBLE_PUSH));
            }
        }

        Bitboard capL = ((pawns & ~BB::FILE_A) >> 9) & enemies;
        Bitboard capR = ((pawns & ~BB::FILE_H) >> 7) & enemies;
        while (capL) {
            const Square to = popLsb(capL);
            const Square from = Square(to + 9);
            if (to <= 7)
                addPawnPromotions(ml, from, to, true);
            else
                ml.add(makeMove(from, to, CAPTURE));
        }
        while (capR) {
            const Square to = popLsb(capR);
            const Square from = Square(to + 7);
            if (to <= 7)
                addPawnPromotions(ml, from, to, true);
            else
                ml.add(makeMove(from, to, CAPTURE));
        }
        if (pos.ep != NO_SQ) {
            Bitboard epAttackers = PawnAttacks[WHITE][pos.ep] & pawns;
            while (epAttackers) {
                const Square from = popLsb(epAttackers);
                ml.add(makeMove(from, pos.ep, EP_CAPTURE));
            }
        }
    }
}

void generatePieceMoves(const Position& pos, MoveList& ml, PieceType pt,
                        bool capturesOnly) {
    const Color us = Color(pos.side);
    const Color them = Color(us ^ 1);
    const Bitboard own = pos.pieces(us);
    const Bitboard enemies = pos.pieces(them);
    const Bitboard targets = capturesOnly ? enemies : ~own;

    Bitboard pieces = pos.pieces(us, pt);
    while (pieces) {
        const Square from = popLsb(pieces);
        Bitboard attacks;
        switch (pt) {
            case KNIGHT: attacks = KnightAttacks[from]; break;
            case BISHOP: attacks = bishopAttacks(from, pos.occupied); break;
            case ROOK: attacks = rookAttacks(from, pos.occupied); break;
            case QUEEN: attacks = queenAttacks(from, pos.occupied); break;
            default: attacks = 0; break;
        }
        attacks &= targets;
        while (attacks) {
            const Square to = popLsb(attacks);
            ml.add(makeMove(from, to, (enemies & squareBB(to)) ? CAPTURE : QUIET));
        }
    }
}

void generateKingMoves(const Position& pos, MoveList& ml, bool capturesOnly) {
    const Color us = Color(pos.side);
    const Color them = Color(us ^ 1);
    const Square from = pos.kingSquare(us);
    const Bitboard own = pos.pieces(us);
    const Bitboard enemies = pos.pieces(them);
    Bitboard attacks = KingAttacks[from] & (capturesOnly ? enemies : ~own);
    while (attacks) {
        const Square to = popLsb(attacks);
        ml.add(makeMove(from, to, (enemies & squareBB(to)) ? CAPTURE : QUIET));
    }

    if (capturesOnly) return;

    // Castling is generated as fully legal: king not in check and path safe.
    const Square E1 = makeSquare(4, 0), F1 = makeSquare(5, 0), G1 = makeSquare(6, 0);
    const Square D1 = makeSquare(3, 0), C1 = makeSquare(2, 0), B1 = makeSquare(1, 0);
    const Square A1 = makeSquare(0, 0), H1 = makeSquare(7, 0);
    const Square E8 = makeSquare(4, 7), F8 = makeSquare(5, 7), G8 = makeSquare(6, 7);
    const Square D8 = makeSquare(3, 7), C8 = makeSquare(2, 7), B8 = makeSquare(1, 7);
    const Square A8 = makeSquare(0, 7), H8 = makeSquare(7, 7);

    if (us == WHITE) {
        if ((pos.castling & 1) && from == E1 &&
            pos.pieceOn(H1) == W_ROOK && pos.pieceOn(F1) == NO_PIECE &&
            pos.pieceOn(G1) == NO_PIECE &&
            !pos.isSquareAttacked(E1, them) && !pos.isSquareAttacked(F1, them) &&
            !pos.isSquareAttacked(G1, them)) {
            ml.add(makeMove(E1, G1, KING_CASTLE));
        }
        if ((pos.castling & 2) && from == E1 &&
            pos.pieceOn(A1) == W_ROOK && pos.pieceOn(D1) == NO_PIECE &&
            pos.pieceOn(C1) == NO_PIECE && pos.pieceOn(B1) == NO_PIECE &&
            !pos.isSquareAttacked(E1, them) && !pos.isSquareAttacked(D1, them) &&
            !pos.isSquareAttacked(C1, them)) {
            ml.add(makeMove(E1, C1, QUEEN_CASTLE));
        }
    } else {
        if ((pos.castling & 4) && from == E8 &&
            pos.pieceOn(H8) == B_ROOK && pos.pieceOn(F8) == NO_PIECE &&
            pos.pieceOn(G8) == NO_PIECE &&
            !pos.isSquareAttacked(E8, them) && !pos.isSquareAttacked(F8, them) &&
            !pos.isSquareAttacked(G8, them)) {
            ml.add(makeMove(E8, G8, KING_CASTLE));
        }
        if ((pos.castling & 8) && from == E8 &&
            pos.pieceOn(A8) == B_ROOK && pos.pieceOn(D8) == NO_PIECE &&
            pos.pieceOn(C8) == NO_PIECE && pos.pieceOn(B8) == NO_PIECE &&
            !pos.isSquareAttacked(E8, them) && !pos.isSquareAttacked(D8, them) &&
            !pos.isSquareAttacked(C8, them)) {
            ml.add(makeMove(E8, C8, QUEEN_CASTLE));
        }
    }
}

}  // namespace

void generateAll(const Position& pos, MoveList& ml) {
    ml.clear();
    generatePawnMoves(pos, ml, false);
    generatePieceMoves(pos, ml, KNIGHT, false);
    generatePieceMoves(pos, ml, BISHOP, false);
    generatePieceMoves(pos, ml, ROOK, false);
    generatePieceMoves(pos, ml, QUEEN, false);
    generateKingMoves(pos, ml, false);
}

void generateCaptures(const Position& pos, MoveList& ml) {
    ml.clear();
    generatePawnMoves(pos, ml, true);
    generatePieceMoves(pos, ml, KNIGHT, true);
    generatePieceMoves(pos, ml, BISHOP, true);
    generatePieceMoves(pos, ml, ROOK, true);
    generatePieceMoves(pos, ml, QUEEN, true);
    generateKingMoves(pos, ml, true);

    // Add quiet queen promotions: they are forcing and belong in qsearch.
    const Color us = Color(pos.side);
    const Bitboard pawns = pos.pieces(us, PAWN);
    const Bitboard empty = ~pos.occupied;
    if (us == WHITE) {
        Bitboard promo = (pawns << 8) & empty & BB::RANK_8;
        while (promo) {
            const Square to = popLsb(promo);
            addPawnPromotionQueen(ml, Square(to - 8), to, false);
        }
    } else {
        Bitboard promo = (pawns >> 8) & empty & BB::RANK_1;
        while (promo) {
            const Square to = popLsb(promo);
            addPawnPromotionQueen(ml, Square(to + 8), to, false);
        }
    }
}
