// tb_glue.cpp -- implementation of the crash-safe Fathom wrapper.
//
// ===========================================================================
// VERIFIED SIDE-TO-MOVE CONVENTION  (derived from source AND measurement)
// ===========================================================================
//
// Fathom does NOT mirror the position.  `tb_probe_wdl_impl()` copies `turn`
// straight into `Pos.turn` and `probe_wdl()` returns a value that is relative
// to the side to move:
//
//   tbprobe.c:1959-1965   "The return value is from the point of view of the
//                          side to move: -2 loss, -1 blessed loss, 0 draw,
//                          +1 cursed win, +2 win"
//
// `tb_probe_wdl_impl()` then adds 2, so the *public* `tb_probe_wdl()` returns
// 0..4, i.e. the raw 0..4 code is ALSO side-to-move relative (TB_WIN means
// "the side to move wins").  Mirroring only happens deeper down, inside
// `probe_table()`, and only to pick which colour's half of an asymmetric
// table to index -- it never flips the sign of the returned value.
//
// Consequences, both measured on real 3-4-5 tables:
//
//   KQvK, white K a1 + Q b1, black K c3.
//     turn = white  -> 4  (TB_WIN)   -- White wins, and White is to move
//     turn = black  -> 0  (TB_LOSS)  -- Black is to move and loses
//
//   The SAME board, side to move swapped, gives mirrored results.  A caller
//   must therefore read a positive value as "good for whoever is to move",
//   not "good for White".
//
// Disambiguating control position (this is the one that rules out the
// alternative reading "positive == White wins"):
//
//   White bare king a1, black king c3, black pawn c2, WHITE to move.
//     turn = white  -> 0  (TB_LOSS)  -- correct: White is lost
//     turn = black  -> 4  (TB_WIN)   -- correct: Black is winning and moves
//   A White-win interpretation would have had to report TB_WIN for white to
//   move here, which it does not.  The value tracks the mover, not the colour.
//
// This is consistent with `Pos.turn` in tbchess.c being plain
// "true = white to move" and with `calc_key()` treating white/black bitboards
// literally; no bit-flipping of the bitboards ever occurs in either entry
// point for the WDL path.
//
// ===========================================================================
// CRASH-SAFETY
// ===========================================================================
//
// `tb_probe_wdl()` (the public inline in tbprobe.h) does NOT validate the
// position.  `probe_wdl()` immediately calls `gen_captures()`, which does
//
//     unsigned from = lsb(pos->kings & us);   // us = side to move
//     assert(from < 64);
//
// If the side to move has no king in the position (or the opposite king is
// capturable / adjacent), `lsb(0)` fires.  With assertions on this aborts;
// with NDEBUG `__builtin_ffsll(0) - 1` is -1 and the code indexes tables with
// a huge unsigned, giving a segfault.  Measured: the task's FEN
// "8/8/8/8/8/3k4/8/KQ6 w - - 0 1" has the black king on d3 attacked by the
// white queen on b1, i.e. the side NOT to move is in check -- an impossible
// position -- and probing it crashed exactly as described.
//
// `tb_probe_root()` is different: `tb_probe_root_impl()` calls `is_valid()`
// first and returns TB_RESULT_FAILED for such positions, so it is already
// safe.  The guard below is therefore redundant for the root path but is kept
// so both entry points behave identically, and because `is_valid()` lives in
// the private section of tbchess.c and cannot be reused directly.
//
// The guard implements the same predicate set as tbchess.c's `is_valid()`:
//   * exactly two kings, one per colour;
//   * the piece bitboards are disjoint and exactly cover the occupancy;
//   * no pawns on rank 1 or rank 8;
//   * the side NOT to move is not in check (Fathom's `is_legal()`); a king
//     cannot be captured, so "enemy king attacked" == illegal.
// The extra "side to move is in check" case is perfectly legal (it is just
// check) and is intentionally NOT rejected.

#include "tb_glue.h"

#include <cstring>

extern "C" {
#include "tbprobe.h"
}

namespace {

// --- attack tables -------------------------------------------------------
// Built once on first use instead of relying on Fathom's private helpers: the
// public helper API (tb_king_attacks etc.) is only compiled when
// TB_NO_HELPER_API is undefined, and we do not want to depend on that.  These
// are 8 leaper masks, cheap enough to compute lazily.

struct AttackTables {
    // [colour][square]; colour index 0 = black, 1 = white, matching
    // Fathom's `pawn_attacks(sq, color)` where true == white.
    uint64_t pawnAttacks[2][64];
    uint64_t knightAttacks[64];
    uint64_t kingAttacks[64];

