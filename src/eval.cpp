#include "eval.h"

#include <algorithm>
#include <cmath>

namespace Eval {
int aggression = 140;
}

namespace {

constexpr int MatMg[PT_NB] = {100, 320, 330, 500, 900, 0};
constexpr int MatEg[PT_NB] = {120, 310, 330, 540, 950, 0};

constexpr int MobMg[PT_NB] = {0, 4, 5, 2, 1, 0};
constexpr int MobEg[PT_NB] = {0, 4, 5, 4, 2, 0};

constexpr int KingAttackWeight[PT_NB] = {0, 2, 2, 3, 5, 0};

// Piece-square tables are written in visual order: index 0 = a8 ... 63 = h1.
constexpr int PST_MG[PT_NB][64] = {
    {// Pawn
     0, 0, 0, 0, 0, 0, 0, 0,
     50, 50, 50, 50, 50, 50, 50, 50,
     10, 10, 20, 30, 30, 20, 10, 10,
     5, 5, 10, 25, 25, 10, 5, 5,
     0, 0, 0, 20, 20, 0, 0, 0,
     5, -5, -10, 0, 0, -10, -5, 5,
     5, 10, 10, -20, -20, 10, 10, 5,
     0, 0, 0, 0, 0, 0, 0, 0},
    {// Knight
     -50, -40, -30, -30, -30, -30, -40, -50,
     -40, -20, 0, 0, 0, 0, -20, -40,
     -30, 0, 10, 15, 15, 10, 0, -30,
     -30, 5, 15, 20, 20, 15, 5, -30,
     -30, 0, 15, 20, 20, 15, 0, -30,
     -30, 5, 10, 15, 15, 10, 5, -30,
     -40, -20, 0, 5, 5, 0, -20, -40,
     -50, -40, -30, -30, -30, -30, -40, -50},
    {// Bishop
     -20, -10, -10, -10, -10, -10, -10, -20,
     -10, 0, 0, 0, 0, 0, 0, -10,
     -10, 0, 5, 10, 10, 5, 0, -10,
     -10, 5, 5, 10, 10, 5, 5, -10,
     -10, 0, 10, 10, 10, 10, 0, -10,
     -10, 10, 10, 10, 10, 10, 10, -10,
     -10, 5, 0, 0, 0, 0, 5, -10,
     -20, -10, -10, -10, -10, -10, -10, -20},
    {// Rook
     0, 0, 0, 0, 0, 0, 0, 0,
     5, 10, 10, 10, 10, 10, 10, 5,
     -5, 0, 0, 0, 0, 0, 0, -5,
     -5, 0, 0, 0, 0, 0, 0, -5,
     -5, 0, 0, 0, 0, 0, 0, -5,
     -5, 0, 0, 0, 0, 0, 0, -5,
     -5, 0, 0, 0, 0, 0, 0, -5,
     0, 0, 0, 5, 5, 0, 0, 0},
    {// Queen
     -20, -10, -10, -5, -5, -10, -10, -20,
     -10, 0, 0, 0, 0, 0, 0, -10,
     -10, 0, 5, 5, 5, 5, 0, -10,
     -5, 0, 5, 5, 5, 5, 0, -5,
     0, 0, 5, 5, 5, 5, 0, -5,
     -10, 5, 5, 5, 5, 5, 0, -10,
     -10, 0, 5, 0, 0, 0, 0, -10,
     -20, -10, -10, -5, -5, -10, -10, -20},
    {// King
     -30, -40, -40, -50, -50, -40, -40, -30,
     -30, -40, -40, -50, -50, -40, -40, -30,
     -30, -40, -40, -50, -50, -40, -40, -30,
     -30, -40, -40, -50, -50, -40, -40, -30,
     -20, -30, -30, -40, -40, -30, -30, -20,
     -10, -20, -20, -20, -20, -20, -20, -10,
     20, 20, 0, 0, 0, 0, 20, 20,
     20, 30, 10, 0, 0, 10, 30, 20}};

constexpr int PST_EG[PT_NB][64] = {
    {// Pawn
     0, 0, 0, 0, 0, 0, 0, 0,
     90, 90, 90, 90, 90, 90, 90, 90,
     55, 55, 55, 55, 55, 55, 55, 55,
     30, 30, 30, 30, 30, 30, 30, 30,
     15, 15, 15, 15, 15, 15, 15, 15,
     5, 5, 5, 5, 5, 5, 5, 5,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0},
    {// Knight
     -40, -30, -20, -20, -20, -20, -30, -40,
     -30, -15, 0, 5, 5, 0, -15, -30,
     -20, 5, 15, 20, 20, 15, 5, -20,
     -20, 10, 20, 25, 25, 20, 10, -20,
     -20, 10, 20, 25, 25, 20, 10, -20,
     -20, 5, 15, 20, 20, 15, 5, -20,
     -30, -15, 0, 5, 5, 0, -15, -30,
     -40, -30, -20, -20, -20, -20, -30, -40},
    {// Bishop
     -15, -10, -5, -5, -5, -5, -10, -15,
     -10, 0, 5, 5, 5, 5, 0, -10,
     -5, 5, 10, 10, 10, 10, 5, -5,
     -5, 5, 10, 15, 15, 10, 5, -5,
     -5, 5, 10, 15, 15, 10, 5, -5,
     -5, 5, 10, 10, 10, 10, 5, -5,
     -10, 0, 5, 5, 5, 5, 0, -10,
     -15, -10, -5, -5, -5, -5, -10, -15},
    {// Rook
     0, 0, 0, 0, 0, 0, 0, 0,
     10, 15, 15, 15, 15, 15, 15, 10,
     5, 5, 5, 5, 5, 5, 5, 5,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0},
    {// Queen
     -20, -10, -10, -5, -5, -10, -10, -20,
     -10, 0, 0, 0, 0, 0, 0, -10,
     -10, 0, 5, 5, 5, 5, 0, -10,
     -5, 0, 5, 5, 5, 5, 0, -5,
     0, 0, 5, 5, 5, 5, 0, -5,
     -10, 5, 5, 5, 5, 5, 0, -10,
     -10, 0, 5, 0, 0, 0, 0, -10,
     -20, -10, -10, -5, -5, -10, -10, -20},
    {// King
     -50, -40, -30, -20, -20, -30, -40, -50,
     -30, -20, -10, 0, 0, -10, -20, -30,
     -30, -10, 20, 30, 30, 20, -10, -30,
     -30, -10, 30, 40, 40, 30, -10, -30,
     -30, -10, 30, 40, 40, 30, -10, -30,
     -30, -10, 20, 30, 30, 20, -10, -30,
     -30, -30, 0, 0, 0, 0, -30, -30,
     -50, -30, -30, -30, -30, -30, -30, -50}};

constexpr int PassedMg[8] = {0, 0, 8, 18, 32, 55, 90, 0};
constexpr int PassedEg[8] = {0, 4, 12, 24, 45, 80, 130, 0};

inline int pstIndex(Color c, Square s) { return c == WHITE ? (s ^ 56) : s; }
inline int chebyshev(Square a, Square b) {
    return std::max(std::abs(fileOf(a) - fileOf(b)), std::abs(rankOf(a) - rankOf(b)));
}

inline Bitboard adjacentFiles(int file) {
    Bitboard b = 0;
    if (file > 0) b |= BB::FILE_A << (file - 1);
    if (file < 7) b |= BB::FILE_A << (file + 1);
    return b;
}

inline Bitboard pawnFrontSpan(Color c, Square s) {
    Bitboard span = 0;
    const int f = fileOf(s);
    const int start = rankOf(s) + (c == WHITE ? 1 : -1);
    const int step = (c == WHITE ? 1 : -1);
    for (int r = start; r >= 0 && r < 8; r += step) {
        for (int df = -1; df <= 1; ++df) {
            const int nf = f + df;
            if (nf >= 0 && nf < 8) span |= squareBB(makeSquare(nf, r));
        }
    }
    return span;
}

Bitboard g_adjacent[8];
Bitboard g_frontSpan[2][64];

struct EvalTables {
    EvalTables() {
        for (int f = 0; f < 8; ++f) {
            g_adjacent[f] = 0;
            if (f > 0) g_adjacent[f] |= BB::FILE_A << (f - 1);
            if (f < 7) g_adjacent[f] |= BB::FILE_A << (f + 1);
        }
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < 64; ++s) {
                Bitboard span = 0;
                const int step = (c == WHITE ? 1 : -1);
                const int f = fileOf(s);
                for (int r = rankOf(s) + step; r >= 0 && r < 8; r += step)
                    for (int df = -1; df <= 1; ++df) {
                        const int nf = f + df;
                        if (nf >= 0 && nf < 8) span |= squareBB(makeSquare(nf, r));
                    }
                g_frontSpan[c][s] = span;
            }
    }
};
const EvalTables g_evalTables;

