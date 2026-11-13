#include "search.h"

#include "eval.h"
#include "movegen.h"
#include "tbprobe_glue.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {

constexpr int MAX_SEARCH_DEPTH = MAX_PLY - 4;
constexpr int PIECE_VALUE[PT_NB] = {100, 320, 330, 500, 900, 20000};

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int scoreToTT(int score, int ply) {
    if (score > MATE_THRESHOLD) return score + ply;
    if (score < -MATE_THRESHOLD) return score - ply;
    return score;
}

int scoreFromTT(int score, int ply) {
    if (score > MATE_THRESHOLD) return score - ply;
    if (score < -MATE_THRESHOLD) return score + ply;
    return score;
}

int capturedValue(const Position& pos, Move m) {
    if (moveFlags(m) == EP_CAPTURE) return PIECE_VALUE[PAWN];
    if (isCapture(m)) return PIECE_VALUE[typeOf(pos.pieceOn(toSq(m)))];
    return 0;
}

}  // namespace

TranspositionTable::TranspositionTable() { resize(64); }

void TranspositionTable::resize(int megabytes) {
    const size_t bytes = size_t(std::max(1, megabytes)) * 1024ULL * 1024ULL;
    size_t entries = bytes / sizeof(TTEntry);
    size_t power = 1;
    while (power * 2 <= entries) power *= 2;
    if (power < 1024) power = 1024;
    table.assign(power, TTEntry{});
    mask = power - 1;
    generation = 0;
}

void TranspositionTable::clear() {
    std::fill(table.begin(), table.end(), TTEntry{});
    generation = 0;
}

bool TranspositionTable::probe(uint64_t key, int ply, Move& move, int& score, int& depth,
                               int& bound) const {
    move = MOVE_NONE;
    if (table.empty()) return false;
    const size_t idx = (size_t(key) & mask) & ~size_t(1);
    const TTEntry& e0 = table[idx];
    const TTEntry* e = nullptr;
    if (e0.key == key && e0.bound != BOUND_NONE)
        e = &e0;
    else {
        const TTEntry& e1 = table[idx + 1];
        if (e1.key == key && e1.bound != BOUND_NONE) e = &e1;
    }
    if (!e) return false;
    move = Move(e->move);
    score = scoreFromTT(e->score, ply);
    depth = e->depth;
    bound = e->bound;
    return true;
}

void TranspositionTable::store(uint64_t key, int depth, int score, int bound, Move move,
                               int ply) {
    if (table.empty()) return;
    const size_t idx = (size_t(key) & mask) & ~size_t(1);
    TTEntry* e0 = &table[idx];
    TTEntry* e1 = &table[idx + 1];
    TTEntry* e = nullptr;
    if (e0->key == key && e0->bound != BOUND_NONE)
        e = e0;
    else if (e1->key == key && e1->bound != BOUND_NONE)
        e = e1;
    else {
        const int age0 = 8 * ((generation - e0->generation) & 0xFF);
        const int age1 = 8 * ((generation - e1->generation) & 0xFF);
        const int v0 = int(e0->depth) - age0;
        const int v1 = int(e1->depth) - age1;
        e = (v0 <= v1) ? e0 : e1;
        // Do not throw away a much deeper exact entry from this search.
        if (e->generation == generation && e->depth > depth + 3 && e->bound == EXACT)
            return;
    }
    e->key = key;
    e->score = int16_t(scoreToTT(score, ply));
    e->move = uint16_t(move & 0xFFFF);
    e->depth = int8_t(std::min(depth, 127));
    e->bound = uint8_t(bound);
    e->generation = generation;
}

int TranspositionTable::hashfull() const {
    if (table.empty()) return 0;
    const size_t sample = std::min<size_t>(1000, table.size());
    int used = 0;
    for (size_t i = 0; i < sample; ++i)
        if (table[i].generation == generation && table[i].bound != BOUND_NONE) ++used;
    return int(used * 1000 / sample);
}

Search::Search() {
    tt.resize(64);
    std::memset(history, 0, sizeof(history));
    std::memset(counterMoves, 0, sizeof(counterMoves));
    std::memset(killers, 0, sizeof(killers));
}

void Search::setHash(int megabytes) { tt.resize(megabytes); }

