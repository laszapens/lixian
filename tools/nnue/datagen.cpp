// Lixian (离弦) - self-play data generator (standalone, no Python needed).
//
// Written for the supercomputer login/compute nodes, which have gcc 7.3 and no
// Python 3. Talks to Stockfish over UCI pipes exactly like the Python version,
// but with real threads instead of processes and a much leaner per-position
// path.
//
// Record layout must stay in sync with tools/nnue/trainer.cpp:
//   u64 occ[2]; u64 piece[12]; i16 cp; i8 result; i8 turn; u8 pad[4];  // 120 B
//
// Usage:
//   ./datagen --sf /path/to/stockfish --openings openings.txt \
//             --out data --workers 32 --minutes 120
#include <algorithm>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int RECORD_SIZE = 120;

struct Record {
    uint64_t occ[2];
    uint64_t piece[12];
    int16_t cp;
    int8_t result;
    int8_t turn;
    uint8_t pad[4];
};
static_assert(sizeof(Record) == RECORD_SIZE, "record must stay 120 bytes");

// ---------------------------------------------------------------------------
// Minimal UCI driver
// ---------------------------------------------------------------------------
class Engine {
public:
    bool start(const std::string& path, int hashMb) {
        int inPipe[2], outPipe[2];
        if (pipe(inPipe) || pipe(outPipe)) return false;
        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            dup2(inPipe[0], STDIN_FILENO);
            dup2(outPipe[1], STDOUT_FILENO);
            close(inPipe[0]);
            close(inPipe[1]);
            close(outPipe[0]);
            close(outPipe[1]);
            if (std::freopen("/dev/null", "w", stderr) == nullptr) {}
            execl(path.c_str(), path.c_str(), (char*)nullptr);
            _exit(127);
        }
        close(inPipe[0]);
        close(outPipe[1]);
        fdIn_ = inPipe[1];
        fdOut_ = outPipe[0];
        in_ = fdopen(fdIn_, "w");
        out_ = fdopen(fdOut_, "r");
        if (!in_ || !out_) return false;
        send("uci");
        if (!waitFor("uciok")) return false;
        send("setoption name Threads value 1");
        send("setoption name Hash value " + std::to_string(hashMb));
        send("setoption name UCI_LimitStrength value false");
        return ready();
    }

    void stop() {
        if (in_) {
            send("quit");
            std::fclose(in_);
            in_ = nullptr;
        }
        if (out_) {
            std::fclose(out_);
            out_ = nullptr;
        }
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }

    std::string bestMove(const std::string& fen, int nodes) {
        send("position fen " + fen);
        send("go nodes " + std::to_string(nodes));
        std::string line;
        while (readLine(line)) {
            if (line.compare(0, 8, "bestmove") == 0) {
                const size_t sp = line.find(' ');
                if (sp == std::string::npos) return "";
                std::string mv = line.substr(sp + 1);
                const size_t sp2 = mv.find(' ');
                if (sp2 != std::string::npos) mv = mv.substr(0, sp2);
                return mv == "(none)" ? "" : mv;
            }
        }
        return "";
    }

    bool label(const std::string& fen, int nodes, int& cp) {
        send("position fen " + fen);
        send("go nodes " + std::to_string(nodes));
        std::string line;
        bool got = false;
        while (readLine(line)) {
            if (line.compare(0, 4, "info") == 0) {
                const size_t p = line.find(" score ");
                if (p != std::string::npos) {
                    size_t i = p + 7;
                    const size_t sp = line.find(' ', i);
                    if (sp != std::string::npos) {
                        const std::string kind = line.substr(i, sp - i);
                        i = sp + 1;
                        const size_t sp2 = line.find(' ', i);
                        const std::string val =
                            line.substr(i, sp2 == std::string::npos ? i : sp2 - i);
                        if (kind == "cp") {
                            cp = std::atoi(val.c_str());
                            got = true;
                        } else if (kind == "mate") {
                            const int m = std::atoi(val.c_str());
                            cp = m > 0 ? 20000 - std::abs(m) * 10
                                       : -(20000 - std::abs(m) * 10);
                            got = true;
                        }
                    }
                }
            } else if (line.compare(0, 8, "bestmove") == 0) {
                return got;
            }
        }
        return false;
    }