inline int kingDanger(int units, int attackers) {
    int d = units * units / 16 + attackers * 6;
    return std::min(d, 650);
}

}  // namespace

int Eval::evaluate(const Position& pos) {
    int mg = 0, eg = 0;
    int materialW = 0, materialB = 0;
    const Color us = Color(pos.side);

    Bitboard pawnsBB[2] = {pos.pieces(WHITE, PAWN), pos.pieces(BLACK, PAWN)};
    Bitboard pawnFiles[2][8] = {};
    int pawnFileCount[2][8] = {};
    for (int c = 0; c < 2; ++c) {
        Bitboard b = pawnsBB[c];
        while (b) {
            const Square s = popLsb(b);
            pawnFiles[c][fileOf(s)] |= squareBB(s);
            ++pawnFileCount[c][fileOf(s)];
        }
    }

    Bitboard pawnAtt[2] = {shiftNE(pawnsBB[0]) | shiftNW(pawnsBB[0]),
                           shiftSE(pawnsBB[1]) | shiftSW(pawnsBB[1])};
    Bitboard kingRing[2] = {KingAttacks[pos.kingSquare(WHITE)] | squareBB(pos.kingSquare(WHITE)),
                            KingAttacks[pos.kingSquare(BLACK)] | squareBB(pos.kingSquare(BLACK))};
    Bitboard mobilityArea[2];
    for (int c = 0; c < 2; ++c) {
        const Bitboard ownKing = squareBB(pos.kingSquare(Color(c)));
        mobilityArea[c] = ~(pawnsBB[c] | ownKing | pawnAtt[c ^ 1]);
    }

    int attackUnits[2] = {0, 0};
    int attackCount[2] = {0, 0};
    int attackWeight[2] = {0, 0};

    // Pawn attacks into the enemy king ring.
    for (int c = 0; c < 2; ++c) {
        const Bitboard intoRing = pawnAtt[c] & kingRing[c ^ 1];
        if (intoRing) {
            attackUnits[c] += popcount(intoRing);
            attackCount[c] += 1;
            attackWeight[c] += 1;
        }
    }

    // Material, piece-square tables, mobility, king-attack pressure.
    for (int c = 0; c < 2; ++c) {
        const Color col = Color(c);
        const int sign = (c == WHITE ? 1 : -1);
        for (int pt = PAWN; pt <= KING; ++pt) {
            Bitboard b = pos.pieces(col, PieceType(pt));
            while (b) {
                const Square s = popLsb(b);
                const int idx = pstIndex(col, s);
                mg += sign * (MatMg[pt] + PST_MG[pt][idx]);
                eg += sign * (MatEg[pt] + PST_EG[pt][idx]);
                if (c == WHITE) materialW += MatMg[pt]; else materialB += MatMg[pt];

                if (pt == KNIGHT || pt == BISHOP || pt == ROOK || pt == QUEEN) {
                    Bitboard att;
                    switch (pt) {
                        case KNIGHT: att = KnightAttacks[s]; break;
                        case BISHOP: att = bishopAttacks(s, pos.occupied); break;
                        case ROOK: att = rookAttacks(s, pos.occupied); break;
                        default: att = queenAttacks(s, pos.occupied); break;
                    }
                    att &= ~pos.pieces(col);

                    const int mob = popcount(att & mobilityArea[c]);
                    mg += sign * MobMg[pt] * mob;
                    eg += sign * MobEg[pt] * mob;

                    const Bitboard intoRing = att & kingRing[c ^ 1];
                    if (intoRing) {
                        const int cnt = popcount(intoRing);
                        attackUnits[c] += KingAttackWeight[pt] * cnt;
                        attackCount[c] += 1;
                        attackWeight[c] += KingAttackWeight[pt];
                    }

                    // Loose-piece pressure. A piece that is attacked and not
                    // defended by a pawn is a real target, not just a nuisance.
                    Bitboard targets = att & pos.pieces(Color(c ^ 1));
                    while (targets) {
                        const Square ts = popLsb(targets);
                        if (pawnAtt[c ^ 1] & squareBB(ts)) continue;
                        const PieceType tpt = typeOf(pos.pieceOn(ts));
                        const int bonus = 6 + MatMg[tpt] / 32;
                        mg += sign * bonus;
                        eg += sign * bonus / 2;
                    }
                }
            }
        }
    }

    // Pawn threats against pieces.
    for (int c = 0; c < 2; ++c) {
        const int sign = (c == WHITE ? 1 : -1);
        const Bitboard targets = pawnAtt[c] & pos.pieces(Color(c ^ 1)) &
                                 ~pos.pieces(Color(c ^ 1), PAWN);
        const int bonus = 8 * popcount(targets);
        mg += sign * bonus;
        eg += sign * bonus / 2;
    }

    // Pawn-structure terms.
    for (int c = 0; c < 2; ++c) {
        const Color col = Color(c);
        const Color opp = Color(c ^ 1);
        const int sign = (c == WHITE ? 1 : -1);
        const Bitboard ownPawns = pawnsBB[c];
        const Bitboard enemyPawns = pawnsBB[c ^ 1];
        Bitboard b = ownPawns;
        int islands = 0;
        for (int f = 0; f < 8; ++f)
            if (pawnFileCount[c][f] && (f == 0 || !pawnFileCount[c][f - 1])) ++islands;
        if (islands > 1) {
            mg -= sign * 8 * (islands - 1);
            eg -= sign * 6 * (islands - 1);
        }

        while (b) {
            const Square s = popLsb(b);
            const int f = fileOf(s);
            const int relRank = (c == WHITE ? rankOf(s) : 7 - rankOf(s));
            const Bitboard adj = g_adjacent[f];

            if (!(ownPawns & adj)) {
                mg -= sign * 12;
                eg -= sign * 8;
            }
            if (pawnFileCount[c][f] > 1) {
                mg -= sign * 8;
                eg -= sign * 14;
            }
            const Bitboard support = PawnAttacks[opp][s] & ownPawns;
            if (support) {
                mg += sign * 4;
                eg += sign * 3;
            }
            if (ownPawns & adj & (BB::RANK_1 << (rankOf(s) * 8))) {
                mg += sign * 4;
                eg += sign * 3;
            }

            const Bitboard front = g_frontSpan[c][s];
            const bool passed = !(front & enemyPawns);
            if (passed) {
                mg += sign * PassedMg[relRank];
                eg += sign * PassedEg[relRank];

                const Square promo = makeSquare(f, c == WHITE ? 7 : 0);
                const int ownDist = chebyshev(pos.kingSquare(col), promo);
                const int oppDist = chebyshev(pos.kingSquare(opp), promo);
                eg += sign * (oppDist - ownDist) * 5;

                const Square ahead = Square(s + (c == WHITE ? 8 : -8));
                if (ahead >= 0 && ahead < 64 && pos.isOccupied(ahead)) {
                    mg -= sign * 15;
                    eg -= sign * 25;
                }
                if (support) {
                    mg += sign * 8;
                    eg += sign * 12;
                }
            } else {
                // Candidate passer: no enemy pawns in front on adjacent files.
                if (!(front & enemyPawns & ~(g_adjacent[f])) && support) {
                    mg += sign * 6;
                    eg += sign * 10;
                }
            }

            // Backward pawn: no friendly support on adjacent files and the
            // stop square is controlled by an enemy pawn.
            const Square stop = Square(s + (c == WHITE ? 8 : -8));
            if (stop >= 0 && stop < 64 && !(ownPawns & adj) &&
                (pawnAtt[c ^ 1] & squareBB(stop))) {
                mg -= sign * 8;
                eg -= sign * 5;
            }
        }
    }

    // Rook files, bishop pair, outposts, bad bishops.
    for (int c = 0; c < 2; ++c) {
        const Color col = Color(c);
        const Color opp = Color(c ^ 1);
        const int sign = (c == WHITE ? 1 : -1);
        const Bitboard ownPawns = pawnsBB[c];
        const Bitboard enemyPawns = pawnsBB[c ^ 1];
        const Bitboard allPawns = ownPawns | enemyPawns;

        Bitboard rooks = pos.pieces(col, ROOK);
        while (rooks) {
            const Square s = popLsb(rooks);
            const int f = fileOf(s);
            if (!(allPawns & (BB::FILE_A << f))) {
                mg += sign * 22;
                eg += sign * 10;
            } else if (!(ownPawns & (BB::FILE_A << f))) {
                mg += sign * 10;
                eg += sign * 5;
            }
            if ((c == WHITE ? rankOf(s) : 7 - rankOf(s)) == 6) {
                mg += sign * 16;
                eg += sign * 10;
            }
        }
        Bitboard rooks2 = pos.pieces(col, ROOK);
        while (rooks2) {
            const Square s1 = popLsb(rooks2);
            Bitboard rooks3 = rooks2;
            while (rooks3) {
                const Square s2 = popLsb(rooks3);
                if (rookAttacks(s1, pos.occupied) & squareBB(s2)) {
                    mg += sign * 8;
                    eg += sign * 5;
                }
            }
        }

        if (popcount(pos.pieces(col, BISHOP)) >= 2) {
            mg += sign * 28;
            eg += sign * 42;
        }

        Bitboard bishops = pos.pieces(col, BISHOP);
        while (bishops) {
            const Square s = popLsb(bishops);
            const Bitboard colorMask =
                (squareBB(s) & BB::DARK_SQUARES) ? BB::DARK_SQUARES : BB::LIGHT_SQUARES;
            const Bitboard sameColorPawns = ownPawns & colorMask;
            mg -= sign * 3 * popcount(sameColorPawns);
            eg -= sign * 2 * popcount(sameColorPawns);
        }

        Bitboard knights = pos.pieces(col, KNIGHT);
        while (knights) {
            const Square s = popLsb(knights);
            const int relRank = (c == WHITE ? rankOf(s) : 7 - rankOf(s));
            if (relRank >= 3 && relRank <= 5 && (PawnAttacks[opp][s] & ownPawns) &&
                !(g_frontSpan[c][s] & enemyPawns)) {
                mg += sign * 22;
                eg += sign * 12;
            }
        }
    }

    // King shelter and pawn storm (middlegame only).
    for (int c = 0; c < 2; ++c) {
        const Color col = Color(c);
        const int sign = (c == WHITE ? 1 : -1);
        const Square ks = pos.kingSquare(col);
        const int kf = fileOf(ks);
        const int kr = rankOf(ks);
        const int dir = (c == WHITE ? 1 : -1);
        const int relRank = (c == WHITE ? kr : 7 - kr);
        if (relRank <= 1) {
            int shield = 0;
            for (int df = -1; df <= 1; ++df) {
                const int f = kf + df;
                if (f < 0 || f > 7) continue;
                int dist = -1;
                for (int d = 1; d <= 3; ++d) {
                    const int r = kr + dir * d;
                    if (r < 0 || r > 7) break;
                    if (pos.pieces(col, PAWN) & squareBB(makeSquare(f, r))) {
                        dist = d;
                        break;
                    }
                }
                if (dist < 0)
                    shield -= 18;
                else
                    shield += (4 - dist) * 8;

                const Bitboard fileMask = BB::FILE_A << f;
                if (!(pawnsBB[c] & fileMask) && !(pawnsBB[c ^ 1] & fileMask))
                    shield -= 8;

                Bitboard storm = pawnsBB[c ^ 1] & (BB::FILE_A << f);
                while (storm) {
                    const Square ps = popLsb(storm);
                    const int pr = rankOf(ps);
                    const int advance = (c == WHITE ? pr - kr : kr - pr);
                    if (advance > 0 && advance <= 3) shield -= 12;
                }
            }
            mg += sign * shield;
        }
    }

    // King danger: scaled asymmetrically by the aggression setting.
    const int aggr = clampInt(Eval::aggression, 50, 250);
    const int defScale = clampInt(100 - (aggr - 100) / 2, 40, 130);
    const int dangerWhite = kingDanger(attackUnits[BLACK], attackCount[BLACK]);
    const int dangerBlack = kingDanger(attackUnits[WHITE], attackCount[WHITE]);
    mg -= dangerWhite * defScale / 100;
    mg += dangerBlack * aggr / 100;

    // Initiative compensation: being down material while the enemy king is
    // under heavy fire is worth something. This is what makes sacrifices look
    // like investments rather than blunders.
    const int matDiff = materialW - materialB;
    const int whiteAttack = kingDanger(attackUnits[WHITE], attackCount[WHITE]) * aggr / 100;
    const int blackAttack = kingDanger(attackUnits[BLACK], attackCount[BLACK]) * aggr / 100;
    if (matDiff < 0)
        mg += std::min(-matDiff / 4, whiteAttack / 2) * aggr / 100;
    else if (matDiff > 0)
        mg -= std::min(matDiff / 4, blackAttack / 2) * aggr / 100;

    // Mop-up term: in a won bare-king ending, drive the defender to the edge
    // and bring the attacking king closer.
    int phase = 4 * popcount(pos.pieces(QUEEN)) + 2 * popcount(pos.pieces(ROOK)) +
                popcount(pos.pieces(KNIGHT)) + popcount(pos.pieces(BISHOP));
    if (phase <= 8 && std::abs(matDiff) >= 350) {
        const Color strong = (matDiff > 0 ? WHITE : BLACK);
        const Color weak = Color(strong ^ 1);
        if (pos.nonPawnMaterial(weak) <= 1) {
            const Square wk = pos.kingSquare(weak);
            const Square sk = pos.kingSquare(strong);
            const int edge = std::max(std::abs(2 * fileOf(wk) - 7),
                                      std::abs(2 * rankOf(wk) - 7));
            const int prox = 14 - chebyshev(sk, wk);
            const int bonus = edge * 9 + prox * 6;
            if (strong == WHITE)
                eg += bonus;
            else
                eg -= bonus;
        }
    }

    phase = clampInt(phase, 0, 24);
    int score = (mg * phase + eg * (24 - phase)) / 24;

    const int tempo = 12;
    score += (us == WHITE ? tempo : -tempo);
    return (us == WHITE ? score : -score);
}
