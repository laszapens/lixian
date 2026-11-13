// tbtest.cpp -- integration test for the vendored Fathom / Syzygy glue.
//
// Build and run (see tests/tbtest_Makefile):
//     make -f tests/tbtest_Makefile run
//
// It does three jobs:
//   1. Smoke-tests the vendored library against the real tablebase directory
//      (/home/laszapen/lixian/syzygy, overridable on the command line).
//   2. Proves the SIDE-TO-MOVE convention of the WDL probe empirically, rather
//      than trusting the header comment.
//   3. Exercises the crash-safety guard in tb_glue.cpp on a deliberately
//      illegal FEN.
//
// Exit code 0 means every assertion held; 1 means at least one failed.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "tb_glue.h"

extern "C" {
#include "tbprobe.h"
}

namespace {

const char* kDefaultTablebasePath = "/home/laszapen/lixian/syzygy";

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (condition) {
        std::printf("    [ok]   %s\n", what);
    } else {
        std::printf("    [FAIL] %s\n", what);
        ++g_failures;
    }
}

// --------------------------------------------------------------------------
// Minimal FEN -> bitboard parser (placement + side to move + ep only).
// Tablebase probing needs nothing else: castling rights are excluded from
// Syzygy tables and the half-move clock is supplied separately.
// --------------------------------------------------------------------------
struct FenPosition {
    uint64_t white = 0;
    uint64_t black = 0;
    uint64_t kings = 0;
    uint64_t queens = 0;
    uint64_t rooks = 0;
    uint64_t bishops = 0;
    uint64_t knights = 0;
    uint64_t pawns = 0;
    bool whiteToMove = true;
    unsigned epSquare = 0;  // 0 = none
    bool ok = false;
};

int squareIndex(char file, char rank) {
    if (file < 'a' || file > 'h' || rank < '1' || rank > '8') return -1;
    return (rank - '1') * 8 + (file - 'a');
}

FenPosition parseFen(const char* fen) {
    FenPosition p;
    const char* s = fen;

    // --- piece placement, rank 8 first ---
    int rank = 7;
    int file = 0;
    while (*s && *s != ' ') {
        const char c = *s++;
        if (c == '/') {
            --rank;
            file = 0;
            if (rank < 0) return p;  // malformed
            continue;
        }
        if (c >= '1' && c <= '8') {
            file += c - '0';
            if (file > 8) return p;
            continue;
        }
        if (file > 7 || rank < 0) return p;

        const int sq = rank * 8 + file;
        const uint64_t bb = 1ULL << sq;
        const bool isWhite = (c >= 'A' && c <= 'Z');
        char lower = isWhite ? static_cast<char>(c - 'A' + 'a') : c;

        switch (lower) {
            case 'k': p.kings |= bb; break;
            case 'q': p.queens |= bb; break;
            case 'r': p.rooks |= bb; break;
            case 'b': p.bishops |= bb; break;
            case 'n': p.knights |= bb; break;
            case 'p': p.pawns |= bb; break;
            default: return p;  // malformed
        }
        if (isWhite) p.white |= bb; else p.black |= bb;
        ++file;
    }
    if (rank != 0 || file != 8) return p;

    // --- side to move ---
    if (*s == ' ') ++s;
    if (*s == 'w') p.whiteToMove = true;
    else if (*s == 'b') p.whiteToMove = false;
    else return p;
    ++s;

    // --- castling (accepted and ignored) ---
    if (*s == ' ') ++s;
    while (*s && *s != ' ') ++s;

    // --- en passant target ---
    if (*s == ' ') ++s;
    if (*s == '-' || *s == '\0') {
        p.epSquare = 0;
    } else if ((*s >= 'a' && *s <= 'h') && *(s + 1) >= '1' && *(s + 1) <= '8') {
        p.epSquare = static_cast<unsigned>(squareIndex(s[0], s[1]));
    } else {
        return p;
    }

    p.ok = true;
    return p;
}

