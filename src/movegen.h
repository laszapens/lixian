// Lixian (离弦) - move generation.
#pragma once

#include "position.h"
#include "types.h"

void generateAll(const Position& pos, MoveList& ml);
void generateCaptures(const Position& pos, MoveList& ml);
