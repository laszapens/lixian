// Lixian (离弦) - NNUE trainer.
//
// Architecture (mirrors src/nnue.h):
//     accumulators  a1[p][i] = b1[p][i] + sum_{f in features(p)} w1[f][i]
//     activation    a1c = relu(a1)
//     hidden        h[j] = relu( b2[j] + sum_i w2[i][j] * a1c[i] )
//     output        out  = b3 + sum_j w3[j] * h[j]        (separate mg/eg heads)
//     score         cp   = (mg * phase + eg * (24-phase)) / 24
//
// The target is q = cp / scale, regressed with plain MSE, where cp is the
// Stockfish search score blended with the game result.
//
// Pure C++ + OpenMP: the cluster has no Python 3 and no internet.
//
// Usage:
//   ./trainer --data DIR [--epochs 20] [--batch 256] [--lr 0.002]
//             [--out weights/nnue.bin] [--threads N]
#include <omp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr int INPUT_DIM = 768;
constexpr int L1 = 256;
constexpr int L2 = 32;
constexpr int RECORD_SIZE = 120;
constexpr int MAX_ACTIVE = 32;

struct Record {
    uint64_t occ[2];
    uint64_t piece[12];
    int16_t cp;
    int8_t result;
    int8_t turn;  // 0 = white to move, 1 = black to move
    uint8_t pad[4];
};
static_assert(sizeof(Record) == RECORD_SIZE, "record layout mismatch");

struct Sample {
    int16_t feat[2][MAX_ACTIVE];
    int8_t count[2];
    float target;
    int phase;   // tapered-eval phase, mirrors the engine's 0..24
    int turn;    // 0 = white to move, 1 = black to move
};

float g_scale = 400.0f;

// The net is trained to regress q = cp / scale directly. A pure result-based
// sigmoid target was tried first and plateaued, because a 3-outcome signal is
// far too coarse to teach positional nuance.
inline float sigmoid(float cp) { return 1.0f / (1.0f + std::exp(-cp / g_scale)); }

inline int featureIndex(int persp, int piece, int sq) {
    const int rel = persp == 0 ? sq : (sq ^ 56);
    const int block = (piece / 6 == persp) ? (piece % 6) : (6 + piece % 6);
    return rel + 64 * block;
}

void decode(const Record& r, Sample& s) {
    // The engine concatenates [side-to-move perspective, opponent perspective].
    // Feature index 0 (persp 0) therefore has to mean "the side to move", not
    // "white"; mixing the two teaches the net two contradictory functions.
    // Absolute perspectives: 0 = WHITE, 1 = BLACK, exactly like the engine's
    // accumulators. The side-to-move ordering only matters when concatenating
    // for the forward pass.
    s.count[0] = s.count[1] = 0;
    for (int piece = 0; piece < 12; ++piece) {
        uint64_t b = r.piece[piece];
        while (b) {
            const int sq = __builtin_ctzll(b);
            b &= b - 1;
            if (s.count[0] < MAX_ACTIVE)
                s.feat[0][s.count[0]++] = int16_t(featureIndex(0, piece, sq));
            if (s.count[1] < MAX_ACTIVE)
                s.feat[1][s.count[1]++] = int16_t(featureIndex(1, piece, sq));
        }
    }
    s.turn = (r.turn != 0) ? 1 : 0;
    // The search score is the teacher. A game result carries only one bit of
    // information per position and, at these weights, ended up contributing
    // more variance than the actual evaluation, which taught the net "who won"
    // instead of "what is good here" -- it happily hung pieces for material.
    const float qSearch = float(r.cp) / g_scale;
    const float qResult = float(r.result);
    s.target = 0.98f * qSearch + 0.02f * qResult;

    // Phase must match Eval/engine: 4*Q + 2*R + 1*B + 1*N, clamped to 0..24.
    const int q = __builtin_popcountll(r.piece[4] | r.piece[10]);
    const int rr = __builtin_popcountll(r.piece[3] | r.piece[9]);
    const int b = __builtin_popcountll(r.piece[2] | r.piece[8]);
    const int n = __builtin_popcountll(r.piece[1] | r.piece[7]);
    s.phase = std::min(24, 4 * q + 2 * rr + b + n);
}

struct Net {
    std::vector<float> w1;  // [INPUT_DIM][2*L1]
    std::vector<float> b1;  // [2*L1]
    std::vector<float> w2;  // [2*L1][L2]
    std::vector<float> b2;  // [L2]
    std::vector<float> w3;
    float b3 = 0;

