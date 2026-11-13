# 离弦 Lixian

[English](README.en.md) · **中文**

一个从零写起的极端进攻型国际象棋引擎。名字取自「箭已离弦」——局面一旦进入战术轨道，它更愿意弃子、开线、把王拖进火网，而不是把局面简化成和棋。

- **语言 / 接口**：C++20，UCI 协议，单文件可执行
- **运行环境**：x86-64 CPU，多线程，无 GPU 依赖
- **依赖**：Fathom（Syzygy 残局库探测，MIT，已内置）
- **强度**：约 2700–2750 Elo（以 Stockfish `UCI_Elo` 限制档为锚点实测）
- **无外部引擎依赖**：搜索、评估、训练全部自研；Stockfish 只作为开发期的测试对手与标注工具，不参与运行时

---

## 1. 它有什么特别的

大多数引擎追求「稳」：均势就和、优势就简化。离弦反过来。

它的评估里有一个显式的**进攻权重**（`Aggression`，默认 140），会同时做两件事：放大自己攻击敌方王区的价值，压低自己王安全的价值。当它少子但对方王区被打穿时，评估还会给一个**主动权补偿**，让搜索愿意继续深算弃子线路而不是立刻判亏。

配合 50 条弃兵/尖锐开局线路的内置开局库（王翼弃兵、伊文思、丹麦、史密斯-莫拉、龙式南斯拉夫、纳道尔夫、马歇尔、Benko、Budapest、阿尔宾），它经常在开局十步内就点火。

**它会在算得清的情况下弃子，而不是随机送子。** 这两者的区别，就是能不能打。

---

## 2. 快速开始

```bash
git clone <repo> && cd lixian
make -j$(nproc)

# 终端对弈：你执白，每步 1 秒
./lixian play

# 你执黑，每步 1.5 秒
./lixian play black 1500

# UCI 模式（给 Arena / Cute Chess / Nibbler 等 GUI）
./lixian
```

终端对弈支持 `moves`（列合法着）、`eval`（看静态评估）、`hint`（要建议）、`undo`（悔一步）、`d`（打印棋盘）、`quit`。

### UCI 选项

| 选项 | 默认 | 说明 |
|---|---:|---|
| `Hash` | 512 | 置换表大小（MB），按线程均分 |
| `Threads` | 4 | 搜索线程数；测强度建议设 1 |
| `Aggression` | 140 | 100 = 中性，越高越重视王攻与弃子补偿 |
| `Contempt` | 0 | 和棋分值；调高更拒绝和棋，但实测掉 Elo |
| `Book` | true | 使用内置弃兵开局库 |
| `BookFile` | `data/gambits.txt` | 开局库路径 |

**想要最强棋力**：`Threads 4`、`Hash 512`、`Aggression 140`、每步 ≥ 2000 ms。
**想要最疯**：`Aggression 200`，接受一定的强度损失换观赏性。

---

## 3. 实测强度

测试条件：固定每步时间；对手是 Stockfish 17.1 并开启 `UCI_LimitStrength`；离弦单线程；多开局轮换。小样本误差约 ±60–80 Elo。

| 对手 | 成绩 | 得分率 | 条件 |
|---|---:|---:|---|
| Stockfish `UCI_Elo` 2000 | 18.5 / 20 | 92.5% | 300 ms/步 |
| Stockfish `UCI_Elo` 2400 | 15.5 / 20 | 77.5% | 300 ms/步 |
| Stockfish `UCI_Elo` 2400（复测） | 5.5 / 10 | 55.0% | 200 ms/步，8 开局 |
| Stockfish `UCI_Elo` 2600 | 17 / 30 | 56.7% | 200 ms/步，30 开局 |
| Stockfish `UCI_Elo` 2600（复测） | 7.5 / 10 | 75.0% | 200 ms/步，8 开局 |
| Stockfish `UCI_Elo` 2800 | 11.5 / 30 | 38.3% | 200 ms/步，30 开局 |
| Stockfish `UCI_Elo` 2800（复测） | 3 / 8 | 37.5% | 200 ms/步 |

三点（2400 / 2600 / 2800）线性拟合后约 **2700–2750**。

> **关于口径**：`UCI_Elo` 是 Stockfish 自己的限制强度标尺，不等于 CCRL 或人类 Elo。上表只用于可复现的相对定位，不是正式评级。

### 正确性