private:
    void send(const std::string& s) {
        std::fputs(s.c_str(), in_);
        std::fputc('\n', in_);
        std::fflush(in_);
    }
    bool readLine(std::string& out) {
        out.clear();
        int ch;
        while ((ch = std::fgetc(out_)) != EOF) {
            if (ch == '\n') {
                if (!out.empty() && out.back() == '\r') out.pop_back();
                return true;
            }
            out.push_back(char(ch));
        }
        return false;
    }
    bool waitFor(const char* token) {
        std::string line;
        while (readLine(line))
            if (line.compare(0, std::strlen(token), token) == 0) return true;
        return false;
    }
    bool ready() {
        send("isready");
        return waitFor("readyok");
    }

    int fdIn_ = -1, fdOut_ = -1;
    FILE* in_ = nullptr;
    FILE* out_ = nullptr;
    pid_t pid_ = -1;
};

// ---------------------------------------------------------------------------
// Position handling. A tiny FEN-level move maker is all we need: Stockfish
// decides the moves, we only have to replay them to get the resulting FEN.
// ---------------------------------------------------------------------------
struct Board {
    char sq[64];  // 'P','n', ... or 0
    int side = 0;  // 0 white, 1 black
    int castling = 0;
    int ep = -1;
    int halfmove = 0;
    int fullmove = 1;
    std::vector<std::string> history;

    void reset() {
        std::memset(sq, 0, sizeof(sq));
        const char* back = "RNBQKBNR";
        const char* pawns = "PPPPPPPP";
        for (int i = 0; i < 8; ++i) {
            sq[i] = back[i];
            sq[8 + i] = pawns[i];
            sq[48 + i] = char(pawns[i] - 'A' + 'a');
            sq[56 + i] = char(back[i] - 'A' + 'a');
        }
        side = 0;
        castling = 0xF;
        ep = -1;
        halfmove = 0;
        fullmove = 1;
        history.clear();
    }

    std::string fen() const {
        std::string out;
        for (int r = 7; r >= 0; --r) {
            int empty = 0;
            for (int f = 0; f < 8; ++f) {
                const char c = sq[r * 8 + f];
                if (!c) {
                    ++empty;
                } else {
                    if (empty) {
                        out += char('0' + empty);
                        empty = 0;
                    }
                    out += c;
                }
            }
            if (empty) out += char('0' + empty);
            if (r) out += '/';
        }
        out += side ? " b " : " w ";
        if (!castling) out += '-';
        if (castling & 1) out += 'K';
        if (castling & 2) out += 'Q';
        if (castling & 4) out += 'k';
        if (castling & 8) out += 'q';
        out += ' ';
        if (ep < 0)
            out += '-';
        else {
            out += char('a' + (ep & 7));
            out += char('1' + (ep >> 3));
        }
        out += ' ' + std::to_string(halfmove) + ' ' + std::to_string(fullmove);
        return out;
    }

    void setFen(const std::string& fen) {
        std::memset(sq, 0, sizeof(sq));
        side = castling = 0;
        ep = -1;
        halfmove = 0;
        fullmove = 1;
        history.clear();
        int r = 7, f = 0;
        size_t i = 0;
        for (; i < fen.size() && fen[i] != ' '; ++i) {
            const char c = fen[i];
            if (c == '/') {
                --r;
                f = 0;
            } else if (c >= '1' && c <= '8') {
                f += c - '0';
            } else {
                if (r >= 0 && f < 8) sq[r * 8 + f] = c;
                ++f;
            }
        }
        while (i < fen.size() && fen[i] == ' ') ++i;
        side = (i < fen.size() && fen[i] == 'b') ? 1 : 0;
        while (i < fen.size() && fen[i] != ' ') ++i;
        while (i < fen.size() && fen[i] == ' ') ++i;
        for (; i < fen.size() && fen[i] != ' '; ++i) {
            if (fen[i] == 'K') castling |= 1;
            else if (fen[i] == 'Q') castling |= 2;
            else if (fen[i] == 'k') castling |= 4;
            else if (fen[i] == 'q') castling |= 8;
        }
        while (i < fen.size() && fen[i] == ' ') ++i;
        if (i < fen.size() && fen[i] != '-') ep = (fen[i] - 'a') + (fen[i + 1] - '1') * 8;
        while (i < fen.size() && fen[i] != ' ') ++i;
        if (i < fen.size()) halfmove = std::atoi(fen.c_str() + i);
    }

