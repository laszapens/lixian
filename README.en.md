# Lixian (离弦)

**English** · [中文](README.md)

An aggressively styled chess engine written from scratch in C++20. The name means *"the arrow has left the string"* — once a position enters a tactical trajectory, Lixian would rather sacrifice material, rip open lines and drag the enemy king into a firestorm than trade down into a draw.

- **Language / protocol**: C++20, UCI, single static binary
- **Runs on**: any x86-64 CPU with AVX2/BMI2, multi-threaded, no GPU
- **Bundled dependency**: Fathom (Syzygy tablebase probing, MIT)
- **Strength**: ~2700–2750 Elo, measured against Stockfish's `UCI_Elo` handicaps
- **No external engine at runtime**: search, evaluation and training are all original. Stockfish is used only as an offline sparring partner and labelling oracle.

---

## 1. What makes it different

Most engines optimise for safety: draw when equal, simplify when ahead. Lixian does the opposite.

Its evaluation carries an explicit **aggression weight** (`Aggression`, default 140) that does two things at once: it multiplies the value of attacking the enemy king, and it discounts the safety of its own. When it is down material but the enemy king is under fire, the eval adds an **initiative compensation** term so the search keeps calculating the sacrifice instead of writing it off immediately.

Combined with an opening book of 50 gambit and sharp lines (King's Gambit, Evans, Danish, Smith-Morra, Yugoslav Dragon, Najdorf, Marshall, Benko, Budapest, Albin), it routinely sets the board alight inside ten moves.

**It sacrifices when it has calculated the line, not at random.** That difference is what makes it playable.

---

## 2. Quick start

```bash
git clone <repo> && cd lixian
make -j$(nproc)

# Terminal game: you are White, 1 second per move
./lixian play

# You are Black, 1.5 seconds per move
./lixian play black 1500

# UCI mode (for Arena / Cute Chess / Nibbler and friends)
./lixian
```

In the terminal game: `moves` lists legal moves, `eval` prints the static evaluation, `hint` asks for a suggestion, `undo` takes back one move, `d` prints the board, `quit` exits.

### UCI options

| Option | Default | Meaning |
|---|---:|---|
| `Hash` | 512 | Transposition table in MB, split across threads |
| `Threads` | 4 | Search threads; use 1 for fair strength testing |
| `Aggression` | 140 | 100 = neutral. Higher prizes king attacks and sacrifice compensation |
| `Contempt` | 0 | Draw score. Raising it refuses draws more often, and measurably loses Elo |
| `Book` | true | Use the built-in gambit book |
| `BookFile` | `data/gambits.txt` | Path to the book |

**Strongest setting**: `Threads 4`, `Hash 512`, `Aggression 140`, at least 2000 ms per move.
**Wildest setting**: `Aggression 200`, accepting some strength loss for spectacle.

---

## 3. Measured strength

Conditions: fixed time per move; opponent is Stockfish 17.1 with `UCI_LimitStrength` enabled; Lixian single-threaded; rotated openings. Sampling error on these small samples is roughly ±60–80 Elo.

| Opponent | Score | Rate | Conditions |
|---|---:|---:|---|
| Stockfish `UCI_Elo` 2000 | 18.5 / 20 | 92.5% | 300 ms/move |
| Stockfish `UCI_Elo` 2400 | 15.5 / 20 | 77.5% | 300 ms/move |
| Stockfish `UCI_Elo` 2400 (re-run) | 5.5 / 10 | 55.0% | 200 ms/move, 8 openings |
| Stockfish `UCI_Elo` 2600 | 17 / 30 | 56.7% | 200 ms/move, 30 openings |
| Stockfish `UCI_Elo` 2600 (re-run) | 7.5 / 10 | 75.0% | 200 ms/move, 8 openings |
| Stockfish `UCI_Elo` 2800 | 11.5 / 30 | 38.3% | 200 ms/move, 30 openings |
| Stockfish `UCI_Elo` 2800 (re-run) | 3 / 8 | 37.5% | 200 ms/move |

Fitting the three anchor points (2400 / 2600 / 2800) gives roughly **2700–2750**.

> **A note on the units.** `UCI_Elo` is Stockfish's own handicap scale. It is not CCRL Elo and not human FIDE Elo. The table above exists so the numbers can be reproduced, not to claim a rating.

### Correctness

- Full standard Perft suite passes; start position at depth 6 = 119,060,324 nodes at ~52 Mnps.
- WAC tactical subset, 60 positions at 0.3 s/move: 54/60 = 90%.
- Every search change during development was gated on a small-sample match to confirm no regression.

### Time control vs. depth (measured in the middlegame)

| Time per move | Depth | Note |
|---|---|---|
| 100 ms | 13 | Blitz, clearly weaker |
| 300 ms | 14 | Around the default |
| 1000 ms | 17 | Noticeably stronger |
| 3000 ms | 19 | Near this architecture's ceiling |

Going from 300 ms to 3000 ms buys five extra plies, worth roughly 100–150 Elo. **Give it two seconds a move to see it play properly.**

---

## 4. Syzygy tablebases

Lixian probes 3-4-5 piece Syzygy tablebases (up to 7 pieces in code). This fixes the classic failure mode of **turning a won endgame into a draw**.

The tables are too large to ship in the repository, so fetch them yourself:

```bash
mkdir -p syzygy && cd syzygy
# Complete 3-4-5 set, ~940 MB. Get the file list from https://syzygy-tables.info/
```

**Verify the file sizes after downloading.** Some mirrors answer part of the requests with a ~324-byte HTML error page instead of the table; those must be deleted or every probe will fail.

At startup the engine looks in `$LIXIAN_SYZYGY`, then `./syzygy`, then `../syzygy`. If none of them contain usable tables, probing is silently disabled.

Conversion results, with Stockfish defending and Lixian playing White:

| Endgame | Result |
|---|---|
| KQ vs K | mate in 11 |
| KR vs K | mate in 21 |
| KRP vs K | mate in 35 |
| KQ vs KR | mate in 15 |

The implementation works at two levels: `TB_probe_root` at the root returns the provably optimal move directly, and `TB_probe_wdl` at interior nodes supplies exact win/draw/loss scores so the search stops floundering inside won positions.

---

## 5. Architecture

### Search

```
PVS + iterative deepening + aspiration windows
├─ transposition table: 2-way buckets, generational replacement, mate score correction
├─ pruning: null move, RFP, razoring, futility, LMP, LMR, SEE pruning
├─ extensions: check extension, IIR
├─ quiescence: captures/promotions + SEE + delta pruning + TT
└─ ordering: TT move → good/bad captures (SEE) → killers → counter-move → history
```

Time management handles `movetime`, `wtime`/`btime` with increment, and `movestogo`. Threads run independent search trees with private transposition tables, share a stop flag, and the highest-scoring result wins.

### Evaluation

Hand-written, tapered middlegame/endgame:

- material plus staged piece-square tables (Michniewski style)
- mobility; pawn structure (isolated, doubled, passed, candidate, backward); rooks on open files; bishop pair; knight outposts; bad bishops
- king pawn shelter and pawn storms
- threats: undefended attacked pieces, pawn attacks on pieces
- endgame mop-up: drive the king to the edge, bring your own king closer
- **initiative**: king-zone pressure weighting and sacrifice compensation, both scaled by `Aggression`

### NNUE (implemented, disabled by default)

The repository contains a complete NNUE pipeline: 768 → 256×2 → 32, dual-perspective accumulators, AVX2 inference.

It is **disabled by default** for the reason in the next section. To try it:

```bash
LIXIAN_NNUE_WEIGHT=100 ./lixian   # pure NNUE
LIXIAN_NNUE_WEIGHT=30  ./lixian   # 30% neural, 70% hand-written
```

---

## 6. The NNUE experiment: an honest negative result

This section exists because it is more useful than a claim that the engine "has a neural network".

### What was built

| Component | Location | Notes |
|---|---|---|
| Self-play data generator | `tools/nnue/datagen.cpp` | Pure C++, no Python; ~940 positions/s on 64 cores |
| Trainer | `tools/nnue/trainer.cpp` | Pure C++ + OpenMP, sparse gradients; 7.56M positions, 20 epochs in ~40 min |
| Inference | `src/nnue.cpp` | float32 + AVX2, 827 ns per evaluation |
| Training data | not committed | 11.61M positions, regenerable with the generator |

### Results

| Configuration | vs SF2000 | vs SF2200 | Eval cost |
|---|---:|---:|---:|
| Hand-written (default) | **100%** | **100%** | 182 ns |
| Pure NNUE (L1=512) | 12.5% | — | 2050 ns |
| Pure NNUE (L1=256, optimised) | 58.3% | 25% | 827 ns |
| NNUE vs hand-written, head to head | — | 56.2% | — |

### Conclusion

The network's **evaluation quality really is better** — head to head under identical conditions it beats the hand-written evaluator 56.2%, and its symmetry, piece values and endgame direction all check out.

But its **inference cost is 4.5× higher**. Under a fixed 200 ms per move, that is three to six plies of search thrown away. At this strength level the marginal value of depth exceeds the value of evaluation precision.

Stockfish wins with NNUE because it pushes evaluation below 100 ns using int8 quantisation and hand-written AVX2 kernels. That is an entire fixed-point inference pipeline — a different order of engineering effort.

**So the code stays and the feature is off.** It is a path that has been explored and measured, not an unfinished one.

---

## 7. Training tools

```bash
# Self-play with Stockfish labelling
g++ -std=c++17 -O3 -march=native -pthread -o build/datagen tools/nnue/datagen.cpp
./build/datagen --sf /path/to/stockfish --openings tools/nnue/openings.txt \
                --out data --workers 30 --minutes 60

# Train
make trainer
./build/trainer --data data --epochs 20 --batch 512 --lr 0.0025 \
                --out weights/nnue.bin --threads 8
```

`tools/` also holds a few development-only Python scripts (they need `python-chess`): `match.py` (UCI matches with Elo estimation), `tactics.py` (EPD tactical suites), `blunders.py` (centipawn-loss analysis of PGNs). None of them are needed at runtime.

---

## 8. Known limitations

- **NNUE is off by default** — see section 6. The bottleneck is inference speed, not evaluation quality.
- `Threads` runs independent searches, not a strict shared-table Lazy SMP; four threads are worth roughly +30–40 Elo.
- The opening book covers 50 lines only. Once the opponent leaves it, the engine is on its own.
- Without Syzygy tables, very simple endgames (3–5 pieces) can still be finished imprecisely.
- No GPU. Neither ROCm nor CUDA is used: search is a pure CPU workload and a GPU of this class does not help at this scale.
- The strength numbers are small-sample match estimates, not an official CCRL rating.

---

## 9. Repository layout

```
src/                    engine sources
  types.h               move encoding, constants
  bitboard.{h,cpp}      bitboards, PEXT/magic slider attacks
  position.{h,cpp}      position state, Zobrist, make/unmake, SEE
  movegen.{h,cpp}       move generation
  eval.{h,cpp}          hand-written evaluation
  nnue.{h,cpp}          NNUE inference
  search.{h,cpp}        PVS search and transposition table
  book.{h,cpp}          opening book
  tbprobe_glue.{h,cpp}  Syzygy bridge
  uci.cpp               UCI protocol and terminal game
  third_party/fathom/   Fathom (MIT), tablebase probing
data/gambits.txt        50 gambit opening lines
tests/perft.cpp         Perft verification
tools/nnue/             C++ data generator and trainer
tools/*.py              development helper scripts
```

Build targets: `make` (engine), `make perft` (verification), `make trainer` (trainer), `make debug` (ASan/UBSan).

Requirements: x86-64 with AVX2 and BMI2 (built with `-march=native`). Without BMI2 the slider attacks fall back to magics and run somewhat slower.

---

## 10. Licence

MIT. See [LICENSE](LICENSE).

The vendored Fathom library is MIT (Ronald de Man, basil00, Jon Dart). Full attribution and licence texts are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Stockfish is **not** included. It is invoked externally by the development scripts as a sparring partner and labelling oracle. It is never linked into the engine, never a runtime dependency, and never redistributed with it.

---

## 11. One line

Lixian is a **style-first engine that can actually play**: it lights fires on the kingside, sacrifices for the attack, and drags opponents into calculation with gambit openings. It overpowers club players and most mid-tier AIs, trades blows with 2600, and does not pretend it can beat full-strength Stockfish.
