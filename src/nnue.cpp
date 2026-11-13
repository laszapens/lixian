#include "nnue.h"

#include "eval.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace NNUE {
namespace {

Network g_net;

// File layout:
//   magic "LXNN2\0\0\0", int32 dims[3], then float32 arrays:
//   w1[768][512], b1[512], w2[32][512], b2[32], w3[32], b3, scale
constexpr char MAGIC[8] = {'L', 'X', 'N', 'N', '2', '\0', '\0', '\0'};

inline float relu(float x) { return x > 0.0f ? x : 0.0f; }

}  // namespace

const Network& net() { return g_net; }
bool available() { return g_net.loaded; }

bool Network::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[8] = {};
    f.read(magic, 8);
    if (!f || std::memcmp(magic, MAGIC, 8) != 0) return false;

    int32_t dims[3] = {};
    f.read(reinterpret_cast<char*>(dims), sizeof(dims));
    if (!f || dims[0] != INPUT_DIM || dims[1] != L1 || dims[2] != L2) return false;

    w1.assign(size_t(INPUT_DIM) * 2 * L1, 0.0f);
    b1.assign(size_t(2) * L1, 0.0f);
    w2.assign(size_t(L2) * 2 * L1, 0.0f);
    b2.assign(L2, 0.0f);
    w3.assign(L2, 0.0f);

    auto readArr = [&](std::vector<float>& v) {
        f.read(reinterpret_cast<char*>(v.data()), std::streamsize(v.size() * sizeof(float)));
    };
    readArr(w1);
    readArr(b1);
    readArr(w2);
    readArr(b2);
    readArr(w3);
    float tail[2] = {};
    f.read(reinterpret_cast<char*>(tail), sizeof(tail));
    if (!f) return false;
    b3 = tail[0];
    scale = tail[1] > 1.0f ? tail[1] : 400.0f;

    loaded = true;
    return true;
}

bool load(const std::string& path) { return g_net.load(path); }

void addPiece(Accumulator& acc, const Network& n, Color pc, PieceType pt, Square sq) {
    if (!n.loaded) return;
    acc.actValid = false;
    const int wIdx = featureIndex(WHITE, pc, pt, sq);
    const int bIdx = featureIndex(BLACK, pc, pt, sq);
    const float* rowW = &n.w1[size_t(wIdx) * 2 * L1];
    const float* rowB = &n.w1[size_t(bIdx) * 2 * L1 + L1];
#if defined(__AVX2__)
    for (int i = 0; i < L1; i += 8) {
        _mm256_storeu_ps(&acc.v[WHITE][i],
                         _mm256_add_ps(_mm256_loadu_ps(&acc.v[WHITE][i]),
                                       _mm256_loadu_ps(rowW + i)));
        _mm256_storeu_ps(&acc.v[BLACK][i],
                         _mm256_add_ps(_mm256_loadu_ps(&acc.v[BLACK][i]),
                                       _mm256_loadu_ps(rowB + i)));
    }
#else
    for (int i = 0; i < L1; ++i) {
        acc.v[WHITE][i] += rowW[i];
        acc.v[BLACK][i] += rowB[i];
    }
#endif
}

void removePiece(Accumulator& acc, const Network& n, Color pc, PieceType pt, Square sq) {
    if (!n.loaded) return;
    acc.actValid = false;
    const int wIdx = featureIndex(WHITE, pc, pt, sq);
    const int bIdx = featureIndex(BLACK, pc, pt, sq);
    const float* rowW = &n.w1[size_t(wIdx) * 2 * L1];
    const float* rowB = &n.w1[size_t(bIdx) * 2 * L1 + L1];
#if defined(__AVX2__)
    for (int i = 0; i < L1; i += 8) {
        _mm256_storeu_ps(&acc.v[WHITE][i],
                         _mm256_sub_ps(_mm256_loadu_ps(&acc.v[WHITE][i]),
                                       _mm256_loadu_ps(rowW + i)));
        _mm256_storeu_ps(&acc.v[BLACK][i],
                         _mm256_sub_ps(_mm256_loadu_ps(&acc.v[BLACK][i]),
                                       _mm256_loadu_ps(rowB + i)));
    }
#else
    for (int i = 0; i < L1; ++i) {
        acc.v[WHITE][i] -= rowW[i];
        acc.v[BLACK][i] -= rowB[i];
    }
#endif
}

void refresh(const Position& pos, Accumulator& acc) {
    const Network& n = g_net;
    if (!n.loaded) return;
    acc.actValid = false;
    std::memcpy(acc.v[WHITE], n.b1.data(), size_t(L1) * sizeof(float));
    std::memcpy(acc.v[BLACK], n.b1.data() + L1, size_t(L1) * sizeof(float));
    for (int s = 0; s < 64; ++s) {
        const Piece p = pos.pieceOn(Square(s));
        if (p == NO_PIECE) continue;
        addPiece(acc, n, colorOf(p), typeOf(p), Square(s));
    }
}