    AttackTables() { init(); }

    static uint64_t bit(int file, int rank) {
        if (file < 0 || file > 7 || rank < 0 || rank > 7) return 0;
        return 1ULL << (rank * 8 + file);
    }

    void init() {
        for (int sq = 0; sq < 64; ++sq) {
            const int f = sq & 7;
            const int r = sq >> 3;

            // White pawns attack "up" the board (towards rank 8).
            pawnAttacks[1][sq] = bit(f - 1, r + 1) | bit(f + 1, r + 1);
            // Black pawns attack "down" the board.
            pawnAttacks[0][sq] = bit(f - 1, r - 1) | bit(f + 1, r - 1);

            uint64_t n = 0;
            static const int knightDelta[8][2] = {
                {1, 2}, {2, 1}, {2, -1}, {1, -2},
                {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
            for (const auto& d : knightDelta) n |= bit(f + d[0], r + d[1]);
            knightAttacks[sq] = n;

            uint64_t k = 0;
            for (int df = -1; df <= 1; ++df)
                for (int dr = -1; dr <= 1; ++dr)
                    if (df || dr) k |= bit(f + df, r + dr);
            kingAttacks[sq] = k;
        }
    }

    // Sliding attacks, ray-by-ray until the first blocker (inclusive).
    static uint64_t sliding(int sq, uint64_t occ, const int dirs[][2],
                            int nDirs) {
        uint64_t att = 0;
        const int f0 = sq & 7, r0 = sq >> 3;
        for (int d = 0; d < nDirs; ++d) {
            int f = f0 + dirs[d][0];
            int r = r0 + dirs[d][1];
            while (f >= 0 && f <= 7 && r >= 0 && r <= 7) {
                const uint64_t b = 1ULL << (r * 8 + f);
                att |= b;
                if (occ & b) break;
                f += dirs[d][0];
                r += dirs[d][1];
            }
        }
        return att;
    }
};

const AttackTables& attacks() {
    static const AttackTables tables;
    return tables;
}

inline int popcount64(uint64_t b) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(b);
#else
    int n = 0;
    while (b) { b &= b - 1; ++n; }
    return n;
#endif
}

inline int lsb64(uint64_t b) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(b);
#else
    int n = 0;
    while (!(b & 1ULL)) { b >>= 1; ++n; }
    return n;
#endif
}

// Does `attacker` give check to the king of colour `kingIsWhite`?
bool kingAttacked(uint64_t occ, uint64_t attackerKings, uint64_t attackerQueens,
                  uint64_t attackerRooks, uint64_t attackerBishops,
                  uint64_t attackerKnights, uint64_t attackerPawns,
                  int kingSquare, bool kingIsWhite) {
    const AttackTables& t = attacks();

    // Fathom models pawns by their attack mask indexed with the mover's
    // colour, so a square is attacked by an enemy pawn iff the enemy pawn's
    // forward-attack mask covers it.  Equivalently: ask for the attack mask of
    // the king square as if a pawn of the *king's own* colour stood there and
    // intersect with enemy pawns.
    if (t.pawnAttacks[kingIsWhite ? 1 : 0][kingSquare] & attackerPawns)
        return true;

    if (t.knightAttacks[kingSquare] & attackerKnights) return true;
    if (t.kingAttacks[kingSquare] & attackerKings) return true;

    static const int rookDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    static const int bishopDirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

    const uint64_t rq = t.sliding(kingSquare, occ, rookDirs, 4);
    if (rq & (attackerRooks | attackerQueens)) return true;

    const uint64_t bq = t.sliding(kingSquare, occ, bishopDirs, 4);
    if (bq & (attackerBishops | attackerQueens)) return true;

    return false;
}

// Replicates tbchess.c's `is_valid()` for the bitboard-only entry points.
bool positionIsSane(uint64_t white, uint64_t black, uint64_t kings,
                    uint64_t queens, uint64_t rooks, uint64_t bishops,
                    uint64_t knights, uint64_t pawns, bool turn) {
    // Exactly one king per colour, and two in total.
    if (popcount64(kings) != 2) return false;
    if (popcount64(kings & white) != 1) return false;
    if (popcount64(kings & black) != 1) return false;

    // Colours must not overlap.
    if (white & black) return false;

    // Piece bitboards must be pairwise disjoint...
    if (kings & queens) return false;
    if (kings & rooks) return false;
    if (kings & bishops) return false;
    if (kings & knights) return false;
    if (kings & pawns) return false;
    if (queens & rooks) return false;
    if (queens & bishops) return false;
    if (queens & knights) return false;
    if (queens & pawns) return false;
    if (rooks & bishops) return false;
    if (rooks & knights) return false;
    if (rooks & pawns) return false;
    if (bishops & knights) return false;
    if (bishops & pawns) return false;
    if (knights & pawns) return false;

    // ...and together exactly equal the occupancy.
    if ((white | black) != (kings | queens | rooks | bishops | knights | pawns))
        return false;

    // No pawns on the first or last rank (0xFF and 0xFF00000000000000).
    if (pawns & 0xFF000000000000FFULL) return false;

    const uint64_t occ = white | black;

    // The side NOT to move must not be in check: a position where the enemy
    // king stands attacked is unreachable, and Fathom would try to capture it.
    const uint64_t mover = turn ? white : black;
    const uint64_t other = turn ? black : white;

    // Side to move must have a king (already implied by the counts above, but
    // keep the check explicit so the lsb() below can never see a zero mask).
    const uint64_t moverKingBB = kings & mover;
    if (!moverKingBB) return false;
    const uint64_t otherKingBB = kings & other;
    if (!otherKingBB) return false;

    const int otherKing = lsb64(otherKingBB);

    // The king of the side not to move must be safe from the side to move.
    if (kingAttacked(occ, kings & mover, queens & mover, rooks & mover,
                     bishops & mover, knights & mover, pawns & mover,
                     otherKing, !turn))
        return false;

    return true;
}

// Fathom's ep field is uint8_t and 0 means "none"; clamp out-of-range input
// rather than silently truncating onto a wrong square.
inline uint8_t normaliseEp(unsigned ep) { return ep < 64 ? static_cast<uint8_t>(ep) : 0; }

}  // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool TB_init(const char* path) {
    // Fathom dereferences `path` unconditionally (strlen/strcmp), so a null
    // pointer must be filtered here.
    if (path == nullptr) path = "";
    return tb_init(path) != false;
}