- 标准 Perft 全套通过；起始局面深度 6 = 119,060,324 节点，约 52 Mnps。
- WAC 战术抽样 60 题（0.3 s/步）：54/60 = 90%。
- 迭代开发中跑过大量自对弈回归；每次改搜索参数都用小样本对抗确认没有掉分。

### 思考时间与深度（中局实测）

| 每步时间 | 深度 | 说明 |
|---|---|---|
| 100 ms | 13 | 快棋，明显变弱 |
| 300 ms | 14 | 默认附近 |
| 1000 ms | 17 | 明显更强 |
| 3000 ms | 19 | 接近本架构上限 |

从 300 ms 提到 3000 ms 多出 5 层，约值 100–150 Elo。**想发挥全力，把思考时间拉到 2 秒以上。**

---

## 4. Syzygy 残局库

引擎支持 3-4-5 子（代码上最多 7 子）Syzygy 残局库。这一项解决的是**「优势残局走成和棋」**这个经典毛病。

残局库不随仓库分发（太大），需要自己下载：

```bash
mkdir -p syzygy && cd syzygy
# 3-4-5 子完整集，约 940 MB，从 lichess 镜像下载（推荐）
curl -s "https://tablebase.lichess.ovh/tables/standard/3-4-5/" >/dev/null 2>&1 || true
# 文件清单与校验值见 https://syzygy-tables.info/
```