std::string squareName(unsigned sq) {
    if (sq > 63) return "??";
    char buf[3];
    buf[0] = static_cast<char>('a' + (sq & 7));
    buf[1] = static_cast<char>('1' + (sq >> 3));
    buf[2] = '\0';
    return buf;
}

const char* wdlName(int wdl) {
    switch (wdl) {
        case -2: return "loss";
        case -1: return "blessed loss";
        case 0:  return "draw";
        case 1:  return "cursed win";
        case 2:  return "win";
        default: return "?";
    }
}

// Fathom's TB_ constant for a 0..4 code, for readable output.
const char* tbWdlName(unsigned wdl) {
    switch (wdl) {
        case TB_LOSS:         return "TB_LOSS";
        case TB_BLESSED_LOSS: return "TB_BLESSED_LOSS";
        case TB_DRAW:         return "TB_DRAW";
        case TB_CURSED_WIN:   return "TB_CURSED_WIN";
        case TB_WIN:          return "TB_WIN";
        default:              return "TB_?";
    }
}

const char* promotesName(unsigned p) {
    switch (p) {
        case TB_PROMOTES_NONE:   return "none";
        case TB_PROMOTES_QUEEN:  return "queen";
        case TB_PROMOTES_ROOK:   return "rook";
        case TB_PROMOTES_BISHOP: return "bishop";
        case TB_PROMOTES_KNIGHT: return "knight";
        default:                 return "?";
    }
}

// --------------------------------------------------------------------------
// WDL probe through the glue, with a uniform report line.
// --------------------------------------------------------------------------
struct WdlOutcome {
    bool ok = false;
    int wdl = 0;
};

WdlOutcome reportWdl(const char* label, const FenPosition& p) {
    WdlOutcome out;
    const bool ok = TB_probe_wdl_simple(p.white, p.black, p.kings, p.queens,
                                        p.rooks, p.bishops, p.knights, p.pawns,
                                        p.epSquare, p.whiteToMove, &out.wdl);
    out.ok = ok;
    std::printf("  %-46s turn=%-5s -> ", label,
                p.whiteToMove ? "white" : "black");
    if (ok) {
        std::printf("wdl=%+d (%s)\n", out.wdl, wdlName(out.wdl));
    } else {
        std::printf("probe FAILED (no usable table, or position rejected)\n");
    }
    return out;
}

// --------------------------------------------------------------------------
// Root probe through the glue, printing every decoded field.
// --------------------------------------------------------------------------
struct RootOutcome {
    bool ok = false;
    unsigned result = TB_RESULT_FAILED;
    bool checkmate = false;
    bool stalemate = false;
};

RootOutcome reportRoot(const char* label, const FenPosition& p,
                       unsigned rule50 = 0) {
    RootOutcome out;
    unsigned result = TB_RESULT_FAILED;
    const bool ok =
        TB_probe_root_simple(p.white, p.black, p.kings, p.queens, p.rooks,
                             p.bishops, p.knights, p.pawns, rule50,
                             p.epSquare, p.whiteToMove, &result);
    out.ok = ok;
    out.result = result;

    std::printf("  %-46s turn=%-5s -> ", label,
                p.whiteToMove ? "white" : "black");
    if (!ok) {
        std::printf("TB_RESULT_FAILED (0x%08X)\n", result);
        return out;
    }

    out.checkmate = (result == TB_RESULT_CHECKMATE);
    out.stalemate = (result == TB_RESULT_STALEMATE);

    if (out.checkmate) {
        std::printf("TB_RESULT_CHECKMATE (wdl=%u dtz=%u)\n", TB_GET_WDL(result),
                    TB_GET_DTZ(result));
        return out;
    }
    if (out.stalemate) {
        std::printf("TB_RESULT_STALEMATE (wdl=%u dtz=%u)\n", TB_GET_WDL(result),
                    TB_GET_DTZ(result));
        return out;
    }

    const unsigned from = TB_GET_FROM(result);
    const unsigned to = TB_GET_TO(result);
    std::printf(
        "wdl=%u %-14s dtz=%-3u move %s%s (%u->%u) promotes=%s ep=%u\n",
        TB_GET_WDL(result), tbWdlName(TB_GET_WDL(result)), TB_GET_DTZ(result),
        squareName(from).c_str(), squareName(to).c_str(), from, to,
        promotesName(TB_GET_PROMOTES(result)), TB_GET_EP(result));
    return out;
}