    void init(std::mt19937& rng) {
        // 1/sqrt(768) keeps the accumulator's variance near 1 after summing the
        // 32 active features, so the ReLU does not kill the signal.
        std::normal_distribution<float> n1(0.0f, 1.0f / std::sqrt(float(INPUT_DIM)));
        // Fan-in of 2*L1 units: this std keeps the hidden pre-activation in the
        // same 0..255 range the engine clamps to.
        // Same 1/sqrt(fan-in) rule for the 512-wide hidden layer.
        std::normal_distribution<float> n2(0.0f, 1.0f / std::sqrt(float(2 * L1)));
        std::normal_distribution<float> n3(0.0f, 1.0f / std::sqrt(float(L2)));
        w1.assign(size_t(INPUT_DIM) * 2 * L1, 0.0f);
        for (auto& w : w1) w = n1(rng);
        b1.assign(2 * L1, 0.0f);
        w2.assign(size_t(2 * L1) * L2, 0.0f);
        for (auto& w : w2) w = n2(rng);
        b2.assign(L2, 0.0f);
        w3.assign(L2, 0.0f);
        for (auto& w : w3) w = n3(rng);
        // Start centred so the first predictions sit at 0.
        float sm = 0;
        for (int j = 0; j < L2; ++j) sm += w3[j];
        for (int j = 0; j < L2; ++j) w3[j] -= sm / L2;
    }
};

inline float relu(float x) { return x > 0 ? x : 0; }

// Sparse accumulator build: O(active features * L1), not O(INPUT_DIM * L1).
void buildAcc(const Net& net, const Sample& s, float* a1) {
    // `a1` holds [side-to-move perspective, opponent perspective]; each half is
    // accumulated from its own absolute perspective.
    const int persp[2] = {s.turn, s.turn ^ 1};
    for (int p = 0; p < 2; ++p) {
        float* dst = a1 + p * L1;
        const int abs = persp[p];
        std::memcpy(dst, &net.b1[size_t(abs) * L1], sizeof(float) * L1);
        for (int k = 0; k < s.count[abs]; ++k) {
            const int f = s.feat[abs][k];
            const float* row = &net.w1[size_t(f) * 2 * L1 + abs * L1];
            for (int i = 0; i < L1; ++i) dst[i] += row[i];
        }
        // No clamp: the engine applies a plain ReLU to the accumulator, so
        // clamping here would train a different function than the one shipped.
    }
}

struct Grads {
    // w1 is sparse: per sample only ~32 of the 768 feature rows fire, so the
    // dense 3 MB buffer was pure overhead. Rows are written through a stamp
    // array so clearing stays O(1).
    std::vector<float> dw1;
    std::vector<uint32_t> stamp1;
    uint32_t stampGen = 1;
    std::vector<int> touched;
    std::vector<float> db1, dw2, db2, dw3;
    float db3 = 0;

    void zero(const Net& n) {
        if (dw1.size() != n.w1.size()) {
            dw1.assign(n.w1.size(), 0.0f);
            stamp1.assign(n.w1.size() / (2 * L1), 0);
            stampGen = 1;
        } else {
            for (int row : touched) {
                float* base = &dw1[size_t(row) * 2 * L1];
                std::memset(base, 0, sizeof(float) * 2 * L1);
                stamp1[size_t(row)] = 0;
            }
        }
        touched.clear();
        db1.assign(n.b1.size(), 0.0f);
        dw2.assign(n.w2.size(), 0.0f);
        db2.assign(n.b2.size(), 0.0f);
        dw3.assign(L2, 0.0f);
        db3 = 0;
    }

    inline float* row1(int feature) {
        if (stamp1[size_t(feature)] != stampGen) {
            stamp1[size_t(feature)] = stampGen;
            touched.push_back(feature);
        }
        return &dw1[size_t(feature) * 2 * L1];
    }

    void addFrom(const Grads& o) {
        for (int row : o.touched) {
            float* dst = row1(row);
            const float* src = &o.dw1[size_t(row) * 2 * L1];
            for (int i = 0; i < 2 * L1; ++i) dst[i] += src[i];
        }
        for (size_t k = 0; k < db1.size(); ++k) db1[k] += o.db1[k];
        for (size_t k = 0; k < dw2.size(); ++k) dw2[k] += o.dw2[k];
        for (size_t k = 0; k < db2.size(); ++k) db2[k] += o.db2[k];
        for (int k = 0; k < L2; ++k) dw3[k] += o.dw3[k];
        db3 += o.db3;
    }
};

