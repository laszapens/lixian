#include "bitboard.h"
#include "book.h"
#include "nnue.h"
#include "tbprobe_glue.h"
#include "eval.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "types.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::vector<std::unique_ptr<Search>> pool;
std::atomic<bool> g_stopAll{false};
Book book;
Position pos;
std::thread searchThread;
std::string bookPath;
int threadCount = 1;
int hashMb = 128;
int contemptValue = 0;

std::string defaultBookPath() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        std::string p(buf, size_t(n));
        const size_t slash = p.find_last_of('/');
        if (slash != std::string::npos) return p.substr(0, slash) + "/data/gambits.txt";
    }
    return "data/gambits.txt";
}

void stopAndJoin() {
    g_stopAll.store(true, std::memory_order_relaxed);
    if (searchThread.joinable()) searchThread.join();
    g_stopAll.store(false, std::memory_order_relaxed);
}

int perThreadHash(int total, int threads) {
    return std::max(16, total / std::max(1, threads));
}

void configurePool(int threads, int totalHash, int contempt) {
    stopAndJoin();
    threadCount = std::max(1, threads);
    hashMb = std::max(1, totalHash);
    contemptValue = contempt;
    g_stopAll.store(false, std::memory_order_relaxed);
    pool.clear();
    const int perHash = perThreadHash(hashMb, threadCount);
    for (int i = 0; i < threadCount; ++i) {
        auto s = std::make_unique<Search>();
        s->setHash(perHash);
        s->setContempt(contemptValue);
        s->setSharedStop(&g_stopAll);
        pool.push_back(std::move(s));
    }
}

uint64_t perft(Position& p, int depth) {
    if (depth == 0) return 1;
    MoveList ml;
    generateAll(p, ml);
    uint64_t nodes = 0;
    for (int i = 0; i < ml.size; ++i) {
        const Move m = ml.moves[i];
        const Color us = Color(p.side);
        Undo u;
        p.makeMove(m, u);
        if (!p.inCheck(us)) nodes += depth == 1 ? 1 : perft(p, depth - 1);
        p.unmakeMove(m, u);
    }
    return nodes;
}

void printUci() {
    std::cout << "id name Lixian 1.0 (离弦)\n";
    std::cout << "id author Lixian project\n";
    std::cout << "option name Hash type spin default 512 min 16 max 4096\n";
    std::cout << "option name Threads type spin default 4 min 1 max 64\n";
    std::cout << "option name Aggression type spin default 140 min 50 max 250\n";
    std::cout << "option name Contempt type spin default 0 min -100 max 100\n";
    std::cout << "option name Book type check default true\n";
    std::cout << "option name BookFile type string default " << bookPath << "\n";
    std::cout << "uciok" << std::endl;
}

void handlePosition(std::istringstream& is) {
    stopAndJoin();
    std::string token;
    is >> token;
    if (token == "startpos") {
        pos.startpos();
        is >> token;
        if (token != "moves") return;
    } else if (token == "fen") {
        std::string fen;
        int fields = 0;
        while (is >> token && token != "moves" && fields < 6) {
            fen += token;
            fen += ' ';
            ++fields;
        }
        pos.setFen(fen);
        if (token != "moves") return;
    } else {
        return;
    }

    while (is >> token) {
        const Move m = pos.uciToMove(token);
        if (m == MOVE_NONE) {
            std::cout << "info string illegal move " << token << std::endl;
            break;
        }
        Undo u;
        pos.makeMove(m, u);
    }
}

