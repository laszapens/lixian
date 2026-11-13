// Lixian (离弦) - Syzygy tablebase probing interface.
#pragma once

#include "position.h"
#include "types.h"

#include <string>

namespace TB {

// Loads every table it can find in `path` (':' separated allowed).
// Returns true when at least one table is usable.
bool init(const std::string& path);
void shutdown();
bool ready();

// WDL from the side to move's point of view:
//   -2 loss, -1 blessed loss, 0 draw, +1 cursed win, +2 win.
// Returns false when the position is not covered (too many pieces, castling
// rights, or en passant with a nonzero halfmove clock).
bool probeWdl(const Position& pos, int& wdl);

// Tablebase-optimal root move plus its WDL. Only meaningful at the root.
bool probeRoot(const Position& pos, Move& move, int& wdl);

}  // namespace TB
