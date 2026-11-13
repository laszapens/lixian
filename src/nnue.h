// Lixian (离弦) - NNUE evaluation.
//
// Design: two-perspective feature-transformer-lite.
//   features  768 per perspective (12 piece types x 64 squares, mirrored)
//   L1        256 units per perspective, incrementally maintained
//   L2         32 units, ReLU on the concatenated 512-wide vector
//   output    a single head; a mg/eg split was tried first but the endgame head
//             never received enough signal and poisoned low-material positions
//
// Everything is float32. A quantised variant would save memory bandwidth, but
// the trainer and the engine must agree bit-for-bit or the net silently
// misbehaves; float makes that contract trivially verifiable and costs only
// ~1.6 MB of weights.
#pragma once

#include "position.h"
#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace NNUE {

constexpr int INPUT_DIM = 768;
constexpr int L1 = 256;
constexpr int L2 = 32;

struct Accumulator {
    float v[2][L1];
    // Cached ReLU of `v`, refreshed lazily by evaluate(). Keeping it here means
    // a node whose accumulator did not change costs only the 32x512 projection,
    // which is the dominant term.
    mutable float act[2][L1];
    mutable bool actValid = false;
};

struct Network {
    bool loaded = false;
    std::vector<float> w1;    // [INPUT_DIM][2 * L1]
    std::vector<float> b1;    // [2 * L1]
    std::vector<float> w2;    // [L2][2 * L1]
    std::vector<float> b2;    // [L2]
    std::vector<float> w3;  // [L2]
    float b3 = 0;
    // Score -> centipawn calibration: the net is trained on q = cp / scale.
    float scale = 400.0f;

    bool load(const std::string& path);
    void unload() { loaded = false; }
};

const Network& net();
bool available();
bool load(const std::string& path);

inline int featureIndex(Color persp, Color pieceColor, PieceType pt, Square sq) {
    const Square rel = (persp == WHITE) ? sq : flipRank(sq);
    const int block = (pieceColor == persp) ? int(pt) : 6 + int(pt);
    return rel + 64 * block;
}

void refresh(const Position& pos, Accumulator& acc);
void addPiece(Accumulator& acc, const Network& n, Color pc, PieceType pt, Square sq);
void removePiece(Accumulator& acc, const Network& n, Color pc, PieceType pt, Square sq);
void pushMove(const Position& pos, Move m, const Undo& u, Accumulator& acc);

// Centipawns from the side to move's point of view.
int evaluate(const Position& pos, const Accumulator& acc);

// Mixes the net with the hand-written evaluator. nnueWeightPct is 0..100.
int evaluateBlended(const Position& pos, const Accumulator& acc, int nnueWeightPct);

}  // namespace NNUE
