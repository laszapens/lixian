// Lixian (离弦) - bridge between Position and the vendored Fathom prober.
//
// Two services are exposed:
//   * probeWdl()  - fast WDL lookup used at interior nodes, so a position that
//                   is already decided by the tablebase is scored exactly
//                   instead of being searched further.
//   * probeRoot() - one root-level call that returns the tablebase-optimal move
//                   together with its DTZ score, used to make the engine play
//                   perfect endgames instead of drifting into draws.
#include "tbprobe_glue.h"

#include "third_party/fathom/tb_glue.h"
#include "third_party/fathom/tbprobe.h"

#include <algorithm>
#include <cstdlib>

namespace TB {
namespace {

bool g_ready = false;

struct Pieces {
    uint64_t white = 0, black = 0;
    uint64_t kings = 0, queens = 0, rooks = 0, bishops = 0, knights = 0, pawns = 0;
};

// Tablebases ignore castling rights and only cover positions with <= 5 pieces
// (for the downloaded set), so anything else is skipped cheaply.
inline bool eligible(const Position& pos, Pieces& p, int& count) {
    if (pos.castling != 0) return false;
    count = 0;
    p = Pieces{};
    for (int s = 0; s < 64; ++s) {
        const Piece pc = pos.pieceOn(Square(s));
        if (pc == NO_PIECE) continue;
        if (++count > 7) return false;
        const uint64_t b = 1ULL << s;
        if (colorOf(pc) == WHITE)
            p.white |= b;
        else
            p.black |= b;
        switch (typeOf(pc)) {
            case PAWN: p.pawns |= b; break;
            case KNIGHT: p.knights |= b; break;
            case BISHOP: p.bishops |= b; break;
            case ROOK: p.rooks |= b; break;
            case QUEEN: p.queens |= b; break;
            default: p.kings |= b; break;
        }
    }
    return count >= 3;
}

}  // namespace

bool init(const std::string& path) {
    if (path.empty()) return false;
    TB_init(path.c_str());
    g_ready = TB_LARGEST > 0;
    return g_ready;
}

void shutdown() {
    if (g_ready) TB_free();
    g_ready = false;
}

bool ready() { return g_ready; }

bool probeWdl(const Position& pos, int& wdl) {
    if (!g_ready) return false;
    if (pos.halfmove > 0) return false;  // DTZ handling would be required.
    Pieces p;
    int count = 0;
    if (!eligible(pos, p, count)) return false;
    if (count > 7) return false;
    return TB_probe_wdl_simple(p.white, p.black, p.kings, p.queens, p.rooks, p.bishops,
                               p.knights, p.pawns, unsigned(pos.ep == NO_SQ ? 0 : pos.ep),
                               pos.side == WHITE, &wdl);
}

bool probeRoot(const Position& pos, Move& move, int& wdl) {
    if (!g_ready) return false;
    Pieces p;
    int count = 0;
    if (!eligible(pos, p, count)) return false;
    if (count > 7) return false;

    unsigned result = 0;
    if (!TB_probe_root_simple(p.white, p.black, p.kings, p.queens, p.rooks, p.bishops,
                              p.knights, p.pawns, unsigned(pos.halfmove),
                              unsigned(pos.ep == NO_SQ ? 0 : pos.ep), pos.side == WHITE,
                              &result))
        return false;
    if (result == TB_RESULT_FAILED || result == TB_RESULT_CHECKMATE ||
        result == TB_RESULT_STALEMATE)
        return false;

    const unsigned from = TB_GET_FROM(result);
    const unsigned to = TB_GET_TO(result);
    const unsigned promo = TB_GET_PROMOTES(result);
    const unsigned tbWdl = TB_GET_WDL(result);

    // Decode Fathom's 0..4 WDL into the conventional -2..+2 range.
    wdl = int(tbWdl) - 2;

    const Move m = pos.uciToMove(squareToString(Square(from)) + squareToString(Square(to)) +
                                 (promo == TB_PROMOTES_QUEEN
                                      ? "q"
                                      : promo == TB_PROMOTES_ROOK
                                            ? "r"
                                            : promo == TB_PROMOTES_BISHOP
                                                  ? "b"
                                                  : promo == TB_PROMOTES_KNIGHT ? "n" : ""));
    if (m == MOVE_NONE) return false;
    move = m;
    return true;
}

}  // namespace TB