void printPosition(const char* name, const FenPosition& p) {
    std::printf("\n%s\n", name);
    std::printf("  white=0x%016llX black=0x%016llX\n",
                static_cast<unsigned long long>(p.white),
                static_cast<unsigned long long>(p.black));
    std::printf("  K=0x%016llX Q=0x%016llX R=0x%016llX B=0x%016llX "
                "N=0x%016llX P=0x%016llX\n",
                static_cast<unsigned long long>(p.kings),
                static_cast<unsigned long long>(p.queens),
                static_cast<unsigned long long>(p.rooks),
                static_cast<unsigned long long>(p.bishops),
                static_cast<unsigned long long>(p.knights),
                static_cast<unsigned long long>(p.pawns));
    std::printf("  turn=%s ep=%u\n", p.whiteToMove ? "white" : "black",
                p.epSquare);
}

}  // namespace

int main(int argc, char** argv) {
    const char* tbPath = argc > 1 ? argv[1] : kDefaultTablebasePath;

    std::printf("======================================================\n");
    std::printf(" Syzygy / Fathom integration test\n");
    std::printf(" tablebase path: %s\n", tbPath);
    std::printf("======================================================\n");

    // -----------------------------------------------------------------
    // 0. Initialise.  Note TB_init() returning true only means the path
    //    parsed -- TB_LARGEST tells us whether any table was actually found.
    // -----------------------------------------------------------------
    std::printf("\n[0] TB_init(\"%s\")\n", tbPath);
    const bool initOk = TB_init(tbPath);
    check(initOk, "TB_init returned true");
    std::printf("    TB_LARGEST = %u\n", TB_LARGEST);
    if (initOk && TB_LARGEST == 0) {
        std::printf(
            "    NOTE: TB_LARGEST == 0 -> no usable tables were found in that\n"
            "    directory.  Probes below will fail.  See README.md.\n");
    }
    if (!initOk) {
        std::printf("FATAL: TB_init failed, nothing more can be tested.\n");
        return 1;
    }

    // -----------------------------------------------------------------
    // 1. FEN 1 -- the task's first position (legal).
    //    8/8/8/8/8/2k5/2p5/K7 w - - 0 1
    //    white K a1; black K c3; black pawn c2; white to move.
    //    Black is a pawn up and wins, so WHITE to move must report a loss.
    // -----------------------------------------------------------------
    const FenPosition fen1 =
        parseFen("8/8/8/8/8/2k5/2p5/K7 w - - 0 1");
    printPosition("[1] FEN 1: 8/8/8/8/8/2k5/2p5/K7 w - - 0 1", fen1);
    check(fen1.ok, "FEN 1 parsed");
    check(fen1.whiteToMove, "FEN 1 side to move is white");
    check(fen1.kings == ((1ULL << 0) | (1ULL << 18)),
          "FEN 1 kings are a1 and c3");
    check(fen1.pawns == (1ULL << 10), "FEN 1 black pawn is on c2");

    const RootOutcome fen1RootWhite = reportRoot("root probe, white to move", fen1);
    const WdlOutcome fen1WdlWhite = reportWdl("wdl probe,  white to move", fen1);

    check(fen1RootWhite.ok, "FEN 1 root probe succeeded (not TB_RESULT_FAILED)");
    check(fen1RootWhite.result != TB_RESULT_FAILED,
          "FEN 1 root result is not TB_RESULT_FAILED");
    check(fen1WdlWhite.ok, "FEN 1 wdl probe succeeded");
    check(fen1WdlWhite.wdl == -2,
          "FEN 1 white to move reports -2 (loss) -- black is a pawn up");

    // Only legal white move is a1-a2 (b1 is covered by the c2 pawn and b2 by
    // the black king), so the root move must be exactly that.
    if (fen1RootWhite.ok && fen1RootWhite.result != TB_RESULT_FAILED &&
        !fen1RootWhite.checkmate && !fen1RootWhite.stalemate) {
        check(TB_GET_FROM(fen1RootWhite.result) == 0 &&
                  TB_GET_TO(fen1RootWhite.result) == 8,
              "FEN 1 root move is a1a2 (0->8), the only legal move");
        check(TB_GET_WDL(fen1RootWhite.result) == TB_LOSS,
              "FEN 1 root TB_RESULT_WDL == TB_LOSS");
        check(TB_GET_DTZ(fen1RootWhite.result) == 2,
              "FEN 1 root TB_RESULT_DTZ == 2");
    }

    // Same board, black to move: black is winning, so TB_WIN.
    FenPosition fen1Black = fen1;
    fen1Black.whiteToMove = false;
    const WdlOutcome fen1WdlBlack =
        reportWdl("wdl probe,  black to move (same board)", fen1Black);
    check(fen1WdlBlack.ok, "FEN 1 black-to-move wdl probe succeeded");
    check(fen1WdlBlack.wdl == 2,
          "FEN 1 black to move reports +2 (win) -- mirrored sign");

    // -----------------------------------------------------------------
    // 2. FEN 2 exactly as given in the task.
    //    8/8/8/8/8/3k4/8/KQ6 w - - 0 1
    //    white K a1, white Q b1, black K d3, white to move.
    //
    //    ***This position is ILLEGAL***: Qb1 already attacks d3 along the
    //    b1-c2-d3 diagonal, so the side NOT to move is in check.  No legal
    //    game can reach it, and Syzygy has no entry for it.
    //
    //    Fathom's tb_probe_wdl() does not validate and would abort on an
    //    assertion (or segfault under NDEBUG) inside gen_captures(), because
    //    the side to move could capture the enemy king.  The glue rejects it
    //    up front, and even raw tb_probe_root() returns TB_RESULT_FAILED via
    //    its internal is_valid() check.  Both are asserted below.
    // -----------------------------------------------------------------
    const FenPosition fen2 =
        parseFen("8/8/8/8/8/3k4/8/KQ6 w - - 0 1");
    printPosition("[2] FEN 2 (as given): 8/8/8/8/8/3k4/8/KQ6 w - - 0 1", fen2);
    check(fen2.ok, "FEN 2 parsed");
    check(fen2.whiteToMove, "FEN 2 side to move is white");
    check(fen2.queens == (1ULL << 1), "FEN 2 white queen is on b1");
    check(fen2.kings == ((1ULL << 0) | (1ULL << 19)),
          "FEN 2 kings are a1 and d3");
    std::printf(
        "    This is an ILLEGAL position: Qb1 attacks d3, so Black (the side\n"
        "    not to move) is in check.  Expect a clean rejection, not a crash.\n");

    const RootOutcome fen2Illegal = reportRoot("root probe, white to move (ILLEGAL)", fen2);
    check(!fen2Illegal.ok,
          "FEN 2 illegal position is rejected by the glue (no crash)");
    check(fen2Illegal.result == TB_RESULT_FAILED,
          "FEN 2 rejection reports TB_RESULT_FAILED");

    // Raw Fathom root probe, bypassing the glue's guard.  This is safe because
    // tb_probe_root_impl() runs its own is_valid() first, and it demonstrates
    // that Fathom itself agrees the position is unusable.
    const unsigned rawRoot =
        tb_probe_root(fen2.white, fen2.black, fen2.kings, fen2.queens,
                      fen2.rooks, fen2.bishops, fen2.knights, fen2.pawns, 0, 0,
                      0, fen2.whiteToMove, nullptr);
    check(rawRoot == TB_RESULT_FAILED,
          "raw tb_probe_root() on the illegal FEN returns TB_RESULT_FAILED");

    // NOTE: raw tb_probe_wdl() is deliberately NOT called on this position.
    // It has no validity check and would abort on an assertion (debug) or
    // segfault (NDEBUG) inside gen_captures().  That is precisely the crash
    // the glue's positionIsSane() guard exists to prevent.  Verified by hand:
    //   gcc -O2            -> Assertion `b != 0' failed in lsb()  (SIGABRT)
    //   gcc -O2 -DNDEBUG   -> SIGSEGV
    std::printf(
        "    (raw tb_probe_wdl is NOT called here on purpose: it would abort or\n"
        "     segfault on this position.  The glue rejects it before Fathom is\n"
        "     reached.  See the comment block in tb_glue.cpp.)\n");

    // The same board with Black to move IS legal (Black is simply in check)
    // and proves the KQvK material is won for White.
    FenPosition fen2Black = fen2;
    fen2Black.whiteToMove = false;
    const WdlOutcome fen2WdlBlack =
        reportWdl("wdl probe,  black to move (same board, LEGAL)", fen2Black);
    check(fen2WdlBlack.ok, "FEN 2 black-to-move wdl probe succeeded");
    check(fen2WdlBlack.wdl == -2,
          "FEN 2 black to move reports -2 (loss) => White wins this material");
    reportRoot("root probe, black to move (same board, LEGAL)", fen2Black);

    // -----------------------------------------------------------------
    // 3. KQvK with WHITE to move -- required to report a white win.
    //    Minimal repair of FEN 2: move the queen from b1 to b2 so that d3
    //    is no longer attacked; the position becomes legal.
    //    8/8/8/8/8/3k4/8/KQ6  ->  8/8/8/8/8/3k4/1Q6/K7
    // -----------------------------------------------------------------
    const FenPosition kqWhite =
        parseFen("8/8/8/8/8/3k4/1Q6/K7 w - - 0 1");
    printPosition("[3] KQvK legal, white to move: 8/8/8/8/8/3k4/1Q6/K7 w - - 0 1",
                  kqWhite);
    check(kqWhite.ok, "KQvK legal FEN parsed");
    check(kqWhite.queens == (1ULL << 9), "KQvK white queen is on b2");

    const WdlOutcome kqWdl = reportWdl("wdl probe,  white to move", kqWhite);
    const RootOutcome kqRoot = reportRoot("root probe, white to move", kqWhite);

    check(kqWdl.ok, "KQvK wdl probe succeeded");
    check(kqRoot.ok, "KQvK root probe succeeded (not TB_RESULT_FAILED)");
    check(kqWdl.wdl == 2,
          "KQvK with WHITE to move reports +2 (win) for the white side");
    if (kqRoot.ok && kqRoot.result != TB_RESULT_FAILED && !kqRoot.checkmate &&
        !kqRoot.stalemate) {
        check(TB_GET_WDL(kqRoot.result) == TB_WIN,
              "KQvK root TB_RESULT_WDL == TB_WIN");
        check(TB_GET_DTZ(kqRoot.result) > 0, "KQvK root TB_RESULT_DTZ > 0");
    }

    // -----------------------------------------------------------------
    // 4. THE DISAMBIGUATING CONTROL.
    //
    //    The naive-but-wrong reading of Fathom is "positive == White wins".
    //    FEN 1 already contradicts it (White to move reports -2), but FEN 1
    //    cannot distinguish "sign tracks the mover" from "sign tracks White"
    //    convincingly enough on its own because White happens to be losing
    //    there.  The control below makes the two hypotheses disagree:
    //
    //      White Kc6 + Pc7 vs Black Ka8
    //
    //    Here WHITE is winning.  Under "sign tracks the mover" white-to-move
    //    is +2 and black-to-move is -2.  Under "sign tracks White" the values
    //    would be identical for both colours.  They are not.
    // -----------------------------------------------------------------
    const FenPosition control =
        parseFen("k7/2P5/2K5/8/8/8/8/8 w - - 0 1");
    printPosition("[4] control: k7/2P5/2K5/8/8/8/8/8 w - - 0 1  (White winning)",
                  control);
    check(control.ok, "control FEN parsed");

    const WdlOutcome controlWhite = reportWdl("wdl probe,  white to move", control);
    FenPosition controlBlack = control;
    controlBlack.whiteToMove = false;
    const WdlOutcome controlBlackWdl =
        reportWdl("wdl probe,  black to move (same board)", controlBlack);

    check(controlWhite.ok && controlWhite.wdl == 2,
          "control: white to move reports +2 (win)");
    check(controlBlackWdl.ok && controlBlackWdl.wdl == -2,
          "control: black to move reports -2 (loss)");
    check(controlWhite.wdl == -controlBlackWdl.wdl,
          "control: the two values are exact mirrors => sign is side-to-move "
          "relative, NOT White-relative");

    // Concretely: White wins this, so a White-relative convention would have
    // to say +2 for BOTH colours.  Assert that it does not.
    check(!(controlWhite.wdl == 2 && controlBlackWdl.wdl == 2),
          "control: convention is NOT 'positive always means White wins'");

    // -----------------------------------------------------------------
    // 5. Draw, mate and stalemate -- sanity for the remaining encodings.
    // -----------------------------------------------------------------
    std::printf("\n[5] draw / mate / stalemate encodings\n");

    const FenPosition knvk = parseFen("7k/8/8/8/8/8/1N6/K7 w - - 0 1");
    check(knvk.ok, "KNvK FEN parsed");
    const WdlOutcome knvkWdl = reportWdl("wdl probe,  KNvK white to move", knvk);
    check(knvkWdl.ok && knvkWdl.wdl == 0, "KNvK is a draw (0)");

    const FenPosition kvk = parseFen("7k/8/8/8/8/8/8/K7 w - - 0 1");
    check(kvk.ok, "KvK FEN parsed");
    const WdlOutcome kvkWdl = reportWdl("wdl probe,  KvK white to move", kvk);
    check(kvkWdl.ok && kvkWdl.wdl == 0, "KvK is a draw (0)");
    // KvK needs no table file at all -- Fathom short-circuits it.
    const RootOutcome kvkRoot = reportRoot("root probe, KvK white to move", kvk);
    check(kvkRoot.ok && !kvkRoot.checkmate && !kvkRoot.stalemate,
          "KvK root probe returns a real move");

    // Black king h8, white Q g7, white K f6, black to move -> checkmate.
    const FenPosition mated = parseFen("7k/6Q1/5K2/8/8/8/8/8 b - - 0 1");
    check(mated.ok, "checkmate FEN parsed");
    const RootOutcome matedRoot = reportRoot("root probe, checkmated black", mated);
    check(matedRoot.ok, "checkmate position probe succeeded");
    check(matedRoot.result == TB_RESULT_CHECKMATE,
          "checkmate decodes to TB_RESULT_CHECKMATE");
    check(!matedRoot.stalemate, "checkmate is not reported as stalemate");

    // Black king a8, white Q c7, white K c6, black to move -> stalemate.
    const FenPosition stale = parseFen("k7/2Q5/2K5/8/8/8/8/8 b - - 0 1");
    check(stale.ok, "stalemate FEN parsed");
    const RootOutcome staleRoot = reportRoot("root probe, stalemated black", stale);
    check(staleRoot.ok, "stalemate position probe succeeded");
    check(staleRoot.result == TB_RESULT_STALEMATE,
          "stalemate decodes to TB_RESULT_STALEMATE");
    check(!staleRoot.checkmate, "stalemate is not reported as checkmate");

    // -----------------------------------------------------------------
    // 6. Guard behaviour for structurally invalid bitboards.
    // -----------------------------------------------------------------
    std::printf("\n[6] glue guard: structurally invalid bitboards\n");
    {
        int wdl = 0;
        // No black king.
        check(!TB_probe_wdl_simple((1ULL << 0), (1ULL << 10), (1ULL << 0),
                                   0, 0, 0, 0, 0, 0, true, &wdl),
              "rejects a position with no black king");
        // Two white kings, no black king.
        check(!TB_probe_wdl_simple((1ULL << 0) | (1ULL << 1), 0,
                                   (1ULL << 0) | (1ULL << 1), 0, 0, 0, 0, 0,
                                   0, true, &wdl),
              "rejects two same-colour kings");
        // Pawn on rank 8 (a8 = square 56; note h8 is 63, while 55 is h7).
        check(!TB_probe_wdl_simple((1ULL << 0) | (1ULL << 56), (1ULL << 10),
                                   (1ULL << 0) | (1ULL << 10), 0, 0, 0, 0,
                                   (1ULL << 56), 0, true, &wdl),
              "rejects a pawn on rank 8 (a8)");
        // Pawn on rank 1 (h1 = square 7).
        check(!TB_probe_wdl_simple((1ULL << 0) | (1ULL << 7), (1ULL << 10),
                                   (1ULL << 0) | (1ULL << 10), 0, 0, 0, 0,
                                   (1ULL << 7), 0, true, &wdl),
              "rejects a pawn on rank 1 (h1)");
        // Overlapping white/black occupancy.
        check(!TB_probe_wdl_simple((1ULL << 10), (1ULL << 10),
                                   (1ULL << 0) | (1ULL << 10), 0, 0, 0, 0, 0,
                                   0, true, &wdl),
              "rejects overlapping colour bitboards");
        // Occupancy disagrees with the piece bitboards (king declared but not
        // present in white/black).
        check(!TB_probe_wdl_simple((1ULL << 0), (1ULL << 10),
                                   (1ULL << 0) | (1ULL << 18), 0, 0, 0, 0, 0,
                                   0, true, &wdl),
              "rejects occupancy/piece mismatch");
        unsigned result = 0;
        check(!TB_probe_root_simple((1ULL << 0), (1ULL << 10), (1ULL << 0),
                                    0, 0, 0, 0, 0, 0, 0, true, &result) &&
                  result == TB_RESULT_FAILED,
              "root probe rejects a kingless position and sets "
              "TB_RESULT_FAILED");
        // A null output pointer must be tolerated.
        check(!TB_probe_wdl_simple((1ULL << 0), (1ULL << 10), (1ULL << 0), 0, 0,
                                   0, 0, 0, 0, true, nullptr),
              "tolerates a null wdl pointer");
    }

    // -----------------------------------------------------------------
    // 7. Null / empty path handling must not crash.
    // -----------------------------------------------------------------
    std::printf("\n[7] TB_init edge cases\n");
    check(TB_init(nullptr), "TB_init(nullptr) is handled (treated as \"\")");
    check(TB_LARGEST == 0, "TB_init(nullptr) leaves TB_LARGEST == 0");
    TB_free();
    TB_free();  // double free must be harmless
    check(true, "TB_free() is idempotent");

    // -----------------------------------------------------------------
    // Summary
    // -----------------------------------------------------------------
    std::printf("\n======================================================\n");
    std::printf(" %d checks, %d failure(s)\n", g_checks, g_failures);
    std::printf(" RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    std::printf("======================================================\n");
    return g_failures == 0 ? 0 : 1;
}