float trainOne(const Net& net, const Sample& s, Grads& g) {
    float a1[2 * L1];
    buildAcc(net, s, a1);
    for (int i = 0; i < 2 * L1; ++i) if (a1[i] < 0.0f) a1[i] = 0.0f;

    float h[L2];
    for (int j = 0; j < L2; ++j) {
        float acc = net.b2[j];
        for (int i = 0; i < 2 * L1; ++i) acc += net.w2[size_t(i) * L2 + j] * a1[i];
        h[j] = relu(acc);
    }

    float out = net.b3;
    for (int j = 0; j < L2; ++j) out += net.w3[j] * h[j];
    const float diff = out - s.target;
    const float dOut = 2.0f * diff;

    for (int j = 0; j < L2; ++j) g.dw3[j] += dOut * h[j];
    g.db3 += dOut;

    float dH[L2];
    for (int j = 0; j < L2; ++j)
        dH[j] = h[j] <= 0.0f ? 0.0f : dOut * net.w3[j];

    for (int i = 0; i < 2 * L1; ++i) {
        float dA = 0;
        for (int j = 0; j < L2; ++j) {
            g.dw2[size_t(i) * L2 + j] += dH[j] * a1[i];
            dA += dH[j] * net.w2[size_t(i) * L2 + j];
        }
        if (a1[i] <= 0.0f) continue;  // ReLU
        const int p = i / L1;
        const int unit = i % L1;
        g.db1[size_t(p) * L1 + unit] += dA;
        for (int k = 0; k < s.count[p]; ++k) {
            const int f = s.feat[p][k];
            g.row1(f)[p * L1 + unit] += dA;
        }
    }
    return diff * diff;
}

void writeNet(const Net& net, const std::string& path, float scale) {
    std::ofstream f(path, std::ios::binary);
    const char magic[8] = {'L', 'X', 'N', 'N', '2', '\0', '\0', '\0'};
    f.write(magic, 8);
    const int32_t dims[3] = {INPUT_DIM, L1, L2};
    f.write(reinterpret_cast<const char*>(dims), sizeof(dims));
    auto writeArr = [&](const std::vector<float>& v) {
        f.write(reinterpret_cast<const char*>(v.data()),
                std::streamsize(v.size() * sizeof(float)));
    };
    writeArr(net.w1);
    writeArr(net.b1);
    // The engine stores w2 as [j][i]; the trainer keeps [i][j].
    std::vector<float> w2(net.w2.size());
    for (int j = 0; j < L2; ++j)
        for (int i = 0; i < 2 * L1; ++i) w2[size_t(j) * 2 * L1 + i] = net.w2[size_t(i) * L2 + j];
    writeArr(w2);
    writeArr(net.b2);
    writeArr(net.w3);
    const float tail[2] = {net.b3, scale};
    f.write(reinterpret_cast<const char*>(tail), sizeof(tail));
}

std::vector<std::string> listShards(const std::string& dir) {
    std::vector<std::string> out;
    for (int i = 0; i < 1000; ++i) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s/shard%03d.bin", dir.c_str(), i);
        std::ifstream f(buf, std::ios::binary);
        if (f) out.push_back(buf);
    }
    return out;
}

