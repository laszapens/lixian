// Lixian (离弦) - evaluation.
#pragma once

#include "position.h"

namespace Eval {
// 100 = neutral, higher values make the engine prize enemy-king attacks and
// de-emphasise its own king safety. Default is intentionally sharp.
extern int aggression;
int evaluate(const Position& pos);
}  // namespace Eval
