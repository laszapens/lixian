// Lixian (离弦) - bitboards and attack tables.
#pragma once

#include "types.h"

namespace BB {
constexpr Bitboard EMPTY = 0ULL;
constexpr Bitboard ALL = ~0ULL;
constexpr Bitboard FILE_A = 0x0101010101010101ULL;
constexpr Bitboard FILE_B = FILE_A << 1;
constexpr Bitboard FILE_C = FILE_A << 2;
constexpr Bitboard FILE_D = FILE_A << 3;
constexpr Bitboard FILE_E = FILE_A << 4;
constexpr Bitboard FILE_F = FILE_A << 5;
constexpr Bitboard FILE_G = FILE_A << 6;
constexpr Bitboard FILE_H = FILE_A << 7;
constexpr Bitboard RANK_1 = 0xFFULL;
constexpr Bitboard RANK_2 = RANK_1 << 8;
constexpr Bitboard RANK_3 = RANK_1 << 16;
constexpr Bitboard RANK_4 = RANK_1 << 24;
constexpr Bitboard RANK_5 = RANK_1 << 32;
constexpr Bitboard RANK_6 = RANK_1 << 40;
constexpr Bitboard RANK_7 = RANK_1 << 48;
constexpr Bitboard RANK_8 = RANK_1 << 56;
constexpr Bitboard DARK_SQUARES = 0xAA55AA55AA55AA55ULL;
constexpr Bitboard LIGHT_SQUARES = ~DARK_SQUARES;
}  // namespace BB

constexpr Bitboard squareBB(Square s) { return 1ULL << s; }

inline int popcount(Bitboard b) { return __builtin_popcountll(b); }
inline Square lsb(Bitboard b) { return Square(__builtin_ctzll(b)); }
inline Square msb(Bitboard b) { return Square(63 - __builtin_clzll(b)); }
inline Square popLsb(Bitboard& b) {
    Square s = lsb(b);
    b &= b - 1;
    return s;
}
inline bool moreThanOne(Bitboard b) { return (b & (b - 1)) != 0; }

// Attack tables (initialized by initBitboards()).
extern Bitboard PawnAttacks[2][64];
extern Bitboard KnightAttacks[64];
extern Bitboard KingAttacks[64];

// Slider attacks.  On BMI2 hardware we use PEXT lookup tables; otherwise
// a classic magic-bitboard implementation is used.
Bitboard bishopAttacks(Square sq, Bitboard occupied);
Bitboard rookAttacks(Square sq, Bitboard occupied);
inline Bitboard queenAttacks(Square sq, Bitboard occupied) {
    return bishopAttacks(sq, occupied) | rookAttacks(sq, occupied);
}

// A ray attack ignoring blockers, used by evaluation.
Bitboard rayAttacks(Square sq, int dir, Bitboard occupied);
Bitboard betweenBB(Square a, Square b);
Bitboard lineBB(Square a, Square b);

void initBitboards();

// Direction indices for rayAttacks.
enum Direction : int {
    NORTH = 0, SOUTH, EAST, WEST, NE, NW, SE, SW, DIR_NB
};

inline Bitboard shiftNorth(Bitboard b) { return b << 8; }
inline Bitboard shiftSouth(Bitboard b) { return b >> 8; }
inline Bitboard shiftEast(Bitboard b) { return (b & ~BB::FILE_H) << 1; }
inline Bitboard shiftWest(Bitboard b) { return (b & ~BB::FILE_A) >> 1; }
inline Bitboard shiftNE(Bitboard b) { return (b & ~BB::FILE_H) << 9; }
inline Bitboard shiftNW(Bitboard b) { return (b & ~BB::FILE_A) << 7; }
inline Bitboard shiftSE(Bitboard b) { return (b & ~BB::FILE_H) >> 7; }
inline Bitboard shiftSW(Bitboard b) { return (b & ~BB::FILE_A) >> 9; }
