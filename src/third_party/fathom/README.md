# Fathom / Syzygy tablebase probing (vendored)

This directory holds a vendored copy of [Fathom](https://github.com/jdart1/Fathom),
the Syzygy tablebase probing library originally by Ronald de Man and basil00,
maintained by Jon Dart. It lets the engine answer "what is the exact result of
this position?" for any position with at most 7 pieces, from the `.rtbw` (WDL)
and `.rtbz` (DTZ) files on disk.

Everything in this directory is third-party code except `tb_glue.h` /
`tb_glue.cpp`, which are ours.

---

## 1. What was vendored

Copied verbatim from `Fathom-master/src/`, byte-identical to upstream:

| File           | Role |
| -------------- | ---- |
| `tbprobe.c`    | The whole prober: file mapping, decompression, move generation, probing. |
| `tbprobe.h`    | Public API, the `TB_RESULT` packing macros, `TB_LARGEST`, WDL constants. |
| `tbchess.c`    | **Not a translation unit.** It is a fragment `#include`d textually from `tbprobe.c` (see `tbprobe.c:433`). Do not compile it on its own. |
| `tbconfig.h`   | Compile-time configuration. Left at upstream defaults. |
| `stdendian.h`  | Portable endianness helpers (of Sebastian Foerster / Mathias Panzenboeck origin). |
| `LICENSE`      | MIT. |

`apps/` is upstream's standalone CLI demo. It is unused by the engine; keep or
delete it freely.

Written by us:

| File | Role |
| ---- | ---- |
| `tb_glue.h`   | Small, crash-safe C++ wrapper: `TB_init`, `TB_free`, `TB_probe_wdl_simple`, `TB_probe_root_simple`. |
| `tb_glue.cpp` | Implementation, including a pre-flight legality guard. |

### Why `tbchess.c` must not be compiled separately

It has no `#include`s of its own and relies on the enclosing `tbprobe.c` for
`uint16_t`, `bool`, `TbMove`, `gen_moves`, `do_move`, and the attack helpers.
Compiling it standalone fails immediately:

```
tbchess.c:1034:8: error: unknown type name 'TbMove'
```

So `tbprobe.c` is the only C file to compile.

---

## 2. Exact compile command

`tbprobe.c` is C11 and must be compiled by the **C** compiler. It uses
`<stdatomic.h>`, `_Atomic`, `_Thread_local`, and C99 designated initialisers,
none of which a C++ compiler will accept.

Verified working (gcc 15.2.0, x86-64 Linux):

```sh
gcc -std=c11 -O2 -DNDEBUG -fPIC \
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
    -I src/third_party/fathom \
    -c -o build/fathom_tbprobe.o \
    src/third_party/fathom/tbprobe.c
```

Notes on each flag:

* `-std=c11` — `gnu11` and `gnu99` (upstream's own choice) also work. Plain
  `c99` works too. Do **not** use `-std=c++*`.
* `-DNDEBUG` — **strongly recommended.** Without it, `assert(b != 0)` inside
  `lsb()` fires on illegal positions. With it, that same path becomes a
  segfault instead, so either way the position must be validated first — which
  is what `tb_glue.cpp` does. `-DNDEBUG` also matches the engine's own flags.
* `-fPIC` — only needed if this object ends up in a shared library. Harmless
  otherwise.
* `-Wno-sign-compare` — silences exactly one benign upstream warning in
  `tb_expand_mate()` (`int` vs `unsigned` comparison). Tested: it is the only
  warning under `-Wall -Wextra`.
* `-pthread` is **required at link time**, not compile time; see below.
* `-lm` is **not** needed — Fathom does not call into libm.

`-O3 -march=native -flto` also compiles and links cleanly (verified). LTO works
across the gcc/g++ boundary.

The glue is ordinary C++20:

```sh
g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wno-unused-parameter \
    -I src/third_party/fathom \
    -c -o build/fathom_tb_glue.o \
    src/third_party/fathom/tb_glue.cpp
```

---

## 3. How to link

The C objects are plain C; the C++ side sees them through `extern "C"`. Both
headers already carry the `extern "C"` guard, so this is all that is needed:

```cpp
#include "tb_glue.h"          // our wrapper (recommended)
// or, for the raw API:
extern "C" {
#include "tbprobe.h"
}
```

Link line:

```sh
g++ -std=c++20 -O2 -DNDEBUG -pthread \
    -o lixian build/*.o build/fathom_tbprobe.o build/fathom_tb_glue.o
```

`-pthread` is not optional: when `TB_NO_THREADS` is undefined (the default we
want, so probes are safe from multiple search threads once initialised),
`tbprobe.c` protects lazy table mapping with a `pthread_mutex_t` and pulls in
`<pthread.h>`. Omitting it gives undefined references on some toolchains.

For the engine's top-level `Makefile`, the minimal integration is to add
`src/third_party/fathom/tbprobe.c` and `tb_glue.cpp` to the source list and
give the C file its own rule, because it must **not** be compiled with
`$(CXX)`:

```make
build/fathom_tbprobe.o: src/third_party/fathom/tbprobe.c
	@mkdir -p build
	gcc -std=c11 -O3 -march=native -DNDEBUG -fPIC -pthread \
	    -Isrc/third_party/fathom -c -o $@ $<

build/fathom_tb_glue.o: src/third_party/fathom/tb_glue.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc/third_party/fathom -c -o $@ $<
```

`tests/tbtest_Makefile` is a working, self-contained example of exactly this.
It is deliberately separate from the top-level `Makefile`.

### Exported symbols

`tbprobe.o` exports 25 globals. None collide with a typical engine's names, but
note that `encode`, `first_ei`, `leading_pawn`, and `num_tables` are generic
enough that they *could* clash; they are non-`static` in upstream. If a clash
appears, compile with `-fvisibility=hidden` and expose only `tb_*`:

```sh
gcc ... -fvisibility=hidden -fvisibility-inlines-hidden ...
```

(The `tb_*` and `TB_*` symbols are the ones the engine actually calls.)

---

## 4. Verified side-to-move convention

**`tb_probe_wdl()` returns a value relative to the SIDE TO MOVE, not to White.**

This was established from the source and then confirmed empirically.

### From the source

`tbprobe.c:1959-1965` documents `probe_wdl()`:

```
// The return value is from the point of view of the side to move:
// -2 : loss
// -1 : loss, but draw under 50-move rule
//  0 : draw
//  1 : win, but draw under 50-move rule
//  2 : win
```

`tb_probe_wdl_impl()` passes `turn` straight into `Pos.turn` and adds 2 to the
result, so the public `tb_probe_wdl()` returns `0..4` — the codes
`TB_LOSS`, `TB_BLESSED_LOSS`, `TB_DRAW`, `TB_CURSED_WIN`, `TB_WIN`, which are
**also** side-to-move relative (`TB_WIN` means *the side to move wins*).

Mirroring does occur inside `probe_table()`, but only to select which colour's
half of an asymmetric table to index; it never flips the sign of the returned
value. `Pos.turn` is plain `true = white to move`, and `calc_key()` treats the
white/black bitboards literally.

### Measured, on real 3-4-5 tables

| Position | `turn` | `tb_probe_wdl` | Meaning |
| --- | --- | --- | --- |
| wK a1 + Q b1, bK c3 | white | 4 (`TB_WIN`) | White wins |
| wK a1 + Q b1, bK c3 | black | 0 (`TB_LOSS`) | Black, to move, loses |
| wK a1, bK c3 + **black** pawn c2 | white | 0 (`TB_LOSS`) | **White** loses |
| wK a1, bK c3 + black pawn c2 | black | 4 (`TB_WIN`) | **Black** wins |
| wK a1 + Q b2, bK d3 | white | 4 (`TB_WIN`) | White wins |
| wK c6 + P c7, bK a8 | white | 4 (`TB_WIN`) | White wins (promotes) |
| wK c6 + P c7, bK a8 | black | 0 (`TB_LOSS`) | Black loses |
| KNvK | either | 2 (`TB_DRAW`) | Draw |
| KvK | either | 2 (`TB_DRAW`) | Draw |

Row 3 is the decisive one: the side *not* moving is the one winning, and the
reported value follows whichever side is to move. A "positive means White
wins" reading would have to return `TB_WIN` there, and it does not.

`tb_probe_root()` follows the same convention: `TB_GET_WDL()` is side-to-move
relative, and the returned move is always a move **for the side to move**.

### Practical rule for engine code

```
positive  -> good for the player whose turn it is
negative  -> bad  for the player whose turn it is
```

When converting to a White-relative score (as search normally wants), negate
the result if it is Black to move.

`TB_GET_WDL()` on a root result yields `0..4`, **not** `-2..+2`. Compare
against `TB_WIN` / `TB_DRAW` / `TB_LOSS` (and their `BLESSED`/`CURSED`
variants), or subtract 2 — do not test for `+2`.

---

## 5. Caveats

### 5.1 The tablebase directory is a *directory*, and a bad one is silent

`tb_init()` returns `true` for any path it can parse, **including a path that
does not exist and a path with no tables in it**. It does not mean "tables
loaded". Check `TB_LARGEST` (0 = nothing loaded) if that distinction matters.

`tb_init("/nonexistent")` → `true`, `TB_LARGEST == 0`.
`tb_init("")` → `true`, `TB_LARGEST == 0`, probing disabled.

Multiple directories may be separated by `:`.

### 5.2 Corrupt or truncated files are rejected, but noisily

`test_tb()` requires `file_size % 64 == 16`. A partial download fails that
check and the table is skipped with a message on **both** stderr and stdout:

```
Incomplete tablebase file KQvK..rtbw          <- stderr
info string Incomplete tablebase file KQvK..rtbw   <- stdout
```

That `info string ...` on stdout is upstream behaviour and will pollute UCI
output during `tb_init()`. If that matters, redirect stdout around the call or
patch those two `printf`s out.

A file that passes the size check but has bad contents (e.g. a 324-byte HTML
error page) is not detected at init; the probe just returns
`TB_RESULT_FAILED`. Note a 324-byte HTML page fails the size check
(`324 % 64 == 4`), which is how this repository's damaged set was caught.

### 5.3 `rule50` handling differs between the two entry points

* `TB_probe_wdl_simple()` probes with `rule50 = 0`. Fathom's own contract is
  that `tb_probe_wdl()` requires `rule50 == 0`; a non-zero value makes the
  inline return `TB_RESULT_FAILED` outright. Use this during search, where the
  WDL result is normally adjusted for the half-move clock by the caller.
* `TB_probe_root_simple()` honours `rule50` (clamped to 255) and will report
  `TB_CURSED_WIN` / `TB_BLESSED_LOSS` rather than `TB_WIN` / `TB_LOSS` when the
  50-move rule intervenes.

### 5.4 The `ep` field cannot express en passant on a1 — by design

`Pos.ep` is a `uint8_t` and **0 means "no en passant"**, so square `a1`
(index 0) is not representable. This is harmless: a legal en passant target
always lies on rank 3 (`a3..h3`, indices 16-23) or rank 6 (`a6..h6`, indices
40-47), never on rank 1. The glue re-maps any out-of-range `epSquare` to 0.

### 5.5 Invalid positions crash the raw WDL probe

`tb_probe_wdl()` has **no validity check**. `probe_wdl()` calls
`gen_captures()`, which does `lsb(pos->kings & us)` where `us` is the side to
move. With the enemy king capturable, or no king at all, that is `lsb(0)`:

* assertions enabled → `Assertion 'b != 0' failed` in `lsb()`, SIGABRT;
* `-DNDEBUG` → `__builtin_ffsll(0) - 1 == -1`, used as a table index → SIGSEGV.

Reproduced with the position `8/8/8/8/8/3k4/8/KQ6 w - - 0 1`, where the black
king on d3 is attacked by the white queen on b1 while White is to move. That
position is illegal (the side *not* to move is in check) and has no Syzygy
entry.

`tb_probe_root()` is safe here because `tb_probe_root_impl()` runs `is_valid()`
first and returns `TB_RESULT_FAILED`.

**`tb_glue.cpp` therefore validates every position before handing it to
Fathom**, replicating `tbchess.c`'s `is_valid()`: two kings, one per colour;
disjoint piece bitboards that exactly cover the occupancy; no pawns on rank 1
or 8; and the side not to move is not in check. Invalid input returns `false`
instead of crashing. Callers must not bypass the glue.

Note that "side to move is in check" is perfectly legal (it is just check) and
is *not* rejected.

### 5.6 `TB_RESULT_FAILED` is not the only special return

`TB_probe_root_simple()` returning `true` guarantees the result is not
`TB_RESULT_FAILED`, but it may still be `TB_RESULT_CHECKMATE` (0x00000004) or
`TB_RESULT_STALEMATE` (0x00000002). Test those before decoding a move.

`TB_RESULT_CHECKMATE` is `TB_LOSS` in the WDL field and
`TB_RESULT_STALEMATE` is `TB_DRAW`, so a naive `TB_GET_WDL` read will report a
plausible-looking but move-less result.

### 5.7 `tb_probe_root*` is single-threaded

Upstream: "This function is NOT thread safe. For engines this function should
only be called once at the root per search." `tb_probe_wdl*` is thread safe
after `tb_init()`.

### 5.8 Castling rights are excluded

Syzygy does not encode castling, so both entry points hard-code `castling = 0`.
Never probe a position where castling is still available; the result would be
wrong. `tb_probe_wdl()` returns `TB_RESULT_FAILED` for a non-zero castling
argument, so the failure is at least loud.

### 5.9 There is no "tablebase root move ranking"

`tb_probe_root_dtz()` / `tb_probe_root_wdl()` (the `TbRootMoves` API) are
available in the vendored source but are not wrapped by `tb_glue`. Most engines
ignore them and filter root moves themselves using `TB_GET_WDL` from the
`results[]` array of `tb_probe_root`.

### 5.10 `TB_PIECES` is 7

`tbprobe.c` defines `TB_PIECES 7`, so 7-man tables are supported when present
(`TB_MAX_PIECE 650`, `TB_MAX_PAWN 861`). Only 3-4-5 tables are currently on
disk here, so `TB_LARGEST` is 5.

---

## 6. Tablebase data in this repository

`/home/laszapen/lixian/syzygy/` should contain the 3-4-5-man set: 145 `.rtbw`
plus 145 `.rtbz`.

> **The set as originally provided was corrupt.** 279 of 290 files were
> 324-byte Apache "403 Forbidden" HTML pages from `tablebase.sesse.net`, not
> tablebases. Fathom skips them (they fail the `size % 64 == 16` check), so
> nearly every probe fails. Genuine tablebases start with the magic
> `0x5d23e871` (`.rtbw`) or `0xd7660ca5` (`.rtbz`) — or, as bytes,
> `71 e8 23 5d` / `d7 66 0c a5`.

To verify or re-download:

```sh
# Any file that is a 324-byte HTML page is bogus:
head -c 20 syzygy/KQvK.rtbw            # should NOT print "<!DOCTYPE HTML"
xxd -l 4 -p syzygy/KQvK.rtbw           # should be 71e8235d

# Re-download from the lichess mirror (the sesse.net mirror 403s):
base=https://tablebase.lichess.ovh/tables/standard
curl -O $base/3-4-5-wdl/KQvK.rtbw
curl -O $base/3-4-5-dtz/KQvK.rtbz
```

Full set sizes: **378 MB** WDL + **561 MB** DTZ ≈ **938 MB**.

---

## 7. Testing

```sh
make -f tests/tbtest_Makefile run          # against ./syzygy
make -f tests/tbtest_Makefile run TB_PATH=/path/to/tables
make -f tests/tbtest_Makefile debug        # assertions + ASan/UBSan
```

`tests/tbtest.cpp` parses the FENs itself, probes both entry points, decodes
every field, and asserts the side-to-move convention using a control position
where "sign follows the mover" and "sign follows White" predict different
answers. It also asserts that the illegal FEN from §5.5 is rejected cleanly
rather than crashing.

---

## 8. Licence

Fathom is MIT-licensed; see `LICENSE`. The copyright notices in `tbprobe.c`,
`tbprobe.h`, `tbchess.c`, `tbconfig.h`, and `stdendian.h` must be preserved.
`tb_glue.h` / `tb_glue.cpp` are original to this project.