void TB_free() { tb_free(); }

bool TB_probe_wdl_simple(uint64_t white, uint64_t black, uint64_t kings,
                         uint64_t queens, uint64_t rooks, uint64_t bishops,
                         uint64_t knights, uint64_t pawns, unsigned epSquare,
                         bool turn, int* wdl) {
    if (!positionIsSane(white, black, kings, queens, rooks, bishops, knights,
                        pawns, turn))
        return false;

    // rule50 is 0 here: this is the search-time entry point, and Fathom's own
    // contract is that tb_probe_wdl() requires rule50 == 0 (a non-zero clock
    // makes it return TB_RESULT_FAILED outright).  Castling is 0 because
    // Syzygy does not encode castling rights.
    const unsigned raw =
        tb_probe_wdl(white, black, kings, queens, rooks, bishops, knights,
                     pawns, /*rule50=*/0, /*castling=*/0, normaliseEp(epSquare),
                     turn);

    if (raw == TB_RESULT_FAILED) return false;

    // Fathom returns 0..4 == TB_LOSS..TB_WIN, already side-to-move relative.
    // Map back onto the conventional -2..+2 scale documented in the header.
    if (wdl != nullptr) *wdl = static_cast<int>(raw) - 2;
    return true;
}

bool TB_probe_root_simple(uint64_t white, uint64_t black, uint64_t kings,
                          uint64_t queens, uint64_t rooks, uint64_t bishops,
                          uint64_t knights, uint64_t pawns, unsigned rule50,
                          unsigned epSquare, bool turn, unsigned* result) {
    // Fathom clamps rule50 to uint8_t; mirror that so the caller sees the same
    // behaviour as the underlying probe.  100+ already means "the 50-move rule
    // makes this a draw", so saturating is equivalent for our purpose.
    const unsigned r50 = rule50 > 255 ? 255 : rule50;

    if (!positionIsSane(white, black, kings, queens, rooks, bishops, knights,
                        pawns, turn)) {
        if (result != nullptr) *result = TB_RESULT_FAILED;
        return false;
    }

    const unsigned res =
        tb_probe_root(white, black, kings, queens, rooks, bishops, knights,
                      pawns, r50, /*castling=*/0, normaliseEp(epSquare), turn,
                      /*results=*/nullptr);

    if (res == TB_RESULT_FAILED) {
        if (result != nullptr) *result = TB_RESULT_FAILED;
        return false;
    }

    // TB_RESULT_CHECKMATE and TB_RESULT_STALEMATE are legitimate, successful
    // outcomes and are passed through untouched for the caller to test.
    if (result != nullptr) *result = res;
    return true;
}
