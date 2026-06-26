Algorithm 1  STFP-R4 Per-Level Compaction Decision

▷ 每个非 L0 层的决策逻辑，按优先级依次判定
▷ L0→L1 的下推由 Ingest 侧的 StageDown / DrainL0AllFiles 独立处理
▷ Score 触发 = 整层归并（TryPickWholeLevelNonL0ST）
▷ Run-count 触发 = 选子集归并（SelectBestMergeBatch），非整层
▷ ⚠ TODO: run-count 优先 vs score 优先的次序待定（当前代码 score 在前）

Input:  level ℓ ≥ 1, max_runs R (=4), score threshold θ (=1.0)
Output: compaction job c or nil

 1  function PickForLevel(v, ℓ):
 2      ▷ Priority 1: score 超标 → 整层下推（当前代码顺序）
 3      if score(v, ℓ) ≥ θ then
 4          return WholeLevelCompact(v, ℓ, output = ℓ+1)
 5
 6      ▷ Priority 2: run-count 超标 → 选子集下推
 7      if runs(v, ℓ) > R then
 8          return SelectBestMergeBatch(v, ℓ, output = ℓ+1)
 9
10      ▷ Priority 3: 空间近邻 → 层内合并（不改变 level）
11      batch ← SpatialNeighbors(v, ℓ)
12      if batch ≠ ∅ then
13          return IntraLevelMerge(v, ℓ, batch, output = ℓ)
14
15      return nil


Algorithm 2  State Determination

Input:  LSM-tree version v, L0 trigger T
Output: state S ∈ {IDLE, NORMAL, HIGH}

16  function ComputeState(v):
17      L0 ← |v.files[0]|
18      if L0 ≥ 2T then return HIGH
19      if L0 = 0  then return IDLE
20      return NORMAL


Algorithm 3  L1 Scheduling by State

▷ L1 是 L0 的直接下游，状态由 L0 积压程度驱动
▷ L2+ 层始终执行 Algorithm 1（不受 L0 状态影响）

Input:  LSM-tree version v, state S, max_runs R (=4)
Output: compaction job c or nil

21  function ScheduleL1(v, S):
22      switch S:

23      case HIGH:                              ▷ L0 ≥ 2T：高写入压力
24          ▷ L1 只接收 L0 下推数据，自身不做 compaction
25          return nil

26      case NORMAL:                            ▷ 0 < L0 < 2T：正常写入
27          return PickForLevel(v, 1)             ▷ θ = 1.0

28      case IDLE:                              ▷ L0 = 0：无写入压力
29          ▷ 1. 膨胀文件下推（Trigger B）
30          f ← FindBloated(v, 1)
31          if f ≠ nil then return PushDown(f)
32          ▷ 2. 正常流程（降低阈值：θ = 0.7）
33          return PickForLevel(v, 1, θ = 0.7)


Algorithm 4  Top-Level Compaction Scheduler

▷ L0 始终最优先

Input:  LSM-tree version v, L0 trigger T, max_runs R (=4)
Output: compaction job c or nil

34  function ScheduleCompaction(v):
35      S ← ComputeState(v)
36
37      ▷ Step 1: L0 清空（始终最优先）
38      if |v.files[0]| > 0 then
39          return StageDownL0(v)
40
41      ▷ Step 2: L1 按状态调度
42      c ← ScheduleL1(v, S)
43      if c ≠ nil then return c
44
45      ▷ Step 3: L2+ 始终执行 Algorithm 1
46      for ℓ ← 2 to v.max_level do
47          c ← PickForLevel(v, ℓ)
48          if c ≠ nil then return c
49
50      return nil


─── Helper Functions ───

  runs(v, ℓ):
      return |{f ∈ v.files[ℓ] ∧ ¬f.is_compacting}|

  score(v, ℓ):
      return size(v.files[ℓ]) / MaxBytes(ℓ) / 0.9

  WholeLevelCompact(v, ℓ, output):
      ▷ 整层归并：该层全部 SST + L+1 overlap
      inputs ← v.files[ℓ] ∪ OverlappingFiles(v, ℓ+1)
      return Compaction(inputs, output_level = output)

  SelectBestMergeBatch(v, ℓ, output):
      ▷ 非整层：选最优 runs 子集归并
      batch ← SelectBestRuns(v.files[ℓ], max_runs = R)
      return Compaction(batch, output_level = output)

  IntraLevelMerge(v, ℓ, batch):
      return Compaction(batch, output_level = ℓ)

  StageDownL0(v):
      f ← NewestFile(v.files[0])
      return Compaction({f}, output_level = 1)

  FindBloated(v, ℓ):
      for f ∈ v.files[ℓ] do
          if |f.KVs| > MaxKeys(ℓ) ∨ f.time_span > MaxSpan(ℓ) then
              return f
      return nil

  PushDown(f):
      return Compaction({f}, output_level = f.level + 1)


─── 状态转换 ───

         ┌──────────────────────────────────┐
         │                                  │
         ▼                                  │
  ┌──────────┐    L0>0       ┌──────────┐   │ compaction
  │   IDLE   │ ────────────→ │  NORMAL  │───┘ 完成
  │  L0 = 0  │ ←──────────── │ 0<L0<2T  │
  └──────────┘   L0=0        └────┬─────┘
                                  │ L0 ≥ 2T
                                  ▼
                            ┌──────────┐    L0 < 2T
                            │   HIGH   │ ──────────→ NORMAL
                            │ L0 ≥ 2T  │
                            └──────────┘
                              L1: 只接不收


─── 三态行为对比 ───

  State    L1 动作                              L2+ 动作           Score 阈值
  ───────  ──────────────────────────────────   ────────────────   ─────────
  HIGH     nil（等待 L0 排空）                   Algorithm 1         —
  NORMAL   Algorithm 1                           Algorithm 1         1.0
  IDLE     Trigger B → Algorithm 1               Algorithm 1         0.7

─── 下推方式对比 ───

  触发条件           方式                   输入范围
  ────────────────   ──────────────────     ────────────
  score ≥ θ          整层归并               该层全部 SST
  runs > R            选子集归并             最优 runs 子集
  Trigger B (IDLE)    单文件下推             膨胀文件 + L+1 邻居
  Trigger A (NORMAL)  层内空间近邻合并       同层近邻 SST 对
  L0 > 0              StageDown             单个最新 L0 文件