void handleGo(std::istringstream& is) {
    stopAndJoin();
    SearchLimits limits;
    std::string token;
    while (is >> token) {
        try {
            if (token == "wtime")
                is >> limits.wtime;
            else if (token == "btime")
                is >> limits.btime;
            else if (token == "winc")
                is >> limits.winc;
            else if (token == "binc")
                is >> limits.binc;
            else if (token == "movestogo")
                is >> limits.movestogo;
            else if (token == "depth")
                is >> limits.depth;
            else if (token == "nodes")
                is >> limits.nodes;
            else if (token == "movetime")
                is >> limits.movetime;
            else if (token == "infinite")
                limits.infinite = true;
            else if (token == "ponder")
                limits.infinite = true;
            else if (token == "searchmoves") {
                std::string rest;
                std::getline(is, rest);
                break;
            }
        } catch (...) {
            break;
        }
    }
    if (limits.depth < 0) limits.depth = 0;
    if (limits.depth > MAX_PLY - 4) limits.depth = MAX_PLY - 4;

    const bool gameClock = !limits.infinite && limits.depth == 0 && limits.nodes == 0;
    if (book.enabled && gameClock) {
        const Move m = book.probe(pos);
        if (m != MOVE_NONE) {
            std::cout << "info depth 1 score cp 0 nodes 1 time 1 pv " << moveToString(m)
                      << std::endl;
            std::cout << "bestmove " << moveToString(m) << std::endl;
            return;
        }
    }

    const Position snapshot = pos;
    searchThread = std::thread([snapshot, limits]() mutable {
        g_stopAll.store(false, std::memory_order_relaxed);
        const size_t n = pool.size();
        std::vector<SearchResult> results(n);
        if (n <= 1) {
            pool[0]->setVerbose(true);
            pool[0]->setThreadId(0);
            pool[0]->setSharedStop(&g_stopAll);
            results[0] = pool[0]->think(snapshot, limits);
        } else {
            std::vector<std::thread> workers;
            workers.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                pool[i]->setVerbose(i == 0);
                pool[i]->setThreadId(int(i));
                pool[i]->setSharedStop(&g_stopAll);
                workers.emplace_back([&, i]() {
                    results[i] = pool[i]->think(snapshot, limits);
                    if (isMateScore(results[i].score))
                        g_stopAll.store(true, std::memory_order_relaxed);
                });
            }
            for (auto& t : workers) t.join();
        }

        SearchResult best;
        int bestScore = -INF;
        int bestDepth = -1;
        for (const auto& r : results) {
            if (r.bestMove == MOVE_NONE) continue;
            if (r.score > bestScore ||
                (r.score == bestScore && r.depth > bestDepth)) {
                best = r;
                bestScore = r.score;
                bestDepth = r.depth;
            }
        }
        std::cout << "bestmove "
                  << (best.bestMove == MOVE_NONE ? "0000" : moveToString(best.bestMove))
                  << std::endl;
    });
}

void handleSetOption(std::istringstream& is) {
    std::string token, name, value;
    is >> token;
    if (token != "name") return;
    while (is >> token) {
        if (token == "value") break;
        if (!name.empty()) name += ' ';
        name += token;
    }
    while (is >> token) {
        if (!value.empty()) value += ' ';
        value += token;
    }

    auto toInt = [](const std::string& s, int fallback) {
        try {
            return std::stoi(s);
        } catch (...) {
            return fallback;
        }
    };

    if (name == "Hash") {
        configurePool(threadCount, std::max(16, toInt(value, 128)), contemptValue);
    } else if (name == "Threads") {
        configurePool(std::max(1, std::min(64, toInt(value, 1))), hashMb, contemptValue);
    } else if (name == "Aggression") {
        Eval::aggression = clampInt(toInt(value, 140), 50, 250);
    } else if (name == "Contempt") {
        contemptValue = clampInt(toInt(value, 0), -100, 100);
        for (auto& s : pool) s->setContempt(contemptValue);
    } else if (name == "Book") {
        book.enabled = (value == "true" || value == "1");
    } else if (name == "BookFile") {
        bookPath = value;
        book.load(bookPath);
    } else if (name == "Clear Hash") {
        stopAndJoin();
        for (auto& s : pool) {
            s->setHash(perThreadHash(hashMb, threadCount));
            s->newGame();
        }
    }
}

std::string trimCopy(const std::string& in) {
    size_t a = 0, b = in.size();
    while (a < b && std::isspace((unsigned char)in[a])) ++a;
    while (b > a && std::isspace((unsigned char)in[b - 1])) --b;
    return in.substr(a, b - a);
}