完整文件列表可从 [syzygy-tables.info](https://syzygy-tables.info/) 获取，或用任意镜像脚本批量下载。**下载后请校验文件大小**：某些镜像会对部分请求返回 HTML 错误页（约 324 字节），那种文件必须删掉，否则探测会失败。

引擎启动时按顺序查找：`$LIXIAN_SYZYGY`、`./syzygy`、`../syzygy`。找不到就静默禁用。

实测转化能力（对手是 Stockfish 防守，离弦执白）：

| 残局 | 结果 |
|---|---|
| KQ vs K | 11 步将死 |
| KR vs K | 21 步将死 |
| KRP vs K | 35 步将死 |
| KQ vs KR | 15 步将死 |

实现上分两层：根节点用 `TB_probe_root` 直接取理论最优着法；内部节点用 `TB_probe_wdl` 给精确胜负分，避免搜索在必胜局面里瞎逛。

---

## 5. 架构

### 搜索

```
PVS + 迭代加深 + aspiration window
├─ 置换表：2-way bucket，代际替换，mate score 校正
├─ 剪裁：null move、RFP、razoring、futility、LMP、LMR、SEE pruning
├─ 扩展：check extension、IIR
├─ 静态搜索：吃子/升变 + SEE + delta pruning + TT
└─ 排序：TT move → 好/坏吃子（SEE）→ killer → counter-move → history
```

时间管理支持 `movetime`、`wtime/btime + inc`、`movestogo`。多线程是独立搜索树 + 各自 TT，共享停止信号，取最高分结果。

### 评估

手写评估，tapered mg/eg：

- 子力 + 分段 PST（Michniewski 风格）
- 机动性、兵结构（孤/叠/通路/候选/落后）、车开放线、象对、骑士前哨、坏象
- 王翼兵盾与兵风暴
- 威胁项：被攻击且无兵保护的子力、兵威胁子力
- 残局 mop-up：逼王到边、拉近己王
- **进攻主动权**：`Aggression` 控制的王区压力加权与弃子补偿

### NNUE（已实现，默认关闭）

仓库里有一整套可用的 NNUE 管线：768→256×2→32，双视角累加器，AVX2 推理。

**但默认关闭**，原因见下一节。想试：

```bash
LIXIAN_NNUE_WEIGHT=100 ./lixian   # 纯 NNUE
LIXIAN_NNUE_WEIGHT=30  ./lixian   # 30% 神经 + 70% 手工
```

---

## 6. NNUE 实验：一个诚实的负面结果

这一节写下来，是因为它比「我们加了神经网络」更有价值。

### 做了什么

| 组件 | 位置 | 说明 |
|---|---|---|
| 自对弈数据生成器 | `tools/nnue/datagen.cpp` | 纯 C++，无 Python 依赖；64 核上约 940 局面/秒 |
| 训练器 | `tools/nnue/trainer.cpp` | 纯 C++ + OpenMP，稀疏梯度；756 万局面 20 轮约 40 分钟 |
| 推理 | `src/nnue.cpp` | float32 + AVX2，827 ns/次 |
| 训练数据 | 未入库 | 1161 万局面，可用 datagen 重新生成 |

### 结果

| 配置 | 对 SF2000 | 对 SF2200 | 单次评估耗时 |
|---|---:|---:|---:|
| 手写评估（默认） | **100%** | **100%** | 182 ns |
| 纯 NNUE（L1=512） | 12.5% | — | 2050 ns |
| 纯 NNUE（L1=256 优化后） | 58.3% | 25% | 827 ns |
| NNUE vs 手写（同条件直接对拉） | — | 56.2% | — |

### 结论

NNUE 的**评估质量确实更好**——同等条件直接对拉能赢手写评估 56.2%；对称性、子力价值、残局方向全部正确。

但它的**推理开销是手写评估的 4.5 倍**。在固定 200 ms 限时下，这等于少搜 3–6 层深度。而在这个强度区间，深度的边际价值大于评估精度。

Stockfish 能用 NNUE 赢，是因为它用 int8 量化 + 手写 AVX2 内核把评估压到 100 ns 以内。那需要一整套定点数量化推理管线，是另一个工程量级的工作。

**所以：代码留着，默认关掉。** 这是一条已经验证过、但当前环境下不划算的路，不是没做完的半成品。

---

## 7. 训练工具

```bash
# 自对弈 + Stockfish 标注
g++ -std=c++17 -O3 -march=native -pthread -o build/datagen tools/nnue/datagen.cpp
./build/datagen --sf /path/to/stockfish --openings tools/nnue/openings.txt \
                --out data --workers 30 --minutes 60

# 训练
make trainer
./build/trainer --data data --epochs 20 --batch 512 --lr 0.0025 \
                --out weights/nnue.bin --threads 8
```

`tools/` 下另有几个开发期脚本（需要 `python-chess`）：`match.py`（UCI 对抗 + Elo 估算）、`tactics.py`（EPD 战术套件）、`blunders.py`（PGN 失误分析）。这些只用于开发，运行时不需要。

---

## 8. 已知限制

- **NNUE 默认关闭**：见第 6 节，瓶颈是推理速度不是评估质量。
- `Threads` 是独立搜索线程，不是共享置换表的严格 Lazy SMP；4 线程约 +30–40 Elo。
- 开局库只覆盖 50 条线路；对手偏离后立即进入搜索。
- 没有 Syzygy 时，极简残局（3-5 子）仍可能出现收尾不精确。
- 不依赖 GPU。ROCm/CUDA 都没有用到——搜索是纯 CPU 工作负载，GPU 在这个规模上帮不上忙。
- 强度数据是小样本对抗估算，不是 CCRL 正式评级。

---

## 9. 目录结构

```
src/                    引擎源码
  types.h               Move 编码、常量
  bitboard.{h,cpp}      位棋盘、PEXT/魔术滑块
  position.{h,cpp}      局面、Zobrist、make/unmake、SEE
  movegen.{h,cpp}       着法生成
  eval.{h,cpp}          手写评估
  nnue.{h,cpp}          NNUE 推理
  search.{h,cpp}        PVS 搜索、置换表
  book.{h,cpp}          开局库
  tbprobe_glue.{h,cpp}  Syzygy 桥接
  uci.cpp               UCI 协议 + 终端对弈
  third_party/fathom/   Fathom（MIT，残局库探测）
data/gambits.txt        50 条弃兵开局线路
tests/perft.cpp         Perft 验证
tools/nnue/             数据生成器与训练器（C++）
tools/*.py              开发期辅助脚本
```

构建目标：`make`（引擎）、`make perft`（验证）、`make trainer`（训练器）、`make debug`（ASan/UBSan）。

要求：支持 AVX2/BMI2 的 x86-64 CPU（构建用 `-march=native`；没有 BMI2 时滑块走魔术回退，会慢一些）。

---

## 10. 许可

MIT，见 [LICENSE](LICENSE)。

内置的 Fathom 是 MIT（Ronald de Man / basil00 / Jon Dart）。第三方归属与许可全文见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

本仓库不包含 Stockfish。Stockfish 只作为开发期测试对手与标注工具被外部调用，从不链接进引擎、不作为运行时依赖、不随引擎分发。

---

## 11. 一句话

离弦是一个**风格优先、但确实能打**的引擎：它会在王翼点火、为攻势弃子、用弃兵把对手拖进计算。对俱乐部棋手和常见中低档 AI 有压制力，对 2600 左右互有胜负，对满血 Stockfish 不冒充能赢。