    // Applies a UCI move. Returns false if it does not look legal on this board.
    bool push(const std::string& uci) {
        if (uci.size() < 4) return false;
        const int from = (uci[0] - 'a') + (uci[1] - '1') * 8;
        const int to = (uci[2] - 'a') + (uci[3] - '1') * 8;
        if (from < 0 || from > 63 || to < 0 || to > 63) return false;
        const char pc = sq[from];
        if (!pc) return false;

        history.push_back(fen());
        const bool white = side == 0;
        const char promo = uci.size() > 4 ? uci[4] : 0;
        const bool capture = sq[to] != 0;

        // En passant capture removes the pawn behind the target square.
        if ((pc == 'P' || pc == 'p') && (to & 7) != (from & 7) && !capture) {
            const int capSq = white ? to - 8 : to + 8;
            sq[capSq] = 0;
        }

        // Castling moves the rook as well.
        if ((pc == 'K' || pc == 'k') && std::abs((to & 7) - (from & 7)) == 2) {
            if (to > from) {
                sq[from + 1] = sq[from + 3];
                sq[from + 3] = 0;
            } else {
                sq[from - 1] = sq[from - 4];
                sq[from - 4] = 0;
            }
        }

        sq[to] = pc;
        sq[from] = 0;
        if (promo) sq[to] = white ? char(std::toupper(promo)) : char(std::tolower(promo));

        // Castling rights.
        if (pc == 'K') castling &= ~3;
        if (pc == 'k') castling &= ~12;
        if (from == 0 || to == 0) castling &= ~2;
        if (from == 7 || to == 7) castling &= ~1;
        if (from == 56 || to == 56) castling &= ~8;
        if (from == 63 || to == 63) castling &= ~4;

        // En passant target.
        ep = -1;
        if ((pc == 'P' || pc == 'p') && std::abs(to - from) == 16) ep = (from + to) / 2;

        if (pc == 'P' || pc == 'p' || capture)
            halfmove = 0;
        else
            ++halfmove;
        if (side == 1) ++fullmove;
        side ^= 1;
        return true;
    }

    void pop() {
        if (history.empty()) return;
        setFen(history.back());
    }

    Record encode(int cp, int result) const {
        Record r{};
        for (int s = 0; s < 64; ++s) {
            const char c = sq[s];
            if (!c) continue;
            const bool white = c >= 'A' && c <= 'Z';
            char lower = white ? char(c - 'A' + 'a') : c;
            int pt;
            switch (lower) {
                case 'p': pt = 0; break;
                case 'n': pt = 1; break;
                case 'b': pt = 2; break;
                case 'r': pt = 3; break;
                case 'q': pt = 4; break;
                default: pt = 5; break;
            }
            const int piece = pt + (white ? 0 : 6);
            const uint64_t b = 1ULL << s;
            r.piece[piece] |= b;
            if (white)
                r.occ[0] |= b;
            else
                r.occ[1] |= b;
        }
        r.cp = int16_t(std::max(-30000, std::min(30000, cp)));
        r.result = int8_t(result);
        r.turn = int8_t(side);
        return r;
    }
};

// ---------------------------------------------------------------------------
// Openings: one SAN-free line per line, as space separated UCI moves.
// ---------------------------------------------------------------------------
std::vector<std::vector<std::string>> loadOpenings(const std::string& path) {
    std::vector<std::vector<std::string>> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::vector<std::string> moves;
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
            const size_t start = i;
            while (i < line.size() && !std::isspace((unsigned char)line[i])) ++i;
            if (i > start) {
                std::string mv = line.substr(start, i - start);
                if (mv.size() >= 4 && mv[0] >= 'a' && mv[0] <= 'h') moves.push_back(mv);
            }
        }
        if (moves.size() >= 4 && moves.size() <= 16) out.push_back(moves);
    }
    return out;
}

std::atomic<long> g_positions{0};
std::atomic<long> g_games{0};
std::mutex g_mutex;

