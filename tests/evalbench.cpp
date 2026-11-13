#include "../src/bitboard.h"
#include "../src/eval.h"
#include "../src/movegen.h"
#include "../src/position.h"

#include <chrono>
#include <cstdio>

int main() {
    initBitboards();
    Zobrist::init();
    Position pos;
    pos.setFen("r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4");

    const int N = 5000000;
    volatile int sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) sink += Eval::evaluate(pos);
    auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("eval: %.1f ns/call, %.2f Mcalls/s (sink %d)\n", sec / N * 1e9,
                N / sec / 1e6, (int)sink);

    // Movegen + make/unmake throughput.
    const int M = 2000000;
    uint64_t nodes = 0;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < M; ++i) {
        MoveList ml;
        generateAll(pos, ml);
        for (int j = 0; j < ml.size; ++j) {
            Undo u;
            pos.makeMove(ml.moves[j], u);
            nodes++;
            pos.unmakeMove(ml.moves[j], u);
        }
    }
    t1 = std::chrono::steady_clock::now();
    const double sec2 = std::chrono::duration<double>(t1 - t0).count();
    std::printf("make/unmake: %.1f ns/pair, %.1f M/s (%llu)\n", sec2 / nodes * 1e9,
                nodes / sec2 / 1e6, (unsigned long long)nodes);
    // SEE throughput on captures.
    MoveList caps;
    generateCaptures(pos, caps);
    uint64_t seeCalls = 0, seeTrue = 0;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000000; ++i) {
        for (int j = 0; j < caps.size; ++j) {
            seeTrue += pos.seeGE(caps.moves[j], 0);
            ++seeCalls;
        }
    }
    t1 = std::chrono::steady_clock::now();
    const double sec3 = std::chrono::duration<double>(t1 - t0).count();
    std::printf("see: %.1f ns/call, %.1f M/s (true %llu/%llu)\n", sec3 / seeCalls * 1e9,
                seeCalls / sec3 / 1e6, (unsigned long long)seeTrue,
                (unsigned long long)seeCalls);
    return 0;
}