std::string scoreText(int score) {
    if (isMateScore(score)) {
        const int mate = score > 0 ? (MATE - score + 1) / 2 : -(MATE + score + 1) / 2;
        return "mate " + std::to_string(mate);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.2f", double(score) / 100.0);
    return buf;
}

bool reportGameEnd(const Position& p) {
    MoveList ml;
    generateAll(p, ml);
    bool hasLegal = false;
    for (int i = 0; i < ml.size; ++i)
        if (p.moveIsLegal(ml.moves[i])) {
            hasLegal = true;
            break;
        }
    if (!hasLegal) {
        if (p.inCheck(Color(p.side))) {
            std::cout << "\nCheckmate. "
                      << (p.side == WHITE ? "Black" : "White") << " wins.\n";
        } else {
            std::cout << "\nStalemate. Draw.\n";
        }
        return true;
    }
    if (p.isDraw()) {
        std::cout << "\nDraw (50-move rule, repetition or insufficient material).\n";
        return true;
    }
    return false;
}

void printPlayHelp() {
    std::cout << "Commands: <uci move>  e.g. e2e4, e7e8q\n"
                 "          moves   list legal moves\n"
                 "          eval    static evaluation (side to move)\n"
                 "          hint    ask Lixian for a suggestion\n"
                 "          undo    take back one move from each side\n"
                 "          d       print board\n"
                 "          quit    leave\n";
}

int runPlay(int argc, char** argv) {
    const std::string humanArg = (argc >= 3 ? argv[2] : "white");
    const bool humanIsWhite = !(humanArg == "black" || humanArg == "b");
    const int engineMs = (argc >= 4 ? std::max(50, std::atoi(argv[3])) : 1000);

    Search engine;
    engine.setHash(256);
    engine.setVerbose(false);
    engine.setThreadId(0);

    pos.startpos();
    std::vector<Move> played;
    std::vector<Undo> undos;

    std::cout << "Lixian (离弦) - terminal game\n";
    std::cout << "You play " << (humanIsWhite ? "White" : "Black")
              << "; engine time " << engineMs << " ms/move.\n";
    std::cout << "Enter moves in UCI notation. Type 'help' for commands.\n";

    while (true) {
        if (reportGameEnd(pos)) break;
        pos.print(std::cout);

        if (pos.side == (humanIsWhite ? WHITE : BLACK)) {
            std::cout << "Your move: " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) break;
            line = trimCopy(line);
            if (line.empty()) continue;
            if (line == "quit" || line == "q") break;
            if (line == "help" || line == "h") {
                printPlayHelp();
                continue;
            }
            if (line == "d") continue;
            if (line == "eval") {
                std::cout << "Static eval: " << scoreText(Eval::evaluate(pos)) << "\n";
                continue;
            }
            if (line == "moves") {
                MoveList ml;
                generateAll(pos, ml);
                for (int i = 0; i < ml.size; ++i)
                    if (pos.moveIsLegal(ml.moves[i]))
                        std::cout << moveToString(ml.moves[i]) << ' ';
                std::cout << "\n";
                continue;
            }
            if (line == "hint") {
                SearchLimits lim;
                lim.movetime = std::max(500, engineMs);
                const SearchResult r = engine.think(pos, lim);
                if (r.bestMove != MOVE_NONE)
                    std::cout << "Suggestion: " << moveToString(r.bestMove) << " ("
                              << scoreText(r.score) << ")\n";
                continue;
            }
            if (line == "undo") {
                for (int k = 0; k < 2 && !played.empty(); ++k) {
                    pos.unmakeMove(played.back(), undos.back());
                    played.pop_back();
                    undos.pop_back();
                }
                continue;
            }
            const Move m = pos.uciToMove(line);
            if (m == MOVE_NONE) {
                std::cout << "Illegal or unknown move. Type 'moves' to list legal moves.\n";
                continue;
            }
            Undo u;
            pos.makeMove(m, u);
            played.push_back(m);
            undos.push_back(u);
            continue;
        }

        // Engine turn.
        Move best = MOVE_NONE;
        int score = 0;
        if (book.enabled) best = book.probe(pos);
        if (best != MOVE_NONE) {
            std::cout << "Lixian: " << moveToString(best) << "  [book]\n";
        } else {
            SearchLimits lim;
            lim.movetime = engineMs;
            const SearchResult r = engine.think(pos, lim);
            best = r.bestMove;
            score = r.score;
            if (best != MOVE_NONE)
                std::cout << "Lixian: " << moveToString(best) << "  [" << scoreText(score)
                          << "]\n";
        }
        if (best == MOVE_NONE) {
            std::cout << "Engine has no legal move.\n";
            break;
        }
        Undo u;
        pos.makeMove(best, u);
        played.push_back(best);
        undos.push_back(u);
    }
    std::cout << "Game over.\n";
    return 0;
}