struct Adam {
    std::vector<float> m, v;
    void ensure(size_t n) {
        if (m.size() != n) {
            m.assign(n, 0.0f);
            v.assign(n, 0.0f);
        }
    }
    void step(float* w, const float* grad, size_t n, float lr, long t) {
        const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
        const float c1 = 1.0f - std::pow(b1, float(t));
        const float c2 = 1.0f - std::pow(b2, float(t));
        for (size_t k = 0; k < n; ++k) {
            const float gk = grad[k];
            m[k] = b1 * m[k] + (1 - b1) * gk;
            v[k] = b2 * v[k] + (1 - b2) * gk * gk;
            w[k] -= lr * (m[k] / c1) / (std::sqrt(v[k] / c2) + eps);
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string dataDir = "data";
    std::string outPath = "weights/nnue.bin";
    int epochs = 20;
    int batch = 256;
    float lr = 0.002f;
    int threads = omp_get_max_threads();
    long maxSamples = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--data") dataDir = next();
        else if (a == "--out") outPath = next();
        else if (a == "--epochs") epochs = std::stoi(next());
        else if (a == "--batch") batch = std::stoi(next());
        else if (a == "--lr") lr = std::stof(next());
        else if (a == "--threads") threads = std::stoi(next());
        else if (a == "--max-samples") maxSamples = std::stol(next());
        else if (a == "--scale") g_scale = std::stof(next());
    }
    omp_set_num_threads(threads);

    const auto shards = listShards(dataDir);
    if (shards.empty()) {
        std::fprintf(stderr, "no shards found in %s\n", dataDir.c_str());
        return 1;
    }
    size_t total = 0;
    std::vector<std::string> files;
    for (const auto& s : shards) {
        std::ifstream f(s, std::ios::binary | std::ios::ate);
        const size_t bytes = size_t(f.tellg());
        const size_t n = bytes / RECORD_SIZE;
        if (n == 0) continue;
        files.push_back(s);
        total += n;
    }
    if (maxSamples > 0 && size_t(maxSamples) < total) total = size_t(maxSamples);
    std::printf("shards=%zu samples=%zu batch=%d threads=%d lr=%.4f\n", files.size(), total,
                batch, threads, lr);
    std::fflush(stdout);

    std::mt19937 rng(1234);
    Net net;
    net.init(rng);

    Adam aW1, aB1, aW2, aB2, aW3, aB3;
    aW1.ensure(net.w1.size());
    aB1.ensure(net.b1.size());
    aW2.ensure(net.w2.size());
    aB2.ensure(net.b2.size());
    aW3.ensure(L2);
    aB3.ensure(1);

    long step = 0;
    std::vector<Record> buf;
    buf.reserve(size_t(batch));
    std::vector<Sample> samples(static_cast<size_t>(batch));

    for (int epoch = 1; epoch <= epochs; ++epoch) {
        double epochLoss = 0;
        long seen = 0;
        bool stop = false;

        for (const auto& path : files) {
            std::ifstream f(path, std::ios::binary);
            if (!f) continue;
            Record r;
            while (!stop && f.read(reinterpret_cast<char*>(&r), RECORD_SIZE)) {
                buf.push_back(r);
                if (int(buf.size()) < batch) continue;

                const int n = int(buf.size());
                for (int i = 0; i < n; ++i) decode(buf[i], samples[size_t(i)]);

                // Each thread accumulates into a private buffer; the merge
                // happens once per batch instead of once per sample.
                const int nthreads = std::max(1, threads);
                std::vector<Grads> locals(static_cast<size_t>(nthreads));
                double lossSum = 0;
                #pragma omp parallel num_threads(nthreads)
                {
                    Grads& lg = locals[size_t(omp_get_thread_num())];
                    lg.zero(net);
                    double ls = 0;
                    #pragma omp for schedule(static)
                    for (int i = 0; i < n; ++i) ls += trainOne(net, samples[size_t(i)], lg);
                    #pragma omp critical
                    lossSum += ls;
                }
                Grads g;
                g.zero(net);
                for (const auto& lg : locals) g.addFrom(lg);

                const float inv = 1.0f / float(n);
                for (int row : g.touched) {
                    float* base = &g.dw1[size_t(row) * 2 * L1];
                    for (int i = 0; i < 2 * L1; ++i) base[i] *= inv;
                }
                for (auto& x : g.db1) x *= inv;
                for (auto& x : g.dw2) x *= inv;
                for (auto& x : g.db2) x *= inv;
                for (auto& x : g.dw3) x *= inv;
                g.db3 *= inv;

                ++step;
                // Slow inverse-time decay: 2% by step 1000, 20% by step 12000.
                const float lrNow = lr / (1.0f + float(step) / 50000.0f);
                aW1.step(net.w1.data(), g.dw1.data(), net.w1.size(), lrNow, step);
                aB1.step(net.b1.data(), g.db1.data(), net.b1.size(), lrNow, step);
                aW2.step(net.w2.data(), g.dw2.data(), net.w2.size(), lrNow, step);
                aB2.step(net.b2.data(), g.db2.data(), net.b2.size(), lrNow, step);
                aW3.step(net.w3.data(), g.dw3.data(), L2, lrNow, step);
                aB3.step(&net.b3, &g.db3, 1, lrNow, step);

                epochLoss += lossSum;
                seen += n;
                if (step % 20 == 0 || step < 5) {
                    std::printf("step %ld loss %.6f lr %.6f\n", step, lossSum / n, lrNow);
                    std::fflush(stdout);
                }
                buf.clear();
                if (maxSamples > 0 && seen >= maxSamples) {
                    stop = true;
                    break;
                }
            }
            if (stop) break;
        }

        std::printf("epoch %d loss %.6f samples %ld\n", epoch,
                    seen ? epochLoss / double(seen) : 0.0, seen);
        std::fflush(stdout);
        writeNet(net, outPath, g_scale);
        std::printf("wrote %s\n", outPath.c_str());
        std::fflush(stdout);
        if (stop) break;
    }
    return 0;
}