void Search::newGame() {
    tt.clear();
    tt.newSearch();
    std::memset(history, 0, sizeof(history));
    std::memset(counterMoves, 0, sizeof(counterMoves));
    std::memset(killers, 0, sizeof(killers));
}

int64_t Search::elapsedMs() const { return nowMs() - startMs; }

void Search::initTime(const SearchLimits& lim, Color side) {
    startMs = nowMs();
    softMs = hardMs = 0;
    if (lim.movetime > 0) {
        softMs = hardMs = std::max<int64_t>(1, int64_t(lim.movetime * 95 / 100));
        return;
    }
    if (lim.infinite) return;
    const int64_t myTime = (side == WHITE ? lim.wtime : lim.btime);
    const int64_t inc = (side == WHITE ? lim.winc : lim.binc);
    if (myTime > 0) {
        const int moves = lim.movestogo > 0 ? lim.movestogo : 30;
        int64_t base = myTime / moves + inc * 3 / 4;
        softMs = std::min<int64_t>(myTime * 55 / 100, base * 3 / 2);
        hardMs = std::min<int64_t>(myTime * 85 / 100, base * 3 + inc);
        softMs = std::max<int64_t>(softMs, 3);
        hardMs = std::max<int64_t>(hardMs, softMs + 2);
    } else if (lim.depth == 0 && lim.nodes == 0) {
        softMs = hardMs = 1000;
    }
}

void Search::checkLimits() {
    if (sharedStop && sharedStop->load(std::memory_order_relaxed))
        stop.store(true, std::memory_order_relaxed);
    if (stop.load(std::memory_order_relaxed)) return;
    if (limits.nodes && nodes >= limits.nodes) {
        stop.store(true, std::memory_order_relaxed);
        return;
    }
    if (hardMs > 0 && elapsedMs() >= hardMs) stop.store(true, std::memory_order_relaxed);
}

int Search::drawScore(Color side) const {
    return side == Color(rootColor) ? -contempt : contempt;
}

void Search::printInfo(int depth, int score) {
    if (!verbose) return;
    const int elapsed = int(std::max<int64_t>(1, elapsedMs()));
    std::cout << "info depth " << depth << " seldepth " << seldepth;
    if (isMateScore(score)) {
        int mate;
        if (score > 0)
            mate = (MATE - score + 1) / 2;
        else
            mate = -(MATE + score + 1) / 2;
        std::cout << " score mate " << mate;
    } else {
        std::cout << " score cp " << score;
    }
    std::cout << " nodes " << nodes << " nps " << (nodes * 1000ULL / uint64_t(elapsed))
              << " hashfull " << tt.hashfull() << " time " << elapsed << " pv";
    if (pvLength[0] > 0) {
        for (int i = 0; i < pvLength[0]; ++i) std::cout << ' ' << moveToString(pvTable[0][i]);
    } else if (!rootMoves.empty()) {
        std::cout << ' ' << moveToString(rootMoves[0].move);
    }
    std::cout << std::endl;
}

void Search::scoreMoves(const Position& pos, MoveList& ml, Move ttMove, int ply, Move prevMove) {
    const Color us = Color(pos.side);
    for (int i = 0; i < ml.size; ++i) {
        const Move m = ml.moves[i];
        int s;
        if (m == ttMove) {
            s = 1 << 30;
        } else if (isCapture(m)) {
            const PieceType victim =
                (moveFlags(m) == EP_CAPTURE) ? PAWN : typeOf(pos.pieceOn(toSq(m)));
            const PieceType attacker = typeOf(pos.pieceOn(fromSq(m)));
            s = (1 << 28) + PIECE_VALUE[victim] * 16 - PIECE_VALUE[attacker];
            if (PIECE_VALUE[victim] >= PIECE_VALUE[attacker] || pos.seeGE(m, 0))
                s += 1 << 27;
            else
                s -= 1 << 22;
        } else if (isPromotion(m)) {
            s = (1 << 27) + PIECE_VALUE[promotionType(m)];
        } else if (m == killers[ply][0]) {
            s = 1 << 26;
        } else if (m == killers[ply][1]) {
            s = 1 << 25;
        } else if (prevMove != MOVE_NONE &&
                   m == counterMoves[us][fromSq(prevMove)][toSq(prevMove)]) {
            s = 1 << 24;
        } else {
            s = history[us][fromSq(m)][toSq(m)];
        }
        ml.scores[i] = s;
    }
}

