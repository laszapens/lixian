#include "../src/bitboard.h"
#include "../src/movegen.h"
#include "../src/position.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static uint64_t perft(Position& pos, int depth) {
    if (depth == 0) return 1;
    MoveList ml;
    generateAll(pos, ml);
    uint64_t nodes = 0;
    for (int i = 0; i < ml.size; ++i) {
        const Move m = ml.moves[i];
        const Color us = Color(pos.side);
        Undo u;
        pos.makeMove(m, u);
        if (!pos.inCheck(us)) {
            nodes += depth == 1 ? 1 : perft(pos, depth - 1);
        }
        pos.unmakeMove(m, u);
    }
    return nodes;
}

struct Case {
    const char* fen;
    int depth;
    uint64_t expected;
};

int main(int argc, char** argv) {
    initBitboards();
    Zobrist::init();

    if (argc >= 3 && std::strcmp(argv[1], "perft") == 0) {
        Position pos;
        pos.setFen(argv[2]);
        const int depth = std::atoi(argv[3]);
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t n = perft(pos, depth);
        const auto t1 = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        std::printf("perft %d = %llu  (%.3fs, %.1f Mnps)\n", depth,
                    (unsigned long long)n, sec, n / sec / 1e6);
        return 0;
    }

    if (argc >= 3 && std::strcmp(argv[1], "moves") == 0) {
        Position pos;
        pos.setFen(argv[2]);
        MoveList ml;
        generateAll(pos, ml);
        std::vector<std::string> out;
        for (int i = 0; i < ml.size; ++i) {
            const Move m = ml.moves[i];
            const Color us = Color(pos.side);
            Undo u;
            pos.makeMove(m, u);
            const bool legal = !pos.inCheck(us);
            pos.unmakeMove(m, u);
            if (legal) out.push_back(moveToString(m));
        }
        std::sort(out.begin(), out.end());
        for (const auto& s : out) std::printf("%s\n", s.c_str());
        std::printf("count %zu\n", out.size());
        return 0;
    }

    const std::vector<Case> cases = {
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 1, 20},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 2, 400},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 3, 8902},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197281},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 1, 48},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 2, 2039},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 1, 14},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 2, 191},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 3, 2812},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43238},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 1, 6},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 2, 264},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 3, 9467},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 1, 44},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 2, 1486},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487},
        {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 1, 46},
        {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 2, 2079},
        {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 3, 89890},
        {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594},
    };

    int failed = 0;
    for (const auto& c : cases) {
        Position pos;
        pos.setFen(c.fen);
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t got = perft(pos, c.depth);
        const auto t1 = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        const bool ok = got == c.expected;
        std::printf("%s depth %d: %llu expected %llu (%.3fs, %.1f Mnps)\n",
                    ok ? "PASS" : "FAIL", c.depth, (unsigned long long)got,
                    (unsigned long long)c.expected, sec, got / sec / 1e6);
        if (!ok) ++failed;
    }
    std::printf("%s\n", failed ? "PERFT FAILURES" : "ALL PERFT OK");
    return failed ? 1 : 0;
}
