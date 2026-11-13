// tb_glue.h -- thin, crash-safe C++ wrapper around the vendored Fathom
// (Syzygy tablebase) probing library.
//
// The glue exists for two reasons:
//   1. It exposes the bitboard layout the engine already uses (a1 = 0 .. h8 = 63)
//      and hides Fathom's packed `TB_RESULT` encoding behind a plain `bool`.
//   2. It validates the position *before* handing it to Fathom.  This matters:
//      `tb_probe_wdl()` performs no validity check and, given a position whose
//      side NOT to move is in check (e.g. the enemy king is capturable), it
//      aborts an assertion in debug builds and dereferences a bad index in
//      release builds.  The glue turns that into a clean `false`.
//
// No exceptions are thrown anywhere in this interface.

#ifndef LIXIAN_TB_GLUE_H
#define LIXIAN_TB_GLUE_H

#include <cstdint>

// ---------------------------------------------------------------------------
// Initialise / tear down.
// ---------------------------------------------------------------------------

// Point the prober at a directory (or a ':'-separated list of directories)
// containing .rtbw/.rtbz files.  Returns true if Fathom accepted the path.
//
// IMPORTANT: a return value of `true` only means "the path was parsed"; it does
// NOT mean any table was found.  Fathom returns true and leaves TB_LARGEST == 0
// when the directory is empty or holds no usable tables.  Callers that care
// should check TB_LARGEST (declared in tbprobe.h) or simply treat every probe
// that returns false as "no tablebase answer available".
//
// Safe to call repeatedly and safe to call with an empty string to disable
// probing.  Calling it again while initialised re-scans and frees the previous
// state.
bool TB_init(const char* path);

// Release every table Fathom has mapped and free its hash tables.  After this,
// probes return false until TB_init() is called again.  Safe to call when not
// initialised, and safe to call twice.
void TB_free();

// ---------------------------------------------------------------------------
// Probing
// ---------------------------------------------------------------------------
//
// Square numbering: a1 = 0, b1 = 1, ..., h1 = 7, a2 = 8, ..., h8 = 63.
// This is Fathom's native numbering and matches the usual little-endian
// bitboard shift: bit N <-> square N.
//
// white / black : occupancy bitboards, one per colour.
// kings / queens / rooks / bishops / knights / pawns :
//                 piece bitboards for BOTH colours combined (the colour is
//                 recovered by intersecting with `white` / `black`).
// epSquare      : 0 = no en passant square, otherwise an a1..h8 index.
//                 NOTE: because 0 means "none", en passant on a1 is not
//                 representable.  This is inherited from Fathom and is
//                 harmless in practice: a legal en passant target always lies
//                 on rank 3 or rank 6 (index 16..47), never on a1 (index 0).
// turn          : true = white to move, false = black to move.
//
// Castling rights are not part of either interface because Syzygy tablebases
// do not encode them; the wrapper always passes 0 to Fathom.

// Probe the Win/Draw/Loss table for a position with no half-move clock.
//
// On success writes the result to *wdl and returns true.  The value is
// SIDE-TO-MOVE RELATIVE (see the convention note in tb_glue.cpp and README.md):
//
//   -2  loss
//   -1  blessed loss   (loss under the normal rules, draw under the 50-move rule)
//    0  draw
//   +1  cursed win     (win under the normal rules, draw under the 50-move rule)
//   +2  win
//
// Returns false (and leaves *wdl untouched) when the position is illegal, when
// the required table is missing/corrupt, or when the tablebase has > 7 pieces.
// `wdl` may be null, in which case only the boolean is reported.
//
// Thread safety: inherits Fathom's; safe to call concurrently once TB_init()
// has returned, as the underlying tables are mapped lazily under a mutex.
bool TB_probe_wdl_simple(uint64_t white, uint64_t black,
                         uint64_t kings, uint64_t queens, uint64_t rooks,
                         uint64_t bishops, uint64_t knights, uint64_t pawns,
                         unsigned epSquare, bool turn, int* wdl);

// Probe at the root: returns the WDL of the position, a best move and its
// distance-to-zero, all packed into `*result`.  Decode with the macros from
// tbprobe.h:
//
//   TB_GET_WDL(result)      0..4 = LOSS, BLESSED_LOSS, DRAW, CURSED_WIN, WIN
//                           (0..4, NOT -2..+2 -- compare with TB_WIN etc.;
//                            side-to-move relative, same convention as above)
//   TB_GET_FROM(result)     origin square,  a1 = 0 .. h8 = 63
//   TB_GET_TO(result)       target square,  a1 = 0 .. h8 = 63
//   TB_GET_PROMOTES(result) TB_PROMOTES_NONE / _QUEEN / _ROOK / _BISHOP / _KNIGHT
//   TB_GET_EP(result)       non-zero if the move is an en passant capture
//   TB_GET_DTZ(result)      distance to zero, always non-negative
//
// `rule50` is the half-move clock and IS honoured here (it decides whether a
// win is reported as TB_CURSED_WIN instead of TB_WIN).
//
// Returns false when the position is illegal or the tables needed for this
// position are unavailable; in that case *result is also set to
// TB_RESULT_FAILED (0xFFFFFFFF).  On success *result is guaranteed not to be
// TB_RESULT_FAILED, but it MAY be TB_RESULT_CHECKMATE or TB_RESULT_STALEMATE,
// which are successful outcomes and must be tested for by the caller before
// decoding a move.
//
// Thread safety: NOT thread safe -- Fathom uses shared scratch state.  Call
// this only from the root/search thread, once per search.
bool TB_probe_root_simple(uint64_t white, uint64_t black,
                          uint64_t kings, uint64_t queens, uint64_t rooks,
                          uint64_t bishops, uint64_t knights, uint64_t pawns,
                          unsigned rule50, unsigned epSquare, bool turn,
                          unsigned* result);

#endif  // LIXIAN_TB_GLUE_H
