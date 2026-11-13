// Lixian (离弦) - opening book built from sharp, gambit-oriented lines.
#pragma once

#include "position.h"

#include <random>
#include <string>
#include <unordered_map>
#include <vector>

class Book {
public:
    Book();

    bool load(const std::string& path);
    bool loaded() const { return !entries.empty(); }
    size_t positionCount() const { return entries.size(); }

    Move probe(const Position& pos);
    bool enabled = true;

private:
    std::unordered_map<uint64_t, std::vector<std::pair<Move, uint16_t>>> entries;
    std::mt19937_64 rng;
};
