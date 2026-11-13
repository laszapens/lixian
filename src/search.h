// Lixian (离弦) - alpha-beta search.
#pragma once

#include "eval.h"
#include "nnue.h"
#include "position.h"
#include "types.h"

#include <atomic>
#include <cstdint>
#include <vector>

struct SearchLimits {
    int64_t wtime = -1;
    int64_t btime = -1;
    int64_t winc = 0;
    int64_t binc = 0;
    int movestogo = 0;
    int depth = 0;
    uint64_t nodes = 0;
    int movetime = 0;
    bool infinite = false;
};

struct SearchResult {
    Move bestMove = MOVE_NONE;
    int score = 0;
    int depth = 0;
    uint64_t nodes = 0;
};

struct TTEntry {
    uint64_t key = 0;
    int16_t score = 0;
    uint16_t move = 0;
    int8_t depth = 0;
    uint8_t bound = 0;
    uint8_t generation = 0;
    uint8_t pad = 0;
};
static_assert(sizeof(TTEntry) == 16, "TTEntry must stay 16 bytes");

class TranspositionTable {
public:
    enum Bound : uint8_t { BOUND_NONE = 0, UPPER = 1, LOWER = 2, EXACT = 3 };

    TranspositionTable();
    void resize(int megabytes);
    void clear();
    void newSearch() { ++generation; }
    bool probe(uint64_t key, int ply, Move& move, int& score, int& depth, int& bound) const;
    void store(uint64_t key, int depth, int score, int bound, Move move, int ply);
    int hashfull() const;
    size_t entryCount() const { return table.size(); }

private:
    std::vector<TTEntry> table;
    size_t mask = 0;
    uint8_t generation = 0;
};

class Search {
public:
    Search();

    void setHash(int megabytes);
    void newGame();
    void setContempt(int value) { contempt = value; }
    void setSharedStop(std::atomic<bool>* flag) { sharedStop = flag; }
    void setVerbose(bool value) { verbose = value; }
    void setThreadId(int value) { threadId = value; }
    void stopSearch() { stop.store(true, std::memory_order_relaxed); }
    void setNnueWeight(int pct) { nnueWeight = pct; }
    bool stopped() const { return stop.load(std::memory_order_relaxed); }

    // Runs a complete search. Prints UCI info lines; the caller prints bestmove.
    SearchResult think(Position pos, const SearchLimits& limits);

private:
    struct RootMove {
        Move move = MOVE_NONE;
        int score = -INF;
    };

    TranspositionTable tt;
    std::atomic<bool> stop{false};
    std::atomic<bool>* sharedStop = nullptr;
    bool verbose = true;
    int threadId = 0;
    SearchLimits limits;
    uint64_t nodes = 0;
    int seldepth = 0;
    int contempt = 0;
    // Default 0: the current net is positionally sound but its inference cost
    // (2 us vs 0.18 us for the hand-written eval) costs 5-6 plies of depth, and
    // measured strength is clearly worse. Kept as an opt-in research path via
    // LIXIAN_NNUE_WEIGHT. See README.
    int nnueWeight = 0;

    int rootColor = WHITE;
    int64_t startMs = 0;
    int64_t softMs = 0;
    int64_t hardMs = 0;

    std::vector<RootMove> rootMoves;
    // One NNUE accumulator per ply. Same shape as Stockfish's layout: each ply
    // owns its buffer, children are derived by applying the move's delta, so
    // there is no per-node memcpy of the whole accumulator.
    std::vector<NNUE::Accumulator> acc;
    Move killers[MAX_PLY][2] = {};
    Move counterMoves[COLOR_NB][64][64] = {};
    int history[COLOR_NB][64][64] = {};
    Move moveStack[MAX_PLY + 2] = {};
    Move pvTable[MAX_PLY][MAX_PLY] = {};
    int pvLength[MAX_PLY] = {};

    int negamax(Position& pos, int depth, int alpha, int beta, int ply, bool nullOk);
    int quiescence(Position& pos, int alpha, int beta, int ply);
    int searchRoot(Position& pos, int depth, int alpha, int beta);
    int evalAt(const Position& pos, int ply) const {
        if (nnueWeight <= 0 || !NNUE::available()) return Eval::evaluate(pos);
        return NNUE::evaluateBlended(pos, acc[ply], nnueWeight);
    }
    void prepareRoot(Position& pos, Move ttMove);
    void orderRootMoves(Move ttMove);
    void scoreMoves(const Position& pos, MoveList& ml, Move ttMove, int ply, Move prevMove);
    bool isKiller(Move m, int ply) const {
        return m == killers[ply][0] || m == killers[ply][1];
    }
    void updateQuietStats(Color us, Move m, int depth, int ply, Move prevMove);
    void checkLimits();
    int drawScore(Color side) const;
    int64_t elapsedMs() const;
    void printInfo(int depth, int score);
    void initTime(const SearchLimits& lim, Color side);
};
