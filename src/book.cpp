#include "book.h"

#include "movegen.h"

#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

Book::Book() : rng(std::random_device{}()) {}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool Book::load(const std::string& path) {
    entries.clear();
    std::ifstream in(path);
    if (!in) return false;

    std::string line;
    int loadedLines = 0;
    int badLines = 0;
    while (std::getline(in, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;

        std::istringstream ss(line);
        std::string token;
        Position pos;
        pos.startpos();
        bool ok = true;
        while (ss >> token) {
            const Move m = pos.uciToMove(token);
            if (m == MOVE_NONE) {
                ok = false;
                break;
            }
            entries[pos.key].push_back({m, 1});
            Undo u;
            pos.makeMove(m, u);
        }
        if (ok)
            ++loadedLines;
        else
            ++badLines;
    }
    if (badLines)
        std::cerr << "info string book: skipped " << badLines << " invalid line(s)\n";
    std::cerr << "info string book: " << loadedLines << " lines, " << entries.size()
              << " positions from " << path << "\n";
    return !entries.empty();
}

Move Book::probe(const Position& pos) {
    if (!enabled || entries.empty()) return MOVE_NONE;
    const auto it = entries.find(pos.key);
    if (it == entries.end()) return MOVE_NONE;

    std::vector<std::pair<Move, uint16_t>> legal;
    int total = 0;
    for (const auto& e : it->second) {
        if (!pos.moveIsLegal(e.first)) continue;
        legal.push_back(e);
        total += e.second;
    }
    if (legal.empty()) return MOVE_NONE;
    int pick = int(rng() % uint64_t(total));
    for (const auto& e : legal) {
        if (pick < e.second) return e.first;
        pick -= e.second;
    }
    return legal.back().first;
}