void Search::updateQuietStats(Color us, Move m, int depth, int ply, Move prevMove) {
    if (killers[ply][0] != m) {
        killers[ply][1] = killers[ply][0];
        killers[ply][0] = m;
    }
    const int bonus = std::min(depth * depth * 16, 1200);
    int& h = history[us][fromSq(m)][toSq(m)];
    h += bonus - h * std::abs(bonus) / 16384;
    h = clampInt(h, -MAX_HISTORY, MAX_HISTORY);
    if (prevMove != MOVE_NONE)
        counterMoves[us][fromSq(prevMove)][toSq(prevMove)] = m;
}

void Search::prepareRoot(Position& pos, Move ttMove) {
    rootMoves.clear();
    MoveList ml;
    generateAll(pos, ml);
    scoreMoves(pos, ml, ttMove, 0, MOVE_NONE);
    for (int i = 0; i < ml.size; ++i) {
        const Move m = ml.moves[i];
        if (!pos.moveIsLegal(m)) continue;
        rootMoves.push_back({m, ml.scores[i]});
    }
    std::stable_sort(rootMoves.begin(), rootMoves.end(),
                     [](const RootMove& a, const RootMove& b) { return a.score > b.score; });
    if (threadId > 0 && rootMoves.size() > 1) {
        const size_t shift = size_t(threadId) % std::min<size_t>(4, rootMoves.size());
        std::rotate(rootMoves.begin(), rootMoves.begin() + shift, rootMoves.end());
    }
}