void runBench() {
    static const char* fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    };
    Search bench;
    bench.setHash(64);
    bench.setVerbose(false);
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t totalNodes = 0;
    for (const char* fen : fens) {
        Position p;
        p.setFen(fen);
        SearchLimits lim;
        lim.depth = 14;
        const SearchResult r = bench.think(p, lim);
        totalNodes += r.nodes;
        std::cout << "info string bench " << moveToString(r.bestMove) << " d" << r.depth
                  << " score " << r.score << " nodes " << r.nodes << std::endl;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "info string bench total " << totalNodes << " nodes in " << sec << "s ("
              << uint64_t(totalNodes / std::max(0.001, sec)) << " nps)" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    initBitboards();
    Zobrist::init();

    bookPath = defaultBookPath();
    if (!book.load(bookPath)) book.load("data/gambits.txt");

    // Syzygy tablebases. LIXIAN_SYZYGY wins; otherwise try the project folder.
    {
        const char* env = std::getenv("LIXIAN_SYZYGY");
        const char* candidates[] = {env, "syzygy", "../syzygy"};
        for (const char* c : candidates) {
            if (c && TB::init(c)) break;
        }
    }

    // NNUE weights: explicit path first, then a few conventional locations.
    {
        const char* env = std::getenv("LIXIAN_NNUE");
        const char* candidates[] = {env, "weights.bin", "data/nnue.bin",
                                    "tools/nnue/weights/nnue.bin"};
        for (const char* c : candidates) {
            if (c && NNUE::load(c)) break;
        }
    }

    if (argc >= 2 && std::string(argv[1]) == "play") return runPlay(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "help") {
        std::cout << "Usage:\n  lixian            UCI mode (stdin/stdout)\n"
                     "  lixian play [white|black] [ms]   terminal game\n";
        return 0;
    }

    configurePool(4, 512, 0);

    // How much of the evaluation comes from the net rather than the hand-written
    // terms. 100 = pure NNUE; lower values keep more explicit tactics.
    {
        const char* env = std::getenv("LIXIAN_NNUE_WEIGHT");
        if (env) {
            const int pct = std::atoi(env);
            if (pct >= 0 && pct <= 100)
                for (auto& s : pool) s->setNnueWeight(pct);
        }
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string cmd;
        if (!(is >> cmd)) continue;

        if (cmd == "uci") {
            printUci();
        } else if (cmd == "isready") {
            std::cout << "readyok" << std::endl;
        } else if (cmd == "ucinewgame") {
            stopAndJoin();
            for (auto& s : pool) s->newGame();
            pos.startpos();
        } else if (cmd == "position") {
            handlePosition(is);
        } else if (cmd == "go") {
            handleGo(is);
        } else if (cmd == "stop") {
            stopAndJoin();
        } else if (cmd == "quit") {
            stopAndJoin();
            break;
        } else if (cmd == "setoption") {
            handleSetOption(is);
        } else if (cmd == "d") {
            pos.print(std::cout);
        } else if (cmd == "eval") {
            if (NNUE::available()) {
                NNUE::Accumulator acc;
                NNUE::refresh(pos, acc);
                std::cout << "info string eval " << NNUE::evaluate(pos, acc)
                          << " (nnue)" << std::endl;
            } else {
                std::cout << "info string eval " << Eval::evaluate(pos) << std::endl;
            }
        } else if (cmd == "perft") {
            int depth = 1;
            is >> depth;
            const auto t0 = std::chrono::steady_clock::now();
            const uint64_t n = perft(pos, std::max(0, depth));
            const auto t1 = std::chrono::steady_clock::now();
            const double sec = std::chrono::duration<double>(t1 - t0).count();
            std::cout << "info string perft " << depth << " nodes " << n << " time "
                      << int(sec * 1000) << " nps " << (sec > 0 ? uint64_t(n / sec) : 0)
                      << std::endl;
        } else if (cmd == "bench") {
            runBench();
        } else if (cmd == "ponderhit" || cmd == "register" || cmd == "debug") {
            // Accepted for UCI compatibility.
        }
    }
    stopAndJoin();
    return 0;
}