void pushMove(const Position& pos, Move m, const Undo& u, Accumulator& acc) {
    const Network& n = g_net;
    if (!n.loaded) return;
    const Color us = Color(pos.side ^ 1);  // side that just moved
    const Square from = fromSq(m);
    const Square to = toSq(m);
    const int flags = moveFlags(m);

    if (flags == EP_CAPTURE) {
        const Square capSq = Square(us == WHITE ? to - 8 : to + 8);
        removePiece(acc, n, Color(us ^ 1), PAWN, capSq);
    } else if (u.captured != NO_PIECE) {
        removePiece(acc, n, colorOf(u.captured), typeOf(u.captured), u.capturedSq);
    }

    if (flags >= PROMO_N) {
        removePiece(acc, n, us, PAWN, from);
        addPiece(acc, n, us, promotionType(m), to);
    } else {
        const Piece now = pos.pieceOn(to);  // the mover, after the move
        removePiece(acc, n, us, typeOf(now), from);
        addPiece(acc, n, us, typeOf(now), to);
    }

    if (flags == KING_CASTLE) {
        const Square rf = (us == WHITE ? 7 : 63);
        const Square rt = (us == WHITE ? 5 : 61);
        removePiece(acc, n, us, ROOK, rf);
        addPiece(acc, n, us, ROOK, rt);
    } else if (flags == QUEEN_CASTLE) {
        const Square rf = (us == WHITE ? 0 : 56);
        const Square rt = (us == WHITE ? 3 : 59);
        removePiece(acc, n, us, ROOK, rf);
        addPiece(acc, n, us, ROOK, rt);
    }
}

// Blend with the hand-written evaluator. The net supplies positional judgement
// that PSTs cannot express, but it is trained on 12k-node labels and therefore
// blunter about raw tactics than the explicit material/king-safety terms. A
// small HCE contribution restores that sharpness for a fraction of the cost.
int evaluate(const Position& pos, const Accumulator& acc) {
    const Network& n = g_net;
    if (!n.loaded) return 0;

    // Concatenate in side-to-move order: first this side's view, then the
    // opponent's. The trainer builds the same ordering.
    const Color persp = Color(pos.side);
    const Color opp = Color(persp ^ 1);
    if (!acc.actValid) {
        for (int p = 0; p < 2; ++p)
            for (int i = 0; i < L1; ++i) acc.act[p][i] = relu(acc.v[p][i]);
        acc.actValid = true;
    }
    const float* a0 = acc.act[persp];  // side to move
    const float* a1 = acc.act[opp];    // opponent

    // Project both halves directly; no staging copy, the two rows are read
    // separately which keeps the working set in L1 cache.
    float h[L2];
#if defined(__AVX2__)
    for (int j = 0; j < L2; ++j) {
        const float* row = &n.w2[size_t(j) * 2 * L1];
        __m256 acc0 = _mm256_setzero_ps();
        __m256 acc1 = _mm256_setzero_ps();
        for (int i = 0; i < L1; i += 16) {
            acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(row + i),
                                   _mm256_loadu_ps(a0 + i), acc0);
            acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(row + i + 8),
                                   _mm256_loadu_ps(a0 + i + 8), acc1);
        }
        for (int i = 0; i < L1; i += 16) {
            acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(row + L1 + i),
                                   _mm256_loadu_ps(a1 + i), acc0);
            acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(row + L1 + i + 8),
                                   _mm256_loadu_ps(a1 + i + 8), acc1);
        }
        const __m256 sum8 = _mm256_add_ps(acc0, acc1);
        __m128 lo = _mm256_castps256_ps128(sum8);
        __m128 hi = _mm256_extractf128_ps(sum8, 1);
        __m128 s4 = _mm_add_ps(lo, hi);
        s4 = _mm_add_ps(s4, _mm_movehl_ps(s4, s4));
        s4 = _mm_add_ss(s4, _mm_shuffle_ps(s4, s4, 1));
        h[j] = relu(_mm_cvtss_f32(s4) + n.b2[j]);
    }
#else
    for (int j = 0; j < L2; ++j) {
        const float* row = &n.w2[size_t(j) * 2 * L1];
        float sv = n.b2[j];
        for (int i = 0; i < L1; ++i) sv += row[i] * a0[i] + row[L1 + i] * a1[i];
        h[j] = relu(sv);
    }
#endif

    float out = n.b3;
#if defined(__AVX2__)
    {
        __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
        for (int j = 0; j < L2; j += 16) {
            acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(&n.w3[j]), _mm256_loadu_ps(&h[j]), acc0);
            acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(&n.w3[j + 8]), _mm256_loadu_ps(&h[j + 8]), acc1);
        }
        const __m256 s8 = _mm256_add_ps(acc0, acc1);
        __m128 lo = _mm256_castps256_ps128(s8);
        __m128 hi = _mm256_extractf128_ps(s8, 1);
        __m128 s4 = _mm_add_ps(lo, hi);
        s4 = _mm_add_ps(s4, _mm_movehl_ps(s4, s4));
        s4 = _mm_add_ss(s4, _mm_shuffle_ps(s4, s4, 1));
        out += _mm_cvtss_f32(s4);
    }
#else
    for (int j = 0; j < L2; ++j) out += n.w3[j] * h[j];
#endif

    // The net predicts q = cp / scale, so multiply the calibration back in.
    float cp = out * n.scale;
    if (cp > 29000.0f) cp = 29000.0f;
    if (cp < -29000.0f) cp = -29000.0f;
    return int(std::lround(cp));
}

int evaluateBlended(const Position& pos, const Accumulator& acc, int nnueWeightPct) {
    const int nn = evaluate(pos, acc);
    if (nnueWeightPct >= 100) return nn;
    const int hce = Eval::evaluate(pos);
    return (nn * nnueWeightPct + hce * (100 - nnueWeightPct)) / 100;
}

}  // namespace NNUE