int Search::searchRoot(Position& pos, int depth, int alpha, int beta) {
    int bestScore = -INF;
    Move bestMove = MOVE_NONE;
    const Color us = Color(pos.side);
    pvLength[0] = 0;
    bool first = true;

    for (size_t i = 0; i < rootMoves.size(); ++i) {
        const Move m = rootMoves[i].move;
        Undo u;
        if (NNUE::available()) {
            acc[1] = acc[0];
            pos.makeMove(m, u);
            NNUE::pushMove(pos, m, u, acc[1]);
        } else {
            pos.makeMove(m, u);
        }
        if (pos.inCheck(us)) {
            pos.unmakeMove(m, u);
            continue;
        }
        moveStack[0] = m;
        pvLength[1] = 1;
        int score;
        if (first) {
            score = -negamax(pos, depth - 1, -beta, -alpha, 1, true);
            first = false;
        } else {
            score = -negamax(pos, depth - 1, -alpha - 1, -alpha, 1, true);
            if (score > alpha)
                score = -negamax(pos, depth - 1, -beta, -alpha, 1, true);
        }
        pos.unmakeMove(m, u);
        if (stop.load(std::memory_order_relaxed)) return bestScore;

        rootMoves[i].score = score;
        if (score > bestScore) {
            bestScore = score;
            bestMove = m;
            pvTable[0][0] = m;
            const int n = std::max(1, pvLength[1]);
            for (int j = 1; j < n; ++j) pvTable[0][j] = pvTable[1][j];
            pvLength[0] = std::max(1, n);
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }
    return bestMove == MOVE_NONE ? -INF : bestScore;
}

int Search::quiescence(Position& pos, int alpha, int beta, int ply) {
    ++nodes;
    if ((nodes & 1023) == 0) checkLimits();
    if (stop.load(std::memory_order_relaxed)) return 0;
    seldepth = std::max(seldepth, ply);
    if (ply >= MAX_PLY - 2) return evalAt(pos, ply);
    pvLength[ply] = ply;

    const Color us = Color(pos.side);
    const bool inCheck = pos.inCheck(us);
    if (!inCheck && pos.halfmove >= 100) return drawScore(us);
    int bestScore;
    if (inCheck) {
        bestScore = -INF;
    } else {
        const int stand = evalAt(pos, ply);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
        bestScore = stand;
    }

    Move ttMove = MOVE_NONE;
    int ttScore = 0, ttDepth = 0, ttBound = 0;
    const bool ttHit = tt.probe(pos.key, ply, ttMove, ttScore, ttDepth, ttBound);
    const int alphaOrig = alpha;
    if (ttHit) {
        if (ttBound == TranspositionTable::EXACT ||
            (ttBound == TranspositionTable::LOWER && ttScore >= beta) ||
            (ttBound == TranspositionTable::UPPER && ttScore <= alpha))
            return ttScore;
    }

    MoveList ml;
    if (inCheck)
        generateAll(pos, ml);
    else
        generateCaptures(pos, ml);
    scoreMoves(pos, ml, ttMove, ply, MOVE_NONE);

    int legal = 0;
    Move bestMove = MOVE_NONE;
    for (int i = 0; i < ml.size; ++i) {
        ml.pickBest(i);
        const Move m = ml.moves[i];
        if (!inCheck) {
            if (isCapture(m) && !pos.seeGE(m, 0)) continue;
            const int cap = capturedValue(pos, m);
            if (bestScore + cap + 180 < alpha) continue;
        }
        Undo u;
        if (nnueWeight > 0 && NNUE::available()) {
            std::memcpy(&acc[ply + 1], &acc[ply], sizeof(NNUE::Accumulator));
            pos.makeMove(m, u);
            NNUE::pushMove(pos, m, u, acc[ply + 1]);
        } else {
            pos.makeMove(m, u);
        }
        if (pos.inCheck(us)) {
            pos.unmakeMove(m, u);
            continue;
        }
        ++legal;
        const int score = -quiescence(pos, -beta, -alpha, ply + 1);
        pos.unmakeMove(m, u);
        if (stop.load(std::memory_order_relaxed)) return 0;
        if (score > bestScore) {
            bestScore = score;
            bestMove = m;
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }
    if (inCheck && legal == 0) return matedIn(ply);
    const int bound = bestScore >= beta
                          ? TranspositionTable::LOWER
                          : (bestScore > alphaOrig ? TranspositionTable::EXACT
                                                   : TranspositionTable::UPPER);
    tt.store(pos.key, 0, bestScore, bound, bestMove, ply);
    return bestScore;
}

int Search::negamax(Position& pos, int depth, int alpha, int beta, int ply, bool nullOk) {
    if (depth <= 0) return quiescence(pos, alpha, beta, ply);
    ++nodes;
    if ((nodes & 1023) == 0) checkLimits();
    if (stop.load(std::memory_order_relaxed)) return 0;
    seldepth = std::max(seldepth, ply);
    if (ply >= MAX_PLY - 2) return evalAt(pos, ply);
    pvLength[ply] = ply;

    const Color us = Color(pos.side);
    const Color them = Color(us ^ 1);
    if (ply > 0 && pos.isDraw()) return drawScore(us);

    alpha = std::max(alpha, matedIn(ply));
    beta = std::min(beta, mateIn(ply + 1));
    if (alpha >= beta) return alpha;

    const bool pvNode = (beta - alpha > 1);
    const bool inCheck = pos.inCheck(us);

    // Tablebase exactness: with <=5 pieces the game is solved, so there is no
    // reason to search it. This is what stops otherwise-won endgames from
    // drifting into draws.
    if (ply > 0 && TB::ready()) {
        int wdl = 0;
        if (TB::probeWdl(pos, wdl)) {
            // Map WDL onto a mate-like score so the search steers towards the
            // conversion rather than just "not losing".
            const int tbScore = (wdl > 0) ? (MATE_THRESHOLD + 200 + ply)
                                          : (wdl < 0 ? -(MATE_THRESHOLD + 200 + ply)
                                                     : drawScore(us));
            if (wdl != 0) return tbScore;
            // Drawn per the tablebase; only trust it when it does not cut a
            // window short.
            if (tbScore <= alpha) return tbScore;
            if (tbScore >= beta) return tbScore;
        }
    }

    Move ttMove = MOVE_NONE;
    int ttScore = 0, ttDepth = 0, ttBound = 0;
    const bool ttHit = tt.probe(pos.key, ply, ttMove, ttScore, ttDepth, ttBound);
    if (ttHit && !pvNode && ttDepth >= depth) {
        if (ttBound == TranspositionTable::EXACT ||
            (ttBound == TranspositionTable::LOWER && ttScore >= beta) ||
            (ttBound == TranspositionTable::UPPER && ttScore <= alpha))
            return ttScore;
    }
    // Internal iterative reduction: without a TT hint, search a little
    // shallower first so the move ordering has something to work with.
    if (!ttHit && depth >= 4 && !inCheck) --depth;

    int eval = 0;
    if (!inCheck) {
        eval = evalAt(pos, ply);
        if (!pvNode) {
            if (depth <= 6 && eval - 85 * depth >= beta) return eval;
            if (depth <= 3 && eval + 260 * depth <= alpha)
                return quiescence(pos, alpha, beta, ply);
        }
    }

    if (nullOk && !inCheck && !pvNode && depth >= 3 && eval >= beta &&
        pos.hasNonPawnMaterial(us)) {
        int r = 2 + depth / 4 + std::min(3, (eval - beta) / 200);
        r = std::min(r, depth - 1);
        Undo u;
        pos.makeNullMove(u);
        moveStack[ply] = MOVE_NONE;
        const int score = -negamax(pos, depth - 1 - r, -beta, -beta + 1, ply + 1, false);
        pos.unmakeNullMove(u);
        if (stop.load(std::memory_order_relaxed)) return 0;
        if (score >= beta) return score;
    }

    MoveList ml;
    generateAll(pos, ml);
    const Move prevMove = (ply > 0 ? moveStack[ply - 1] : MOVE_NONE);
    scoreMoves(pos, ml, ttMove, ply, prevMove);

    const int alphaOrig = alpha;
    int bestScore = -INF;
    Move bestMove = MOVE_NONE;
    int legalMoves = 0;
    int searched = 0;

    for (int i = 0; i < ml.size; ++i) {
        ml.pickBest(i);
        const Move m = ml.moves[i];
        const bool quiet = !isCapture(m) && !isPromotion(m);
        const bool ttMoveHere = (m == ttMove);
        const bool killer = isKiller(m, ply);

        if (!inCheck && searched > 0 && quiet && !ttMoveHere && !killer) {
            if (depth <= 4 && eval + 90 + 100 * depth <= alpha) continue;
            if (depth <= 3 && i > 3 + depth * depth) continue;

        }
        if (!inCheck && searched > 0 && depth <= 4 && isCapture(m) && !isPromotion(m) &&
            !pos.seeGE(m, -70 * depth))
            continue;

        Undo u;
        if (nnueWeight > 0 && NNUE::available()) {
            std::memcpy(&acc[ply + 1], &acc[ply], sizeof(NNUE::Accumulator));
            pos.makeMove(m, u);
            NNUE::pushMove(pos, m, u, acc[ply + 1]);
        } else {
            pos.makeMove(m, u);
        }
        if (pos.inCheck(us)) {
            pos.unmakeMove(m, u);
            continue;
        }
        ++legalMoves;
        ++searched;
        const bool givesCheck = pos.inCheck(them);
        int newDepth = depth - 1;
        int r = 0;
        if (depth >= 3 && searched > 3 && quiet && !inCheck && !givesCheck) {
            r = int(0.75 + std::log(double(depth)) * std::log(double(searched)) / 2.0);
            r = std::min(r, newDepth - 1);
            if (r < 0) r = 0;
        }
        if (givesCheck && depth <= 6 && ply < 32) ++newDepth;

        moveStack[ply] = m;
        pvLength[ply + 1] = ply + 1;
        int score;
        if (searched == 1) {
            score = -negamax(pos, newDepth, -beta, -alpha, ply + 1, true);
        } else {
            score = -negamax(pos, newDepth - r, -alpha - 1, -alpha, ply + 1, true);
            if (score > alpha)
                score = -negamax(pos, newDepth, -beta, -alpha, ply + 1, true);
        }
        pos.unmakeMove(m, u);
        if (stop.load(std::memory_order_relaxed)) return 0;

        if (score > bestScore) {
            bestScore = score;
            bestMove = m;
            pvTable[ply][ply] = m;
            const int n = std::max(ply + 1, pvLength[ply + 1]);
            for (int j = ply + 1; j < n; ++j) pvTable[ply][j] = pvTable[ply + 1][j];
            pvLength[ply] = std::max(ply + 1, n);
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) {
            if (quiet) updateQuietStats(us, m, depth, ply, prevMove);
            break;
        }
    }

    if (legalMoves == 0) return inCheck ? matedIn(ply) : 0;
    if (bestMove == MOVE_NONE) {
        bestMove = ttMove;
        bestScore = alpha;
    }
    const int bound = bestScore >= beta
                          ? TranspositionTable::LOWER
                          : (bestScore > alphaOrig ? TranspositionTable::EXACT
                                                   : TranspositionTable::UPPER);
    tt.store(pos.key, depth, bestScore, bound, bestMove, ply);
    return bestScore;
}

SearchResult Search::think(Position pos, const SearchLimits& lim) {
    limits = lim;
    stop.store(false, std::memory_order_relaxed);
    nodes = 0;
    seldepth = 0;
    rootColor = pos.side;
    initTime(lim, Color(pos.side));
    tt.newSearch();
    std::memset(killers, 0, sizeof(killers));
    std::memset(moveStack, 0, sizeof(moveStack));
    if (acc.size() < size_t(MAX_PLY + 2)) acc.resize(MAX_PLY + 2);
    if (nnueWeight > 0 && NNUE::available()) NNUE::refresh(pos, acc[0]);

    Move ttMove = MOVE_NONE;
    int ttScore, ttDepth, ttBound;
    tt.probe(pos.key, 0, ttMove, ttScore, ttDepth, ttBound);
    prepareRoot(pos, ttMove);

    // A tablebase hit at the root is authoritative: prefer it over search.
    if (TB::ready()) {
        Move tbMove = MOVE_NONE;
        int tbWdl = 0;
        if (TB::probeRoot(pos, tbMove, tbWdl)) {
            SearchResult tb;
            tb.bestMove = tbMove;
            tb.score = tbWdl > 0 ? (MATE_THRESHOLD + 100)
                                 : (tbWdl < 0 ? -(MATE_THRESHOLD + 100) : 0);
            tb.depth = 1;
            tb.nodes = 0;
            if (verbose) {
                std::cout << "info string tablebase wdl " << tbWdl << " move "
                          << moveToString(tbMove) << std::endl;
            }
            return tb;
        }
    }

    SearchResult result;
    if (rootMoves.empty()) {
        result.nodes = nodes;
        return result;
    }

    Move bestMove = rootMoves[0].move;
    int bestScore = 0;
    int prevScore = 0;
    int completedDepth = 0;

    for (int depth = 1; depth <= MAX_SEARCH_DEPTH; ++depth) {
        if (limits.depth && depth > limits.depth) break;

        int alpha = -INF, beta = INF;
        int fails = 0;
        if (depth >= 4 && !isMateScore(prevScore)) {
            alpha = std::max(-INF, prevScore - 50);
            beta = std::min(INF, prevScore + 50);
        }
        int score = 0;
        while (true) {
            score = searchRoot(pos, depth, alpha, beta);
            if (stop.load(std::memory_order_relaxed)) break;
            if (score <= alpha) {
                alpha = std::max(-INF, alpha - 70 - 60 * fails);
                ++fails;
                continue;
            }
            if (score >= beta) {
                beta = std::min(INF, beta + 70 + 60 * fails);
                ++fails;
                continue;
            }
            prevScore = score;
            break;
        }
        if (stop.load(std::memory_order_relaxed)) break;

        std::stable_sort(rootMoves.begin(), rootMoves.end(),
                         [](const RootMove& a, const RootMove& b) { return a.score > b.score; });
        bestMove = rootMoves[0].move;
        bestScore = prevScore;
        completedDepth = depth;
        printInfo(depth, bestScore);

        if (limits.depth && depth >= limits.depth) break;
        if (limits.nodes && nodes >= limits.nodes) break;
        if (softMs > 0 && elapsedMs() >= softMs) break;
        if (isMateScore(bestScore)) {
            const int matePlies = MATE - std::abs(bestScore);
            if (matePlies <= depth * 2 + 4) break;
        }
    }

    if (completedDepth == 0) {
        bestMove = rootMoves.empty() ? MOVE_NONE : rootMoves[0].move;
        bestScore = 0;
    }

    result.bestMove = bestMove;
    result.score = bestScore;
    result.depth = completedDepth;
    result.nodes = nodes;
    return result;
}
