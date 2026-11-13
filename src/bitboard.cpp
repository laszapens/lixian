#include "bitboard.h"

#include <vector>
#include <cstring>
#include <cstdio>

#if defined(__BMI2__)
#include <immintrin.h>
#endif

Bitboard PawnAttacks[2][64];
Bitboard KnightAttacks[64];
Bitboard KingAttacks[64];

namespace {

Bitboard g_between[64][64];
Bitboard g_line[64][64];

struct SliderInit {
    Bitboard mask;
    int offset;
    Bitboard magic;
    int shift;
};

#if defined(__BMI2__)
SliderInit g_rookInit[64];
SliderInit g_bishopInit[64];
std::vector<Bitboard> g_rookTable;
std::vector<Bitboard> g_bishopTable;
#else
SliderInit g_rookInit[64];
SliderInit g_bishopInit[64];
std::vector<Bitboard> g_rookTable;
std::vector<Bitboard> g_bishopTable;
#endif

constexpr int ROOK_DIRS[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
constexpr int BISHOP_DIRS[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

Bitboard slowSliderAttacks(Square sq, Bitboard occ, const int dirs[4][2]) {
    Bitboard att = 0;
    const int f0 = fileOf(sq), r0 = rankOf(sq);
    for (int d = 0; d < 4; ++d) {
        int f = f0 + dirs[d][0];
        int r = r0 + dirs[d][1];
        while (f >= 0 && f < 8 && r >= 0 && r < 8) {
            const Square s = makeSquare(f, r);
            att |= squareBB(s);
            if (occ & squareBB(s)) break;
            f += dirs[d][0];
            r += dirs[d][1];
        }
    }
    return att;
}

Bitboard relevantMask(Square sq, const int dirs[4][2]) {
    Bitboard mask = 0;
    const int f0 = fileOf(sq), r0 = rankOf(sq);
    for (int d = 0; d < 4; ++d) {
        int f = f0 + dirs[d][0];
        int r = r0 + dirs[d][1];
        while (f >= 0 && f < 8 && r >= 0 && r < 8) {
            const int f2 = f + dirs[d][0];
            const int r2 = r + dirs[d][1];
            if (f2 < 0 || f2 >= 8 || r2 < 0 || r2 >= 8) break;
            mask |= squareBB(makeSquare(f, r));
            f = f2;
            r = r2;
        }
    }
    return mask;
}

[[maybe_unused]] uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void initSliders() {
    g_rookTable.clear();
    g_bishopTable.clear();
    g_rookTable.reserve(102400 + 64);
    g_bishopTable.reserve(5248 + 64);

#if defined(__BMI2__)
    for (int s = 0; s < 64; ++s) {
        const Square sq = Square(s);
        const Bitboard rmask = relevantMask(sq, ROOK_DIRS);
        const Bitboard bmask = relevantMask(sq, BISHOP_DIRS);
        g_rookInit[s].mask = rmask;
        g_rookInit[s].offset = int(g_rookTable.size());
        g_rookTable.resize(g_rookTable.size() + (size_t(1) << popcount(rmask)));
        Bitboard sub = 0;
        do {
            g_rookTable[g_rookInit[s].offset + int(_pext_u64(sub, rmask))] =
                slowSliderAttacks(sq, sub, ROOK_DIRS);
            sub = (sub - rmask) & rmask;
        } while (sub);

        g_bishopInit[s].mask = bmask;
        g_bishopInit[s].offset = int(g_bishopTable.size());
        g_bishopTable.resize(g_bishopTable.size() + (size_t(1) << popcount(bmask)));
        sub = 0;
        do {
            g_bishopTable[g_bishopInit[s].offset + int(_pext_u64(sub, bmask))] =
                slowSliderAttacks(sq, sub, BISHOP_DIRS);
            sub = (sub - bmask) & bmask;
        } while (sub);
    }
#else
    uint64_t rng = 0x123456789ABCDEFULL;
    for (int s = 0; s < 64; ++s) {
        const Square sq = Square(s);
        const Bitboard rmask = relevantMask(sq, ROOK_DIRS);
        const Bitboard bmask = relevantMask(sq, BISHOP_DIRS);

        auto build = [&](Bitboard mask, const int dirs[4][2], SliderInit& entry,
                         std::vector<Bitboard>& table) {
            const int bits = popcount(mask);
            const int size = 1 << bits;
            std::vector<Bitboard> occs(size), refs(size);
            Bitboard sub = 0;
            int i = 0;
            do {
                occs[i] = sub;
                refs[i] = slowSliderAttacks(sq, sub, dirs);
                ++i;
                sub = (sub - mask) & mask;
            } while (sub);

            std::vector<int> used(size, 0);
            std::vector<Bitboard> mapped(size, 0);
            int epoch = 0;
            while (true) {
                // Sparse random candidate.
                const Bitboard magic = splitmix64(rng) & splitmix64(rng) & splitmix64(rng);
                if (popcount((mask * magic) >> 56) < 6) continue;
                ++epoch;
                bool ok = true;
                for (int j = 0; j < size; ++j) {
                    const int idx = int((occs[j] * magic) >> (64 - bits));
                    if (used[idx] != epoch) {
                        used[idx] = epoch;
                        mapped[idx] = refs[j];
                    } else if (refs[j] != mapped[idx]) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) continue;

                entry.mask = mask;
                entry.magic = magic;
                entry.shift = 64 - bits;
                entry.offset = int(table.size());
                table.resize(table.size() + size);
                for (int j = 0; j < size; ++j)
                    table[entry.offset + int((occs[j] * magic) >> entry.shift)] = refs[j];
                return;
            }
        };

        g_rookInit[s].offset = int(g_rookTable.size());
        build(rmask, ROOK_DIRS, g_rookInit[s], g_rookTable);
        g_bishopInit[s].offset = int(g_bishopTable.size());
        build(bmask, BISHOP_DIRS, g_bishopInit[s], g_bishopTable);
    }
#endif
}

}  // namespace

Bitboard bishopAttacks(Square sq, Bitboard occupied) {
#if defined(__BMI2__)
    return g_bishopTable[g_bishopInit[sq].offset + int(_pext_u64(occupied, g_bishopInit[sq].mask))];
#else
    const SliderInit& e = g_bishopInit[sq];
    return g_bishopTable[e.offset + int(((occupied & e.mask) * e.magic) >> e.shift)];
#endif
}

Bitboard rookAttacks(Square sq, Bitboard occupied) {
#if defined(__BMI2__)
    return g_rookTable[g_rookInit[sq].offset + int(_pext_u64(occupied, g_rookInit[sq].mask))];
#else
    const SliderInit& e = g_rookInit[sq];
    return g_rookTable[e.offset + int(((occupied & e.mask) * e.magic) >> e.shift)];
#endif
}

Bitboard rayAttacks(Square sq, int dir, Bitboard occupied) {
    int df = 0, dr = 0;
    switch (dir) {
        case NORTH: dr = 1; break;
        case SOUTH: dr = -1; break;
        case EAST: df = 1; break;
        case WEST: df = -1; break;
        case NE: df = 1; dr = 1; break;
        case NW: df = -1; dr = 1; break;
        case SE: df = 1; dr = -1; break;
        case SW: df = -1; dr = -1; break;
        default: return 0;
    }
    Bitboard att = 0;
    int f = fileOf(sq) + df, r = rankOf(sq) + dr;
    while (f >= 0 && f < 8 && r >= 0 && r < 8) {
        const Square s = makeSquare(f, r);
        att |= squareBB(s);
        if (occupied & squareBB(s)) break;
        f += df;
        r += dr;
    }
    return att;
}

Bitboard betweenBB(Square a, Square b) {
    return g_between[a][b];
}

Bitboard lineBB(Square a, Square b) {
    return g_line[a][b];
}

void initBitboards() {
    // Pawn, knight and king attacks.
    for (int s = 0; s < 64; ++s) {
        const Square sq = Square(s);
        const int f = fileOf(sq), r = rankOf(sq);
        PawnAttacks[WHITE][s] = 0;
        PawnAttacks[BLACK][s] = 0;
        if (r < 7) {
            if (f > 0) PawnAttacks[WHITE][s] |= squareBB(makeSquare(f - 1, r + 1));
            if (f < 7) PawnAttacks[WHITE][s] |= squareBB(makeSquare(f + 1, r + 1));
        }
        if (r > 0) {
            if (f > 0) PawnAttacks[BLACK][s] |= squareBB(makeSquare(f - 1, r - 1));
            if (f < 7) PawnAttacks[BLACK][s] |= squareBB(makeSquare(f + 1, r - 1));
        }

        KnightAttacks[s] = 0;
        static const int kn[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2},
                                     {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
        for (auto& d : kn) {
            const int nf = f + d[0], nr = r + d[1];
            if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8)
                KnightAttacks[s] |= squareBB(makeSquare(nf, nr));
        }

        KingAttacks[s] = 0;
        for (int df = -1; df <= 1; ++df)
            for (int dr = -1; dr <= 1; ++dr) {
                if (df == 0 && dr == 0) continue;
                const int nf = f + df, nr = r + dr;
                if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8)
                    KingAttacks[s] |= squareBB(makeSquare(nf, nr));
            }
    }

    initSliders();

    for (int a = 0; a < 64; ++a) {
        for (int b = 0; b < 64; ++b) {
            g_between[a][b] = 0;
            g_line[a][b] = 0;
            if (a == b) continue;
            const Square sa = Square(a), sb = Square(b);
            const Bitboard ratt = rookAttacks(sa, 0);
            const Bitboard batt = bishopAttacks(sa, 0);
            Bitboard line = 0;
            if (ratt & squareBB(sb)) {
                line = ratt & rookAttacks(sb, 0);
            } else if (batt & squareBB(sb)) {
                line = batt & bishopAttacks(sb, 0);
            } else {
                continue;
            }
            g_line[a][b] = line | squareBB(sa) | squareBB(sb);
            g_between[a][b] = line & ~squareBB(sa) & ~squareBB(sb);
        }
    }
}