void worker(int id, const std::string& sfPath, const std::string& outDir,
            const std::vector<std::vector<std::string>>& openings, int gamesTarget,
            int playNodes, int jitter, int labelNodes, int maxPlies, int seed,
            std::atomic<bool>& stopAll) {
    std::mt19937 rng(uint32_t(seed + id * 7919));
    Engine player, labeler;
    if (!player.start(sfPath, 64) || !labeler.start(sfPath, 128)) {
        std::lock_guard<std::mutex> lk(g_mutex);
        std::fprintf(stderr, "worker %d: failed to start stockfish\n", id);
        return;
    }

    char path[512];
    std::snprintf(path, sizeof(path), "%s/shard%03d.bin", outDir.c_str(), id);
    FILE* out = std::fopen(path, "wb");
    if (!out) {
        std::lock_guard<std::mutex> lk(g_mutex);
        std::fprintf(stderr, "worker %d: cannot open %s\n", id, path);
        return;
    }
    std::vector<char> buffer;
    buffer.reserve(RECORD_SIZE * 4096);

    int games = 0;
    while (games < gamesTarget && !stopAll.load()) {
        const auto& book = openings[rng() % openings.size()];
        Board b;
        b.reset();
        const int trunc = std::min<int>(book.size(), 4 + int(rng() % (book.size() - 3)));
        for (int i = 0; i < trunc; ++i) b.push(book[i]);

        const int recordFrom = int(b.history.size());
        std::vector<std::pair<Board, int>> stored;  // (position, cp)
        int plies = recordFrom;
        while (plies < maxPlies) {
            const std::string fen = b.fen();
            int cp = 0;
            if (plies >= recordFrom && labeler.label(fen, labelNodes, cp) && std::abs(cp) <= 2500)
                stored.push_back({b, cp});
            const std::string mv = player.bestMove(fen, playNodes + int(rng() % uint32_t(jitter + 1)));
            if (mv.empty() || !b.push(mv)) break;
            ++plies;
        }

        // Adjudicate: material lead decides, otherwise draw.
        int diff = 0;
        for (int s = 0; s < 64; ++s) {
            const char c = b.sq[s];
            if (!c) continue;
            const bool white = c >= 'A' && c <= 'Z';
            const char lower = white ? char(c - 'A' + 'a') : c;
            int v = 0;
            switch (lower) {
                case 'p': v = 1; break;
                case 'n': v = 3; break;
                case 'b': v = 3; break;
                case 'r': v = 5; break;
                case 'q': v = 9; break;
                default: break;
            }
            diff += white ? v : -v;
        }
        const int result = std::abs(diff) >= 5 ? (diff > 0 ? 1 : -1) : 0;

        for (auto& pr : stored) {
            const int sideResult = pr.first.side == 0 ? result : -result;
            const Record r = pr.first.encode(pr.second, sideResult);
            const char* bytes = reinterpret_cast<const char*>(&r);
            buffer.insert(buffer.end(), bytes, bytes + RECORD_SIZE);
            if (buffer.size() >= RECORD_SIZE * 4096) {
                std::fwrite(buffer.data(), 1, buffer.size(), out);
                buffer.clear();
                std::fflush(out);
            }
            ++g_positions;
        }
        ++games;
        ++g_games;
    }
    if (!buffer.empty()) std::fwrite(buffer.data(), 1, buffer.size(), out);
    std::fclose(out);
    player.stop();
    labeler.stop();
    
}

}  // namespace

int main(int argc, char** argv) {
    std::string sfPath = "./stockfish";
    std::string openPath = "openings.txt";
    std::string outDir = "data";
    int workers = 8;
    int minutes = 60;
    int games = 1 << 30;
    int playNodes = 3000, jitter = 6000, labelNodes = 12000;
    int maxPlies = 260;
    int seed = 12345;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--sf") sfPath = next();
        else if (a == "--openings") openPath = next();
        else if (a == "--out") outDir = next();
        else if (a == "--workers") workers = std::stoi(next());
        else if (a == "--minutes") minutes = std::stoi(next());
        else if (a == "--games") games = std::stoi(next());
        else if (a == "--play-nodes") playNodes = std::stoi(next());
        else if (a == "--play-jitter") jitter = std::stoi(next());
        else if (a == "--label-nodes") labelNodes = std::stoi(next());
        else if (a == "--max-plies") maxPlies = std::stoi(next());
        else if (a == "--seed") seed = std::stoi(next());
    }

    const auto openings = loadOpenings(openPath);
    if (openings.empty()) {
        std::fprintf(stderr, "no openings loaded from %s\n", openPath.c_str());
        return 1;
    }
    std::printf("openings=%zu workers=%d sf=%s out=%s\n", openings.size(), workers,
                sfPath.c_str(), outDir.c_str());
    std::fflush(stdout);

    std::atomic<bool> stopAll{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < workers; ++i)
        threads.emplace_back(worker, i, std::cref(sfPath), std::cref(outDir),
                             std::cref(openings), games, playNodes, jitter, labelNodes,
                             maxPlies, seed, std::ref(stopAll));

    const auto t0 = std::chrono::steady_clock::now();
    long lastPos = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        const auto el = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        const long pos = g_positions.load();
        std::printf("[%6lds] games=%ld pos=%ld rate=%.0f/s\n", el, g_games.load(), pos,
                    double(pos - lastPos) / 20.0);
        std::fflush(stdout);
        lastPos = pos;
        if (el >= long(minutes) * 60) {
            stopAll.store(true);
            break;
        }
    }
    for (auto& t : threads) t.join();
    std::printf("done positions=%ld\n", g_positions.load());
    return 0;
}
