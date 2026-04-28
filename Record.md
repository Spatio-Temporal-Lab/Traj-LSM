## 2026-04-14 — `OFF/ON vs Vanilla` 首次尝试失败（`vanilla_as_baseline_unavailable`）

| 项 | 内容 |
|----|------|
| **实验目的** | 在不拆 Global/Local 的前提下，直接比较 `verify_wuxi_segment_natural_compact_off` 与 `..._on` 相对 Vanilla 分母的倍率（`-VanillaAsBaseline`）。 |
| **实验结果** | 产物：`data/experiments/natural_off_on_vs_vanilla.tsv`。24 行（OFF 12 + ON 12）全部 `error=vanilla_as_baseline_unavailable`，`vanilla_wall_us` 为空。 |
| **是否达到预期** | **否**。本次未拿到 Vanilla 分母。 |
| **为什么** | `st_prune_vs_full_baseline_sweep.ps1` 在 `-VanillaAsBaseline` 下要求每行必须得到 `vanilla_wall_us`（来自 Vanilla cache 命中或 `st_segment_window_scan_vanilla.exe` 现场跑）。当前两库对应的 Vanilla 路径/缓存键未命中（或 Vanilla exe 未成功返回），导致整表不可用。 |
| **下一步** | 1) 明确可打开的 Vanilla DB 路径；2) 传 `-VanillaWallDbPathsCsv`（与 OFF/ON 两库位置一一对齐，必要时可传同一路径两次）；3) 或先重建覆盖当前窗表的 Vanilla cache，再重跑 `-VanillaAsBaseline`。 |

## 2026-04-28 — JSONL ingest 批次继续：`none+b4096` / `snappy+b65536` / `unsplit+snappy+b65536`

| 项 | 内容 |
|----|------|
| **实验目的** | 按“先把实验批次跑齐，再统一验证”的节奏，补一组可复现 ingest 结果，覆盖压缩与 split policy 两个维度，为后续对比与验证做输入准备。 |
| **实验结果** | 运行 `tools/ingest_jsonl_and_summarize.ps1` 三次：1) `p80_120 + none + b4096 + RunTag=20260428_e8_none_b4096`；2) `p80_120 + snappy + b65536 + RunTag=20260428_e8_snappy_b65536`；3) `unsplit + snappy + b65536 + RunTag=20260428_e8_unsplit_snappy_b65536`。结果目录分别在 `Result/vanilla_jsonl_ingest/...`，每组都产出 `summary.json/table_props.json/table_props.tsv/ingest_stdout.txt`。关键数字：① `p80_120 none b4096` 写入 `374097` keys，`sst_count=26`，`sst_total_bytes=1,937,362,354`；② `p80_120 snappy b65536` 写入 `374097` keys，`sst_count=7`，`sst_total_bytes=359,991,426`；③ `unsplit snappy b65536` 写入 `75,571` keys，`sst_count=7`，`sst_total_bytes=386,540,372`。 |
| **是否达到预期** | **是（阶段目标）**：实验批次已继续推进并成功落盘，可直接进入下一轮统一验证/汇总。 |
| **为什么** | 同一脚本统一了构建、灌入、表属性导出与 summary 打包，便于后续做同口径比较；当前阶段重点是先把样本批次跑完。 |
| **下一步** | 继续补齐你计划中的剩余参数组合（若有），然后再一次性做验证与对比汇总（读取各 run 的 `summary.json`/`table_props.json`）。 |

## 2026-04-14 — 自然输入 A/B：`compact OFF` vs `compact ON`（同一 fork，非 736/776 预分桶）

| 项 | 内容 |
|----|------|
| **实验目的** | 按 A 方案验证 compaction 设计本身：不依赖 `736/776` 预分桶，使用自然输入建基库后复制两份，在同一 fork 内做 **OFF（常规 compact）** vs **ON（`--st-time-split` + ST spatial/read-repair 参数）**，比较三模式 `manifest/sst/sst_manifest`。 |
| **库与规模** | `BASE=verify_wuxi_segment_natural_base`（`*.sst=8`），`OFF=..._compact_off`（`*.sst=15`），`ON=..._compact_on`（`*.sst=74`）。 |
| **输出 TSV** | `data/experiments/natural_off_vs_on_manifest.tsv`、`natural_off_vs_on_sst.tsv`、`natural_off_vs_on_sst_manifest.tsv`。 |
| **聚合结果（12 窗中位）** | **manifest**：OFF `med_ratio_wall=0.9949`，ON `1.0489`（ON 更慢）；**sst**：OFF `0.0218`，ON `0.0183`（ON 更快）；**sst_manifest**：OFF `0.0050`，ON `0.00335`（ON 明显更快）。对应 `med_prune_wall_us`：sst `6029.9 -> 5412.5`，sst_manifest `1382.4 -> 1090.8`（OFF->ON）。`med_full_wall_us`：`~274k/276k -> ~311k`（ON full 变慢）。 |
| **是否达到预期** | **部分达到（偏正向）**：若目标是提升 ST 剪枝路径（Local/L+G）收益，ON 优于 OFF；若目标是提升 full 或 manifest-only，则本批 ON 不占优。 |
| **为什么** | ON 将库拆成更多 SST（15→74），提高了时空可分离与候选过滤机会，利好 `sst`/`sst_manifest` 的 prune；但更多文件也增加 full 基础开销，且 manifest-only 在本批设置下接近“按文件扫描即可覆盖”，未体现优势。 |
| **下一步** | 1) 用同批 TSV 画 OFF/ON 三模式柱图（`med_ratio_wall` + `med_prune_wall_us`）；2) 对代表窗跑 `st_meta_sst_diag` 比较 file/block disjoint 分布，补机制证据；3) 若要对外给单一结论，需先明确优化目标是 **prune 查询** 还是 **full 基线**。 |

## 2026-04-14 — Compaction 前后质量验证（二）：`manifest only` + `sst only` + 三维雷达指标

| 项 | 内容 |
|----|------|
| **实验目的** | 按“先看整体性能，再分解质量维度”的口径，补齐 compaction 前后双库对照：`manifest only`（文件级质量）与 `sst only`（块/键级质量），并与既有 `sst_manifest` 一起形成三维指标（文件级 / 块级 / 时延）。 |
| **实验结果（新增 TSV）** | 1) `data/experiments/wuxi_bucket3600_pre_vs_be_manifest.tsv`；2) `data/experiments/wuxi_bucket3600_pre_vs_be_sst.tsv`。先前已跑：`..._sst_manifest.tsv`。均为双库同窗（12 窗）对照：`verify_wuxi_segment_bucket3600_sst` vs `verify_wuxi_segment_bucket3600_sst_be`。 |
| **雷达式指标（12 窗聚合）** | **manifest only**：前库 `med_ratio_wall=0.01195`、`file_skip_ratio=0.9843`；后库 `med_ratio_wall=0.9669`、`ratio_bytes=1.0`、`ratio_block=1.0`。**sst only**：前库 `med_ratio_wall=0.0118`、`med_ratio_block=0.0343`、`block_skip_ratio=0.9923`；后库 `med_ratio_wall=0.0167`、`med_ratio_block=0.4628`、`block_skip_ratio=0.9923`。**sst_manifest**：前库 `med_ratio_wall=0.00225`（最好），后库 `med_ratio_wall=0.0166`。同时 full 中位壁钟前后约 `~607ms -> ~280ms`（full 变快）。 |
| **是否达到预期** | **部分达到**。Compaction 明显降低了 full 路径成本，但“质量”上文件级与块级剪枝收益形态发生重排：后库 `manifest` 几乎失效（在本批窗与配置下），`sst` 仍有效但相对 full 的比例收益收窄。 |
| **为什么** | 你的判断与数据一致：SST 从 736 合并到 76 后，单文件覆盖范围扩大，文件级 MBR 更易与查询窗相交，`Global` 可跳过空间显著缩小；块级仍能高比例判 disjoint（`block_skip_ratio` 仍 ~0.992），但因 full 本身已更便宜，ratio 口径下优势不再像前库那么“极端”。 |
| **下一步** | 对代表窗做 `st_meta_sst_diag --window` 前后库并排（窄/中/宽各 1），把“文件级相交增多、块级仍可跳过”补成可视化证据；最终报告建议同时给 **绝对时延** 与 **ratio**，避免只看一个口径。 |

## 2026-04-14 — Compaction 质量验证（`bucket3600_sst` 前后双库对照，`sst_manifest`）

| 项 | 内容 |
|----|------|
| **实验目的** | 按 Method 的 Compaction 口径验证“质量变化”而非仅文件数：对比 **compaction 前** `verify_wuxi_segment_bucket3600_sst`（736 SST）与 **compaction 后** `verify_wuxi_segment_bucket3600_sst_be`（76 SST）在同一 12 窗、同一 bench 参数下的 full/prune 行为。 |
| **实验结果** | 对照 TSV：`data/experiments/wuxi_bucket3600_pre_vs_be_sst_manifest.tsv`（`-PruneMode sst_manifest -TimeBucketCount 736 -VirtualMerge -VirtualMergeAuto`）。聚合（12 窗均值/中位数）显示：**前库** `prune_file_skipped` 均值 **724.4**（`considered=736`），`ratio_block_read` 中位 **0.0343**，`ratio_wall` 中位 **0.00225**；**后库** `prune_file_skipped=0`、`prune_file_considered=0`，`ratio_block_read` 中位 **0.46275**，`ratio_wall` 中位 **0.0166**。同时 **full** 成本明显下降：`full_block_read` **1472 → 228**，`full_wall_us` 中位 **607150.5 → 279629.5**。 |
| **是否达到预期** | **部分达到**。Compaction 显著降低了 full 路径开销（SST 数与读放大下降），但在当前参数组合下 manifest 文件级统计从“可大量跳过”变为“未记录/未触发”，并伴随 prune/full 比值上升，说明“可剪枝质量”发生了结构性变化。 |
| **为什么** | Compaction 重写了 SST 边界与层级布局：一方面减少了 full 打开与遍历成本；另一方面改变了文件级 disjoint 触发条件与观测计数（该批后库行 `prune_file_*` 为 0，需通过 `manifest` only + diag 再确认是机制退化还是模式路径差异）。 |
| **下一步** | 1) 对前/后库分别跑 `-PruneMode manifest`（同窗）确认文件级跳过是否真实退化；2) 用 `st_meta_sst_diag --window` 对至少 3 个代表窗（窄/中/宽）做 file-level disjoint 对照；3) 再补 `sst` 模式，拆分“文件级变化”与“块/键级变化”贡献。 |

## 2026-04-14 — 无锡 `bucket3600_sst_be`：`sst_manifest` 12 窗 sweep（修正参数后）

| 项 | 内容 |
|----|------|
| **实验目的** | 修复 `st_prune_vs_full_baseline_sweep.ps1` 调用参数后（从错误的 `-Db/-BenchExe` 改为 `-RocksDbPaths/-Bench`），在无锡第三档库 `verify_wuxi_segment_bucket3600_sst_be` 上按 stratified12 窗表重跑 `sst_manifest`，验证 full+prune 与 KV 对拍是否正常。 |
| **实验结果** | 命令成功执行并写出 `data/experiments/wuxi_bucket3600_be_sst_manifest.tsv`，12 窗均完成；`kv_correct` 全部为 **OK (12/12)**，`kv_full_inwindow_kv == kv_prune_inwindow_kv`。`full_keys_scanned_total` 各窗一致为 **81768**；`full_block_read=228`。`prune_keys_in_window` 与 full 窗内键一致（如窄窗 `54/60/51/77`，中窗 `73/137/122/104`，宽窗 `1197/1658/1117/2033`）。`ratio_bytes` 约 **0.0015–0.0275**，`ratio_block_read` 约 **0.0658–0.3509**，`ratio_wall` 约 **0.0019–0.0320**。文件级统计稳定：`prune_file_considered=76`，`prune_file_skipped=72~74`，`prune_file_time_disjoint=72~74`，`prune_file_space_disjoint=0`。 |
| **是否达到预期** | **是**。此前 `db_missing` 问题消失，当前 sweep 已按目标库与目标窗表跑通，且 prune 与 full 在 bench 口径下结果一致。 |
| **为什么** | 根因是脚本参数名不匹配导致回落默认库 `verify_pkdd_st`（不存在）；改为正确参数后脚本读取到目标 DB。当前结果中 `time_disjoint` 高、`space_disjoint=0`，说明该批窗主要通过时间维触发文件级过滤；同时 `sst_manifest` 叠加了块级/键级过滤，使 `bytes/wall` 相对 full 显著下降。 |
| **下一步** | 1) 继续同库跑 `manifest` 与 `sst` 两个模式，产出三份 TSV；2) 运行 `tools/summarize_wuxi_ablation_three_modes.py --pooled` 做三模式汇总；3) 若要与主报告口径严格对齐，补充 `-TimeBucketCount 736 -VirtualMerge -VirtualMergeAuto` 再跑一轮并与本次 TSV 对比。 |

## 2026-04-16 — Manifest 分片 preflight：dangling 引用修复与护栏

| 项 | 内容 |
|----|------|
| **验证目的** | 在 `verify_wuxi_segment_bucket3600_sst` 上推进 Manifest 分片闭环的 Phase D：`MANIFEST_SET` 索引自洽、桶宽一致性、并阻断“MANIFEST_SET 引用缺失 MANIFEST”导致 DB 无法打开的问题。 |
| **现象** | `MANIFEST_SET-*` 里会出现历史 `entry=MANIFEST-xxxxxx,...`，但对应 `MANIFEST-xxxxxx` 在目录里缺失，导致 `DB::Open` 直接失败；`CURRENT` 会持续滚动到新的 `MANIFEST_SET-`，但仍可能带入缺失历史引用。 |
| **处理** | 1) `tools/manifest_sharding_preflight.ps1` 增强：打印 `[DIAG]` 每条 entry 的存在性与范围；支持 `-RepairMissingHistorical` 自动重写当前 `MANIFEST_SET`，删除缺失历史引用并备份 `*.bak`；修复了 `bucket_width` 正则匹配问题，并用 `AFTER_SETOPTIONS_OPTS` 校验 `st_event_time_split/bucket_width`。2) `rocks-demo/st_meta_compact_existing.cpp` 调整：`--dry-run` 也执行 `SetOptions` 并打印 `AFTER_SETOPTIONS_OPTS`，便于脚本校验。3) `rocksdb/db/version_set.cc` 增加护栏：当读取 `CURRENT/MANIFEST_SET` 失败时，禁止删除任何 manifest（避免误删仍被引用的 MANIFEST）。 |
| **验证结果** | 运行命令：`powershell -NoProfile -ExecutionPolicy Bypass -File D:\Project\tools\manifest_sharding_preflight.ps1 -DbPath D:\Project\data\verify_wuxi_segment_bucket3600_sst -ExpectedBucketWidthSec 3600 -CompactExe D:\Project\rocks-demo\build\st_meta_compact_existing.exe -RepairMissingHistorical`；输出 `AFTER_SETOPTIONS_OPTS st_event_time_split=1 bucket_width=3600 ...`，最终 `[PREFLIGHT PASS]`。 |
| **是否达到预期** | **部分达到**：preflight 已能稳定验证并给出可执行修复；但“为何 `CURRENT` 滚动时仍会产生缺失历史引用”仍需继续收敛到写入/清理策略的最终一致性（目标是不依赖 repair 也能长期自洽）。 |

## 2026-04-28 — generated_trajs 数据：MANIFEST_SET 多 entry smoke + 单窗 read_bench correctness

| 项 | 内容 |
|----|------|
| **实验目的** | 用仓库自带 `generated_trajs_segments_p80_120.jsonl` 快速构造 ST 点键（`0xE5`）输入，验证（1）Manifest 分片写入能生成 **多 entry** 的 `MANIFEST_SET-*`；（2）在该库上跑一次 `st_meta_read_bench` 做 full vs prune 的 correctness 对拍与基础剪枝统计。 |
| **实验结果** | 1) 生成 CSV：`python tools/generated_jsonl_to_segments_csv.py --in-jsonl data/generated_trajs_segments_p80_120.jsonl --out-dir data/processed/generated --max-segments 5000 --max-points-per-segment 200`（产物：`data/processed/generated/segments_points.csv`、`segments_meta.csv`）。2) Manifest smoke 成功：`tools/manifest_set_natural_multi_entry_smoke.ps1` 输出根目录 `data/experiments/manifest_set_natural_multi_entry_20260428_092834/`，捕获 `MANIFEST_SET-000005 entry_count=2`（见 `summary.tsv` 与 `success_manifest_set.txt`）。3) 建库：`st_meta_smoke.exe --db data/verify_generated_traj_st_e5 --csv data/processed/generated/segments_points.csv --st-keys --max-points 200000 --compact-after-ingest`。4) 单窗读基准：`st_meta_read_bench.exe --db data/verify_generated_traj_st_e5 --prune-mode sst_manifest --verify-kv-results`，该窗下 `full_inwindow_kv=0 prune_inwindow_kv=0 correctness=OK`；`full: bytes_read=12413720 block_read_count=3 wall_us=59851`，`prune: bytes_read=146302 block_read_count=1 wall_us=352`，`block_index_examined=2987 index_skipped_st_disjoint=2987`。 |
| **是否达到预期** | **是（用于 smoke 验证）**：Manifest 分片 smoke 立即抓到多 entry 的 `MANIFEST_SET`；read_bench 的 full/prune 对拍为 OK，且能观察到明显的 block-index 级跳过。 |
| **为什么** | 该数据链路走 `0xE5` 点键，key 内含 `t/x/y`，因此块级 ST meta 与块索引剪枝可以稳定触发；Manifest 分片 smoke 通过 `ROCKSDB_EXPERIMENTAL_MANIFEST_SET_WRITE=1` 等环境变量驱动生成 `MANIFEST_SET-*`，在一次尝试内即可形成多 entry。 |
| **下一步** | 1) 选一个 **非空窗**（`full_inwindow_kv>0`）再跑一次 read_bench，补一条“命中窗” correctness 记录；2) 若要对齐 `Mind.md` 的“段级返回整段/跨桶 Master+Ghost”，需要在段级 schema 与 manifest/ghost 逻辑落地后再做端到端 recall 验证。 |

# 实验与验证记录（Project）

> 约定：每次重要实验后在此追加一条 **概要记录**（目的 / 结果 / 是否达预期 / 原因 / 下一步）。  
> **断线恢复**：会话重连时 AI 可先读项目根目录 **`AGENT_HANDOFF.md`**（进度、路径、命令摘要），再对照本文细节。


---

## 2026-04-04 — `st_meta_compaction_verify`：事件时间切分未体现为多出 SST

| 项 | 内容 |
|----|------|
| **验证目的** | 确认 `experimental_compaction_event_time_*` 在 compaction 中是否使输出 SST 数量相对 baseline 增加（`--split` vs 默认）。 |
| **验证结果** | `GetOptions()` 显示 `split=1`、`max_span` 等与 CLI 一致；但 `total_sst=1`（与 `split_OFF` 相同）；`--bucket-width 1` 仍为 1 个 SST。 |
| **是否符合期望** | 否（若期望看到多 SST）。 |
| **原因** | `CompactRange` 在满足 `IsTrivialMove()` 时走 **PerformTrivialMove**，只改 manifest、不重写 SST，**CompactionOutputs / ShouldStopBeforeEventTime 不执行**。另：曾用 `compression_per_level`+Snappy 逼非 trivial move，但当前 **rocksdb 构建未链 Snappy**，Open 报错。 |
| **已采取动作** | `st_meta_compaction_verify`：`--split` 时改用 **`CompactFiles`（默认 `allow_trivial_move=false`）** 强制真实 merge；去掉对 Snappy 的依赖。 |

---

## 2026-04-04 — `manifest_bucket_rtree_validate`：合成「时间桶 + R 树」基准

| 项 | 内容 |
|----|------|
| **验证目的** | 在 **不接 RocksDB** 的前提下，验证 §7.2–7.3 抽象模型：**时间桶多挂 + 每桶空间索引** 相对全表暴力（时间∩MBR）的 **正确性** 与 **性能趋势**。 |
| **验证结果** | **正确性**：`mismatched_queries=0`。宽查询窗（如 2 万文件、无 `--query-*`）：`brute/indexed < 1`（索引略慢）。**尖查询窗**（如 20 万文件、`--query-time-max 40 --query-mbr-hw-max 0.2`）：`avg \|brute\|≈0.35`，**`brute/indexed≈1.08`（索引更快）**。 |
| **是否符合期望** | 部分符合：正确性符合；性能上 **高选择性** 下符合「索引应更优」，**低选择性/中小 N** 下暴力扫描仍可能更快或接近。 |
| **原因** | 宽窗时命中文件多，线性扫描 cache 友好；索引有过桶、树遍历、跨桶重复再时间 refine 的常数项。缩小时间与空间窗后命中极少，桶+R 树跳过大量文件。暴力侧曾用 `std::set` 会高估索引劣势，已改为 **`unordered_set` + reserve** 使对比更公平。 |
| **已采取动作** | 增加 `--query-time-max`、`--query-mbr-hw-max`；小桶线性扫描（≤32）、桶表 `vector` 下标访问；输出 **`brute/indexed`（>1 表示 indexed 更快）**。 |

---

## 自动化实验脚本

- **推荐运行**：在项目根目录执行 **`tools\run_st_experiments.cmd`**，或 `powershell -NoProfile -ExecutionPolicy Bypass -File ".\tools\run_st_experiments.ps1"`。  
  在 Windows 上 **双击 `.ps1` 或错误关联时常会「用编辑器打开」而不是执行**，不要用那种方式当「运行脚本」。  
- **`tools/run_st_experiments.ps1`**（由 `.cmd` 调用）：依次跑  
  - **A1–A3**：`manifest_bucket_rtree_validate`（宽窗 / 尖窗 / 快速正确性；**A1/A2 含 `--open-ns 100000`** 读路径加权）  
  - **B1–B2**：`st_meta_compaction_verify`（`--split` + bucket=1 vs OFF）  
  日志：`experiment_logs/run_YYYYMMDD_HHMMSS.log`。  
- 可选参数：`-RocksDemoBuild D:\path\to\rocks-demo\build`、`-DataRoot D:\path\to\data`。

---

## 接下来要干什么（当前共识）

1. **真实读路径 IO（已加第一步工具）**：`rocks-demo` 新增 **`st_meta_read_bench`**：对 **已有 ST 库**（**默认示例 DB / 窗**：无锡段库 + **`EXPERIMENTS_AND_SCRIPTS.md` §0.5**）**不删不建**，测量 **全表迭代** vs **`ReadOptions::experimental_st_prune_scan`** 的 **墙钟时间**、**`PerfContext::block_read_count`**、**`IOStatsContext::bytes_read` / `read_nanos`** 及 **Statistics 缓存命中**。见下文「命令」。**仍待做**：与 **Manifest 级 file ST 元数据** 联合剪枝（若与块级剪枝并列）、以及 **多查询循环 / 冷缓存协议** 固化。  
2. **真实 Manifest / Version**：**（部分落地）** `FileMetaData` 已含 **`has_st_file_meta` + `st_file_meta`**（与块尾同构），经 **`kNewFile4` custom tag 17** 写入 MANIFEST；来源为 flush/compaction 输出 SST 的 **`rocksdb.experimental.st_file_bounds`**（由开启 **`experimental_spatio_temporal_meta_in_index_value`** 时的索引构建聚合）。**仍待做**：Version 读路径 **按查询窗剪枝**、内存 **时间桶 + R 树**、**`OpenTable` 计数** 与合成工具对齐。  
3. **合成工具**：继续用 **尖查询参数** 扫 `files` / `bucket-width` / 查询窗；同时看 **`brute/indexed`（CPU）** 与 **`weighted_brute/indexed`（`--open-ns`）**；与 **选择性** 及 **Open 成本假设** 对齐（**不等价**于真实 IO，与 **`st_meta_read_bench`** 互补）。  
4. **Compaction 事件时间切分**：若仍要验证 SST 切分形态，保持 **`CompactFiles` 或禁用 trivial move** 的路径，避免再次被 trivial move 短路。

**`st_meta_read_bench` 示例（先 `cmake --build` `--target st_meta_read_bench`）** — 默认剪枝窗已为 **无锡 w01**（见 **`EXPERIMENTS_AND_SCRIPTS.md` §0.5**）；仅传 `--db` 即可与文档默认对齐：

```text
rocks-demo\build\st_meta_read_bench.exe --db D:\Project\data\verify_wuxi_segment_bucket3600_sst
```

显式写出与 cov CSV 首行相同的窗（可选）：

```text
rocks-demo\build\st_meta_read_bench.exe --db D:\Project\data\verify_wuxi_segment_bucket3600_sst ^
  --prune-t-min 1596200108 --prune-t-max 1596201324 ^
  --prune-x-min 111.788384 --prune-x-max 127.462201 --prune-y-min 28.777820 --prune-y-max 38.854327
```

**Legacy（Geolife 点库对照）**：`verify_traj_st_full` + 旧北京窗仅用于历史复现，**不再作为项目默认测试窗**；见 §0.5。

仅跑剪枝路径、跳过全表（大库省时）：加 **`--no-full-scan`**。加大缓存减轻重复读盘： **`--large-cache`** 或 **`--block-cache-mb 256`**。

---

## 追加记录模板（复制一节填写）

```markdown
## YYYY-MM-DD — <简短标题>

| 项 | 内容 |
|----|------|
| **验证目的** |  |
| **验证结果** | （关键数字 / 日志片段可摘要） |
| **是否符合期望** | 是 / 否 / 部分 |
| **原因** |  |
| **下一步** |  |
```

---

## 2026-04-05 — `run_st_experiments.ps1` 首次跑通（脚本有 bug，已修）

| 项 | 内容 |
|----|------|
| **验证目的** | 一键跑 A1–A3（mbrv）+ B1–B2（compaction_verify）。 |
| **验证结果** | 日志中 **A1/A2/A3 参数与输出完全一致**（均为 `files=5000 queries=2000`、`query_time_max=0`）；**B1 显示 `MODE split_OFF`** 且 PowerShell 在 `rocksdb.lib MUST...` 的 stderr 处 **报错终止**（`NativeCommandError`）。 |
| **是否符合期望** | 否（脚本未把各组参数传给 exe；B 段被 stderr + `Stop` 打断）。 |
| **原因** | 1）函数参数命名为 **`$args`**，与 PowerShell **自动变量 `$args` 冲突**，`@args` 未正确绑定，子进程相当于 **无 CLI 参数**（mbrv 使用内置默认 5000/2000）。2）**`$ErrorActionPreference = Stop`** 时，原生程序写 **stderr** 会被当成错误。3）B1 因未收到 `--split`，故为 `split_OFF`。 |
| **已修复** | `Run-Cmd` 参数改为 **`$ArgumentList`**；执行 exe 时临时 **`$ErrorActionPreference = Continue`**。请 **重新拉脚本后** 再运行 `tools\run_st_experiments.cmd`。 |
| **下一步** | 重跑全流程；若 B 段仍失败，再查 `st_meta_compaction_verify` 与 `rocksdb.lib` 的 **Release 对齐**。 |

## 2026-04-05 — 脚本修复后重跑（本机验证）

| 项 | 内容 |
|----|------|
| **验证目的** | 确认 `Run-Cmd` 参数与 `ErrorActionPreference` 修复后，A/B 全流程可跑完。 |
| **验证结果** | **A1** `files=20000 queries=5000`；**A2** `files=200000` 且 `query_time_max=40 query_mbr_hw_max=0.2`；**A3** `files=2000 --check-only`；**B1** `MODE split_ON`，`CompactFiles ok`，`exit 0`；**B2** `MODE split_OFF`，`CompactRange ok`，`exit 0`。stderr 中的 `rocksdb.lib MUST...` 提示仍打印，但 **不再终止脚本**。 |
| **是否符合期望** | 是。 |
| **原因** | 与上节「已修复」一致；B 段 Release 与工具一致时 Open/Compaction 正常。 |
| **下一步** | 按需把本机 `experiment_logs\run_*.log` 摘要进报告；长期仍按「接下来要干什么」推进真实 Manifest。 |

## 2026-04-05 — `open-ns` 读路径模型 + 全流程（`run_20260405_133940.log`）

| 项 | 内容 |
|----|------|
| **验证目的** | 在 **A1/A2** 中引入 **`--open-ns 100000`（100µs/Open）**，用「无文件级 MBR 则按时间相交逐个 Open」vs「Manifest t+MBR 后只 Open 结果集」验证 **读路径叙事**；**A3** 仍为纯 CPU + opens 统计（`open_ns=0`）；**B1/B2** 复验 compaction。 |
| **验证结果** | **A1**：`brute/indexed_opens≈57.3`，CPU `brute/indexed≈0.79`，**`weighted_brute/indexed≈56.0`**。**A2**：`brute/indexed_opens≈402`，CPU `brute/indexed≈1.11`，**`weighted_brute/indexed≈25.1`**。**A3**：`mismatched_queries=0`，`brute/indexed_opens≈58`（无加权行）。**B1** `split_ON` `total_sst=500`；**B2** `split_OFF` `total_sst=1`；均 `exit 0`（脚本清理旧 DB 目录属正常）。 |
| **是否符合期望** | 是：在 **100µs/Open** 假设下，**宽窗与尖窗均显示加权后索引侧大幅更优**；与「真实读路径成本在 Open」一致。CPU-only 仍可能 `brute/indexed<1`（A1）。 |
| **原因** | 宽查询下 **时间相交文件数**仍远多于 **\|结果\|**（A1 约 2285 vs 40 opens/query）；尖查询下相交数降到约 141 而结果约 0.35，**opens 比**更大。加权项 `opens×open_ns` 主导总时间，故 **`weighted_brute/indexed` ≫ 1**。 |
| **下一步** | 按真实环境校准 **`open_ns`**（或扫多档）；Manifest 落地后对比 **真实 `OpenTable` 计数** 与合成指标。 |

---

## 2026-04-05 — 存储粒度说明（段 vs 点）

| 项 | 内容 |
|----|------|
| **结论** | **「整轨迹段作为基本存储单元」尚未实现**：`segments_points.csv` 点表 + `trajectory_validate` / `st_meta_smoke --csv`（默认键）均为 **每点一条 KV**；`segments_meta.csv` 仅为段级汇总。若以段为单元需另定 schema。 |

---

## 2026-04-05 — ST user key 真实轨迹灌库 + `manifest_dump` + 全流程 `run_20260405_152258`

| 项 | 内容 |
|----|------|
| **验证目的** | 用 **`st_meta_smoke --csv --st-keys`** 在 **Geolife 加工点表** 上验证 **0xE5 ST key** 路径；**`ldb manifest_dump --hex`** 对照 MANIFEST；再跑 **`tools\run_st_experiments.ps1`**（A1–A3 + B1–B2）。 |
| **验证结果** | **`st_meta_smoke`**：`--max-points 5000` / `50000` / **`-1`（全表 1279214 点）** 均 **Put + Flush + Get OK**；库目录 `data\verify_traj_st_keys`、`data\verify_traj_st_full`。**`manifest_dump --verbose --hex`**：边界键为 **`E5…`**，`PersistUserDefinedTimestamps: true`，与 ST 键一致。**日志 `experiment_logs\run_20260405_152258.log`**：**A1** `mismatched_queries=0`，`weighted_brute/indexed≈56.10`；**A2** `mismatched_queries=0`，`weighted_brute/indexed≈25.77`；**A3** `mismatched_queries=0`；**B1** `split_ON` `total_sst=500` `L6=500`；**B2** `split_OFF` `total_sst=1` `L6=1`；B 段均 `exit 0`。stderr 仍打印 `rocksdb.lib MUST...`，脚本未中断。 |
| **是否符合期望** | 是。 |
| **原因** | 重编后的 **`st_meta_smoke.exe`** 含 **`--st-keys`**；合成 mbrv 与 compaction verify 参数与此前「open-ns / CompactFiles」设计一致。 |
| **下一步** | 按需对 **`verify_traj_st_full`** 做 **SST/manifest 体量** 摘要；Version **读路径剪枝** 与真实 **OpenTable** 计数仍按「接下来要干什么」推进。 |

---

## 2026-04-05 — `st_meta_read_bench`：`verify_traj_st_full` 首跑（默认宽剪枝窗）

| 项 | 内容 |
|----|------|
| **验证目的** | 在 **真实已灌库**（`verify_traj_st_full`，约 **1279214** 条 ST user key）上对比 **全表迭代** 与 **`experimental_st_prune_scan`**，用 **PerfContext / IOStatsContext / Statistics** 观察 **真实读路径** 是否因剪枝减少 IO（相对合成 `open_ns` 模型）。 |
| **验证结果** | 命令：`st_meta_read_bench.exe --db D:\Project\data\verify_traj_st_full --prune-t-min 1224600000 --prune-t-max 1224800000 --prune-x-min 116.2 --prune-x-max 116.4 --prune-y-min 39.9 --prune-y-max 40.1`。`full_scan`：`keys=1279214`，`block_read_count=5`，`wall_us≈233473`，`IOStats bytes_read=78704054`。`prune_scan`：`keys=1279138`，`block_read_count=5`，`wall_us≈226221`，`bytes_read=78704054`（与 full 相同）。`key_selectivity≈0.999941`，`block_read_ratio=1.0`，`wall_time_ratio≈0.969`。Statistics：`BLOCK_CACHE_DATA_MISS=38387`，`BLOCK_CACHE_DATA_HIT=0`，`INDEX_MISS=4`，`INDEX_HIT=2`。 |
| **是否符合期望** | **部分**：工具跑通、指标可读；**未出现「剪枝显著省 IO」**，与「宽窗下应接近全表」一致。 |
| **原因** | 剪枝窗 **覆盖几乎全部数据**（仅少 **76** 条 key），迭代路径与读盘范围与全表扫 **几乎相同**，故 **`bytes_read`、`block_read_count` 无差异**；`wall_us` 略降主要来自 **略少 key 的 CPU 路径**。`PerfContext::block_read_count` 与 `BLOCK_CACHE_DATA_MISS` 口径不同，不宜混为一谈。 |
| **下一步** | ~~尖窗重跑~~ 见下一节「尖窗复跑」结论。 |

---

## 2026-04-05 — `st_meta_read_bench`：尖窗复跑（与宽窗对比）

| 项 | 内容 |
|----|------|
| **验证目的** | 缩小 **t/x/y** 后，观察 **`prune_scan keys`、IO** 相对 **宽窗首跑** 是否变化，验证 **块级 ST 剪枝** 在 **`verify_traj_st_full`** 上的选择性。 |
| **验证结果** | **Run1**（`--no-full-scan`）：`t=[1224730000,1224730500]`，`x=[116.31,116.32]`，`y=[39.984,39.986]` → `prune_scan keys=1279138`，`bytes_read=78704054`，`block_read_count=5`，`wall_us≈233997`，`BLOCK_CACHE_DATA_MISS=19193`。**Run2**（full+prune，同窗）：`full keys=1279214`，`prune keys=1279138`，`key_selectivity≈0.999941`，`block_read_ratio=1.0`，`bytes_read` 均为 `78704054`，`wall_time_ratio≈0.9458`，`BLOCK_CACHE_DATA_MISS=38387`。 |
| **是否符合期望** | **否**（若期望尖窗显著减少 key 与读盘）：**`prune_scan keys` 与宽窗首跑完全相同（1279138）**，**`bytes_read` / `block_read_count` 与宽窗同量级**。 |
| **原因** | **`experimental_st_prune_scan` 仅在索引层跳过「整块」ST 包围盒与查询窗不相交的数据块**（`AdvanceIndexPastPrunedForward`，`block_based_table_iterator.cc`）；**块内 key 不按窗过滤**，打开块后仍 **`Next()` 输出块内全部 key**。若 **`has_st_meta==false` 则不再跳过后续块**。当前库形态下，仅排序 **最前** 若干块与窗不相交（约 **76** key），故 **full 比 prune 多 76**；**收窄窗未改变「被跳过的块集合」**（与宽窗相同），故 **尖窗与宽窗 `prune keys`、`bytes_read` 一致**。 |
| **下一步** | **`ldb sst_dump`** / **`manifest_dump`** 确认 **SST 个数、每表 data block 数、index 是否带 ST meta**；**compaction** 或调 **`target_file_size_base` / `block_size`** 制造 **多文件、更小包围盒**；长期：**块内按窗过滤**、**Manifest 文件级剪枝**；可试 **极端尖窗**（如 1 秒 `t`）若 `keys` 仍不变再查 **读写表选项一致性**。 |

## 2026-04-05 — `sst_dump`：`verify_traj_st_full\000009.sst` 表属性

| 项 | 内容 |
|----|------|
| **验证目的** | 确认 **`verify_traj_st_full`** 中 SST 的 **data block 数量、条目数、是否带 `rocksdb.experimental.st_file_bounds`**，解释 **`st_meta_read_bench` 剪枝几乎无效** 是否与 **物理布局** 一致。 |
| **验证结果** | `sst_dump --show_properties`：`# data blocks: 13447`，`# entries: 895267`，`user defined timestamps persisted: true`，**Raw user collected properties 含 `# rocksdb.experimental.st_file_bounds`**（file-level ST）。过程中打印 **`Corruption: bad block contents`**，但 **Table Properties 段已输出**。 |
| **是否符合期望** | **部分**（表结构信息仍有效）。**条目差** 已由下文 **「计划第 1 步」** 澄清：**两 SST 合计 1279214**。 |
| **原因** | 同下；**Corruption** 与 **`VerifyChecksum: OK`** 的关系见 **「计划第 1 步」** 一节。 |
| **下一步** | ~~列 SST 汇总 entries~~ 已完成；转 **块级 meta 诊断 / Manifest 文件级剪枝**（见最新「计划第 1 步」下一步）。 |

## 2026-04-05 — 执行「计划第 1 步」：`traj_db_audit` + `audit_sst_entries.ps1`（本机结果）

| 项 | 内容 |
|----|------|
| **验证目的** | **校验 DB**、**汇总各 SST `num_entries`**（manifest vs `sst_dump`），解释 **单文件 895267 vs 全库 1279214**，并判断 **`sst_dump` Corruption** 是否表示库损坏。 |
| **验证结果** | **`traj_db_audit`**：`VerifyChecksum` **OK**。`live_sst count=2`：**L0 `000011.sst`** `num_entries=383947`；**L0 `000009.sst`** `num_entries=895267`；**`sum_num_entries=1279214`**，`sum_file_size=78892620`；**`expect_keys=1279214` 与 sum 一致**。**`audit_sst_entries.ps1`**：两文件 stderr 均含 **Corruption** 提示，`exit=0`；解析 **`# entries`** 得 **895267 + 383947 = 1279214**，与 manifest **一致**。 |
| **是否符合期望** | **是**：**无多文件遗漏**；**校验通过**；**manifest 与 sst_dump 条目和一致**。 |
| **原因** | 全库键分布在 **两个 L0 SST**；先前只看 **`000009.sst`** 故 **895267 < 1279214**。**`DB::VerifyChecksum` 通过** 表明 RocksDB 视角下 **表文件一致**；**`sst_dump` 仍报 `bad block contents`** 更可能来自 **工具遍历 data block 路径**（如压缩/校验实现与 dump 路径差异），**不应单独据此判库坏**；**ps1 已用 Process 合并流**，避免 stderr 中断。 |
| **下一步** | **计划第 2 步**：块级 **`has_st_meta` / 包围盒** 诊断（日志或小型 RocksDB 补丁），或 **Version 侧按 file-level `st_file_bounds` / manifest ST 减少 Open**；可选 **compaction** 观察 prune 行为是否随 **多文件/更紧块界** 改善。 |

**命令**（可复现）：

```bat
cd /d D:\Project\rocks-demo\build
cmake --build . --config Release --target traj_db_audit -j 8
traj_db_audit.exe --db D:\Project\data\verify_traj_st_full --expect-keys 1279214
powershell -NoProfile -ExecutionPolicy Bypass -File D:\Project\tools\audit_sst_entries.ps1
```

*（以下由后续对话追加）*

## 2026-04-05 — 工具 `st_meta_sst_diag`（块级诊断 + 文件级相交）

| 项 | 内容 |
|----|------|
| **验证目的** | **先块级**：遍历 **block-based index**，统计 **`has_st_meta` / 无 tail 的 index 条数**，并在给定 **`--window`** 时按与 **`experimental_st_prune_scan` 相同的轴对齐不相交规则** 统计「会被整块跳过 / 相交 / 无 ST tail 不参与剪枝」；**再文件级**：从表属性解码 **`rocksdb.experimental.st_file_bounds`**，并报告与同一 **`--window` 是否相交**。 |
| **实现** | RocksDB fork：`tools/st_meta_sst_diag.cc`（已加入 `tools/CMakeLists.txt` 的 `CORE_TOOLS`）；`SstFileDumper::GetBlockBasedTableOrNull()`（`static_cast_with_check`，避免 Windows **`/GR-`** 下 **`dynamic_cast` 不可用**）。 |
| **用法** | `st_meta_sst_diag.exe [--window T_MIN T_MAX X_MIN Y_MIN X_MAX Y_MAX] [--verify-checksum] file1.sst [file2.sst ...]` |
| **构建** | `cmake --build <rocksdb/build> --target st_meta_sst_diag`（与 `sst_dump` / `ldb` 同级）。 |
| **下一步** | 在 **`verify_traj_st_full`** 两 SST 上跑一遍，把 **index 条数 vs `num_data_blocks`、`has_st_meta` 占比、与 bench 同窗的 prune 计数** 记入本表。 |

## 2026-04-05 — 概念：SST 只有两个 ≠ Manifest「失效」；`st_meta_sst_diag` 编译与路径

| 项 | 内容 |
|----|------|
| **验证目的** | 澄清 **L0 仅 2 个 SST** 是否导致 **Manifest 失效**；记录 **`st_meta_sst_diag` 编译错误 C2248** 与用户命令混淆。 |
| **验证结果** | **`audit_sst_entries.ps1`**：`sst_files=2`，`sum_entries=1279214`，与 manifest / `traj_db_audit` 一致。**`st_meta_sst_diag`** 若在 **`rocks-demo\build`** 下执行会报「不是内部或外部命令」——可执行文件在 **`rocksdb\build\tools\`**，与 **`sst_dump.exe`** 同目录。 |
| **是否符合期望** | **Manifest**：**否**（见下）。**工具路径**：在 `rocks-demo` 下找不到 exe **符合预期**（未安装到 PATH、也未复制到该目录）。 |
| **原因** | **Manifest 不会因 SST 数量少而失效**：它仍是 **当前版本层级的权威元数据**（文件号、层、大小、`num_entries` 等）；`traj_db_audit` 已证明 **manifest 与两文件一致**。SST **少**带来的是 **文件级剪枝粒度粗**（一次只能按「整文件」跳过），以及 **单文件很大 → 块级 MBR 覆盖范围大**，这与 **`experimental_st_prune_scan` 尖窗与宽窗读盘相近** 的现象 **更相关**，而不是 manifest 坏了。**C2248**：`BlockBasedTable::NewIndexIterator` 为 **private**，外部工具不能直接调；已改为 **`friend class StMetaSstDiag`** + 在 `st_meta_sst_diag.cc` 内实现 **`StMetaSstDiag::WalkIndex`**。 |
| **下一步** | `cmake --build D:\Project\rocksdb\build --target st_meta_sst_diag` 成功后执行：`D:\Project\rocksdb\build\tools\st_meta_sst_diag.exe D:\Project\data\verify_traj_st_full\000009.sst ...`（**`cmake --build` 只接受一个 target**，不要把 exe 路径拼在 target 后面）。 |

## 2026-04-05 — `st_meta_sst_diag`：`Corruption: bad entry in block`（SstFileDumper 未开 ST index 解码）

| 项 | 内容 |
|----|------|
| **验证目的** | 解释 **`000009.sst`** 上 **表属性 / `st_file_bounds` 可读**，但 **index 遍历** 报 **`bad entry in block`**。 |
| **验证结果** | **根因**：`SstFileDumper` 打开 SST 时 **`BlockBasedTableOptions::experimental_spatio_temporal_meta_in_index_value` 默认为 false**，但 **fork 写入的 index value 带 ST tail**；`IndexBlockIter::DecodeCurrentValue` 在未解码 tail 时会 **缩短 `value_`**，而 **`NextEntryOffset()` 依赖 `entry_`/`value_` 与物理编码对齐**，导致 **后续 entry 解析错位** → **`DecodeEntry` 失败** → `CorruptionError("bad entry in block")`。 |
| **是否符合期望** | **修复前否**；**修复后** 应在 **`SetTableOptionsByMagicNumber`** 中若存在 **`rocksdb.experimental.st_file_bounds`** 则 **`experimental_spatio_temporal_meta_in_index_value = true`**（与当前 fork「有 file bounds 即有 index tail」一致），再 **`cmake --build … --target rocksdb st_meta_sst_diag`** 重跑诊断。 |
| **原因** | 见上；与 **`sst_dump` 扫 data 块 stderr** 同类：**工具读路径选项与文件实际格式不一致** 会表现为 corruption，**不等于** SST 物理损坏。 |
| **下一步** | 重编后再次运行 **`st_meta_sst_diag.exe`** 两 SST，确认 **index 条数 = `num_data_blocks`**、`has_st_meta` 统计合理。 |

## 2026-04-05 — `st_meta_sst_diag` 实测：`verify_traj_st_full` 两 SST（修复 SstFileDumper 后）

| 项 | 内容 |
|----|------|
| **验证目的** | 确认 **index 遍历** 正常，且 **块级 ST tail** 与 **`num_data_blocks`** 一致。 |
| **验证结果** | **`000009.sst`**：`num_data_blocks=13447`，`index entries=13447`，**`has_st_meta=13447`，无 tail=0**；**`st_file_bounds`**：`t[1201959048,1246779915]`，`x[0,145.014]`，`y[0,68.0948]`。**`000011.sst`**：`num_data_blocks=5747`，`index entries=5747`，**`has_st_meta=5747`，无 tail=0**；**`st_file_bounds`**：`t[1201959086,1372747803]`，`x[-8.77824,119.416]`，`y[0,51.0032]`。**合计 index 条数** `13447+5747=19194`（两文件各自与属性一致；全库 **1279214** 为 **KV 条数**非 index 条数）。 |
| **是否符合期望** | **是**：**每个 data block 对应一条带 ST tail 的 index entry**；**文件级 bounds** 可解码。 |
| **原因** | **`SstFileDumper` 按 `st_file_bounds` 打开 `experimental_spatio_temporal_meta_in_index_value`** 后，index 解码与磁盘格式一致。 |
| **下一步** | 见下 **「对照流程」**（与 **`st_meta_read_bench` 同窗的 `st_meta_sst_diag --window`**）。 |

---

## 对照流程：`st_meta_read_bench` ↔ `st_meta_sst_diag --window`

**参数对应**（与 `ReadOptions::experimental_st_prune_scan` / `block_based_table_iterator.cc` 一致）：

| `st_meta_read_bench` | `st_meta_sst_diag --window`（顺序） |
|----------------------|--------------------------------------|
| `--prune-t-min` / `--prune-t-max` | 第 1、2 个数：`T_MIN` `T_MAX` |
| `--prune-x-min` / `--prune-x-max` | 第 3、5 个数：`X_MIN` … `X_MAX` |
| `--prune-y-min` / `--prune-y-max` | 第 4、6 个数：`Y_MIN` … `Y_MAX` |

即：`--window <t_min> <t_max> <x_min> <y_min> <x_max> <y_max>`（**先 x 再 y 交替**，与 bench 六个标量一一对应）。

**一键跑宽窗 + 尖窗**（路径随仓库根目录；`st_meta_sst_diag` 在 **`rocksdb\build\tools`**）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\Project\tools\st_diag_align_read_bench.ps1
```

**手工宽窗**（与 `st_meta_read_bench` **默认**及 `Record.md` 宽窗实验一致）：

```bat
D:\Project\rocksdb\build\tools\st_meta_sst_diag.exe --window 1224600000 1224800000 116.2 39.9 116.4 40.1 D:\Project\data\verify_traj_st_full\000009.sst D:\Project\data\verify_traj_st_full\000011.sst
```

**手工尖窗**（与 `Record.md` 尖窗 **Run1** 一致）：

```bat
D:\Project\rocksdb\build\tools\st_meta_sst_diag.exe --window 1224730000 1224730500 116.31 39.984 116.32 39.986 D:\Project\data\verify_traj_st_full\000009.sst D:\Project\data\verify_traj_st_full\000011.sst
```

**先 bench 再 diag**（同机同库，便于贴日志）：

```bat
D:\Project\rocks-demo\build\st_meta_read_bench.exe --db D:\Project\data\verify_traj_st_full --no-full-scan
D:\Project\rocksdb\build\tools\st_meta_sst_diag.exe --window 1224600000 1224800000 116.2 39.9 116.4 40.1 D:\Project\data\verify_traj_st_full\000009.sst D:\Project\data\verify_traj_st_full\000011.sst
```

**如何读 `st_meta_sst_diag` 输出**：

- **每个 SST 末尾**：`would skip (disjoint ST)` = 在 **SeekToFirst + 仅 `Next` 前进** 语义下，索引里 **整块包围盒与查询窗不相交** 的 **data block 数**（与迭代器 **`AdvanceIndexPastPrunedForward`** 一致）；`would keep (intersects)` = 可能读到 data 的块数；`no ST tail` 在你们库上应为 **0**。
- **两文件相加**的 **`would skip`**：若 **宽窗与尖窗相同**，则与 **`st_meta_read_bench` 尖窗与宽窗 `prune_scan keys` 相同（1279138）** 同因：**被跳过的块集合未变**。
- **文件级 `file vs query window`**：两 SST 的 **`st_file_bounds` 与窗均相交** 时，**文件级无法省打开**；剪枝只剩 **块级**。
- **diag 不输出 key 条数**：**1279138** 只在 **`st_meta_read_bench`** 里统计；diag 只解释 **有多少块被跳过/保留**。

## 2026-04-05 — 同窗对照实测：`st_diag_align_read_bench.ps1`（宽窗 vs 尖窗）

| 项 | 内容 |
|----|------|
| **验证目的** | 用 **`st_meta_sst_diag --window`** 与 **`st_meta_read_bench`** 同窗，验证 **尖窗与宽窗 `prune_scan keys` 相同** 是否对应 **相同的「被跳过 data block」集合**。 |
| **验证结果** | **宽窗**（`t∈[1224600000,1224800000]`，`x∈[116.2,116.4]`，`y∈[39.9,40.1]`）：**`000009.sst`** `would skip=1`，`would keep=13446`；**`000011.sst`** `would skip=0`，`would keep=5747`；两文件 **`file vs query window` 均为 INTERSECTS**。**尖窗**（`t∈[1224730000,1224730500]`，`x∈[116.31,116.32]`，`y∈[39.984,39.986]`）：**逐文件 `skip`/`keep` 与宽窗完全一致**（仍为 **1 + 0** 块 disjoint）。 |
| **是否符合期望** | **是**（与此前 bench 现象一致）：**收窄窗未改变**「与查询窗 **不相交** 的块」——全库仅 **1 个** index/data block（在 **`000009.sst`**）在两种窗下都被判 **disjoint**；**`000011.sst`** 内 **0** 块 disjoint。 |
| **原因** | **块级 MBR**：只有排序靠前的那 **1** 块的时空包围盒与 **宽窗、尖窗** 都不相交，其余 **19193** 块与 **两窗均相交**。故 **`AdvanceIndexPastPrunedForward`** 跳过块数相同 → **`st_meta_read_bench` 下宽/尖 `prune_scan keys` 均为 1279138**。**`full − prune = 76` key** 与 **仅跳过 1 个 data block** 一致：该块内约 **76** 条 KV（块内其余 key 仍随迭代读出需看迭代顺序；等价表述：**少扫的 key 数 = 被整块跳过的块内 key 数**）。**文件级**两 SST 与窗均 **INTERSECTS**，**无法**靠 file bounds 省打开文件。 |
| **下一步** | 若要让 **尖窗** 多跳过块：需 **更紧的块级界**（更多 SST/更小 `block_size`/compaction）或 **块内过滤**；若要让 **文件级** 生效：多文件 + 与窗 **DISJOINT** 的 `st_file_bounds`。 |

## 2026-04-05 — 继续：在库副本上 compaction 拆 SST（`st_meta_compact_existing`）

| 项 | 内容 |
|----|------|
| **验证目的** | 在 **不破坏 `verify_traj_st_full` 原件** 的前提下，通过 **更小 `target_file_size_base` + `CompactRange`** 增加 **SST 个数**，观察 **`st_meta_sst_diag` 的 `would skip` / 文件级 `INTERSECTS`** 是否更易出现「多文件 / 多块」差异。 |
| **实现** | **`rocks-demo/st_meta_compact_existing.cpp`**（`cmake --build … --target st_meta_compact_existing`）：打开方式与 **`st_meta_read_bench`** 一致（**ST index tail**）；**`--dry-run`** 只列 live SST；否则 **`SetOptions`** + 全表 **`CompactRange`**。**`tools/copy_compact_traj_st.ps1`**：`robocopy` 到 **`data/verify_traj_st_compact_work`**（默认）后跑 compact，并在存在 **`st_meta_sst_diag.exe`** 时用 **宽窗** 对所有 `*.sst` 跑一遍诊断。 |
| **用法** | 先编：`cmake --build D:\Project\rocks-demo\build --target st_meta_compact_existing`。首次：`powershell -NoProfile -ExecutionPolicy Bypass -File D:\Project\tools\copy_compact_traj_st.ps1`（若目标目录已存在会报错，需先删 **`verify_traj_st_compact_work`**）。仅看当前 SST 数：`st_meta_compact_existing.exe --db <dir> --dry-run`。 |
| **注意** | **全库 compact** 对 **~128 万键** 可能 **耗时数分钟～更久**、占磁盘；务必只对 **副本** 操作。拆小文件后 **块数会增加**，**`st_meta_read_bench` 的 `block_read_count` 与 prune 行为** 可能变化，需 **冷/热缓存** 分开看。 |

## 2026-04-05 — 实测：`copy_compact_traj_st.ps1`（compact 速度与拆 SST 后 diag）

| 项 | 内容 |
|----|------|
| **验证目的** | 副本 + **`target_file_size_base=32MiB`** + **`CompactRange`** 后 **SST 个数、块级 skip、文件级相交**；并记录 **为何一次跑完感觉「很快」**。 |
| **验证结果** | **`robocopy`** 约 **1s**、**~75.5MB**。**`BEFORE`**：`live_sst=2`（L0 `000009` 895267 entries，`000011` 383947）。**`SetOptions` ok**。**`AFTER`**：`live_sst=3` 均在 **L6**：`000040` 552057、`000041` 552029、`000042` 175128（**合计 1279214**，与全库键数一致）。**宽窗 `st_meta_sst_diag`**：`000040` skip **0**；`000041` skip **1**；`000042` skip **0**；三文件 **file vs window 均为 INTERSECTS**。 |
| **是否符合期望** | **部分**：**SST 由 2→3**、**data block 总数**约 **8248+8248+2617=19113**（略少于拆前 19194，与 **更大 SST / 不同合并边界** 有关，属正常）。**块级 disjoint 仍为全库 1 块**（落在 **`000041`**），与拆前「仅 1 块与宽窗不相交」**一致**——** compaction 未 magically 多出与窗不相交的块**。**文件级**仍 **无法** 跳过整文件。 |
| **原因（为何这么快）** | **数据量不大**：全库 SST 仅 **~76MB**，**整库已在 OS 页缓存 / SSD** 时，**读两遍量级**的合并可在 **秒级～十余秒**完成（具体看 CPU/盘）。**输入仅 2 个 L0 文件**，输出 **3 个** L6，**无跨盘、无网络**。**之前「可能数分钟」** 是针对 **冷盘、更大 DB、或更激进 target** 的保守说法；本机 **热缓存 + 小 footprint** 下 **快是正常现象**。 |
| **下一步** | 对 **`verify_traj_st_compact_work`** 跑 **`st_meta_read_bench`**（宽/尖窗）看 **`block_read_count` / `prune_scan keys`** 是否相对 **2-SST 原件**有变化；若要 **更多 SST**，可再减小 **`--target-file-mb`**（如 16）或多次 compact / 调 **`max_bytes_for_level_base`**（需评估写放大）。 |

## 2026-04-05 — `verify_traj_st_compact_work`：`st_meta_read_bench` + 尖窗 `st_meta_sst_diag`

| 项 | 内容 |
|----|------|
| **验证目的** | **Compact 后（3×L6 SST）** 上对比 **原件** 的 **`prune_scan keys` / `block_read_count`**，并用 **尖窗** **`st_meta_sst_diag`** 看 **disjoint 块数** 是否与 **bench** 一致。 |
| **验证结果** | **`st_meta_read_bench`**（`--db verify_traj_st_compact_work`）：**`full_scan`** `keys=1279214`，`block_read_count=9`，`bytes_read≈78737063`，`wall_us≈235528`。**宽窗 prune**：`keys=1279213`（较 full **少 1**），`block_read_count=9`（与 full **相同**），`wall_time_ratio(prune/full)≈0.89`。**仅宽窗 prune 第二遍**：`keys=1279213`，`block_read_count=9`。**尖窗 prune**：`keys=1279212`（较 full **少 2**），`block_read_count=9`。**`st_meta_sst_diag` 尖窗**（三 SST）：`000040` `would skip=1`；`000041` `would skip=1`；`000042` `would skip=0`；**合计 disjoint 块 = 2**；三文件 **file 均为 INTERSECTS**。 |
| **是否符合期望** | **部分**：相对 **2-SST 原件**（`Record.md`）：**`block_read_count` 5→9**（**更多 SST / L6** → 更多 index/data 次读，属预期）。**宽窗 `prune_scan keys` 由 1279138 变为 1279213**（与 full 差 **1** 而非 **76**）——**合并重划块界**后，**与宽窗不相交的整块**里 **全局迭代路径上少掉的 key 数** 变成 **1** 量级（**不等同**于「物理块里只有 1 条 key」，而是 **剪枝跳过块后计数与块内键分布/顺序有关**；此处以 bench 观测为准）。**尖窗 vs 宽窗**：`1279212` vs `1279213`（**差 1 key**），**diag 尖窗 disjoint 块 = 2**、**宽窗 diag 曾为 1+0+0**（见上节）——**收窄窗多识别出 1 个不相交块**，与 **bench 再少 1 key** 对齐。 |
| **原因** | **Compaction 改变 data block 切分与 index 条**：同一查询窗下 **「整块不相交」的块集合会变**；故 **不能** 假设 compact 后 **宽窗 prune 仍少 76 key**。**`bytes_read` 与 `block_read_count` 在 prune/full 仍相同**（仍要打开相交块、块内全扫），与 **原件** 结论同类。 |
| **下一步** | 若要复现「尖窗明显少于宽窗」的 **IO**：需 **块内过滤** 或 **更细块界 + 与尖窗不相交的块显著增多**；可试 **`--target-file-mb 16`** 再对照 **diag + bench**（仍用副本）。 |

## 2026-04-05 — 读路径：`LevelIterator` 文件级 ST 剪枝（`experimental_st_prune_scan`）

| 项 | 内容 |
|----|------|
| **验证目的** | 落实 **`docs/sst_and_manifest_plan.md` §4**：在 **不打开 SST** 的前提下，用 **Manifest / `FileMetaData::st_file_meta`** 与查询窗 **不相交** 则 **跳过该文件**，以降低 **`OpenTable` / index 读**，服务「**后两个预期**」中的 **IO / 少打开文件**。 |
| **实现** | **`rocksdb/db/version_set.cc`**：`StFileDisjointFromExperimentalPruneScan`（与块级 **轴对齐不相交** 同式）；**`LevelIterator::SeekToFirst`** 与 **`SkipEmptyFileForward`** 前进下一文件时用 **`NextForwardFileIndexSkippingStPrune`**。**`Seek` / `SeekForPrev` / `SeekToLast` 不跳过**（与 `ReadOptions` 注释一致）。**`num_range_deletions > 0` 的文件永不跳过**。**`include/rocksdb/options.h`** 已补充说明。 |
| **是否符合期望** | **`verify_traj_st_full` / compact 副本**：两库 **`st_meta_sst_diag` 均为各文件 `file vs query window: INTERSECTS`** → **文件级剪枝不会触发**，**`block_read_count` 预期不变**；**效果需在「存在与窗不相交的 `st_file_bounds`」的多文件库**上观测（或合成小库：一文件仅含远离窗的数据）。 |
| **下一步** | **重编** `rocksdb` + `rocks-demo` 后跑 **`st_meta_read_bench`**；构造 **1 个 SST 与查询窗 DISJOINT、另 1 个 INTERSECTS** 的库，对比 **`block_read_count` / `INDEX_*`**；可选：**块内 key 过滤** 以进一步降 **`bytes_read`**。 |

## 2026-04-05 — 重编后：`st_meta_read_bench`（`verify_traj_st_full`，仅宽窗 prune）

| 项 | 内容 |
|----|------|
| **验证目的** | 合并 **`LevelIterator` 文件级 ST 剪枝** 并重编 **rocksdb + rocks-demo** 后，在 **2×SST 原件** 上确认 **宽窗 prune** 是否与改前一致。 |
| **验证结果** | 命令：`st_meta_read_bench --db verify_traj_st_full --no-full-scan`，宽窗同默认。**`prune_scan keys=1279138`**，**`block_read_count=5`**，**`bytes_read=78704054`**，`wall_us≈387690`，**`IOStatsContext read_nanos` 较高**（偏 **冷 OS 缓存** 首跑）。Statistics：**`BLOCK_CACHE_DATA_MISS=19193`**，**`BLOCK_CACHE_INDEX_MISS=3`**，**`BLOCK_CACHE_INDEX_HIT=1`**。 |
| **是否符合期望** | **是**：与改前 **`Record.md`** 中 **宽窗 `prune_scan keys=1279138`、`block_read_count=5`** **一致**——两 SST **`st_file_bounds` 与窗均相交**，**文件级剪枝不生效** 属预期；**块级**仍少 **76** key（1279214−1279138）。 |
| **原因** | 见上节「文件级仅 DISJOINT 时跳过」；本库 **无 DISJOINT 文件**，故 **数字不变** 说明 **未引入回归**。 |
| **还需要什么吗** | **不必**为确认文件级逻辑再跑本条。若要 **看到 `block_read` 因文件级下降**：需 **含与查询窗不相交的 `st_file_meta` 的 SST** 的库；或再跑一遍 **`--no-full-scan`**（**热缓存**）只看 **`read_nanos` / `wall_us` 趋势**。可选补跑 **`--no-full-scan` 尖窗** 与 **`full+prune` 一轮** 做 **`key_selectivity` / `block_read_ratio`**。 |

## 2026-04-05 — 读路径：块内 ST key 过滤（`experimental_st_prune_scan`）

| 项 | 内容 |
|----|------|
| **验证目的** | 在 **块 MBR 与查询窗仍相交** 的 data block 内，按 **user key 解码的 (t,x,y)** 与窗做 **与块/文件相同的轴对齐不相交判断**，**不对外暴露**不相交 key；进一步降低 **`st_meta_read_bench` 的 `prune_scan keys`**，并为后续压 **`bytes_read`/value 读** 铺路（当前仍遍历 block 内迭代步，但可少 **PrepareValue** 类工作）。 |
| **实现** | **`table/st_meta_user_key.{h,cc}`**：`UserKeySpatioTemporalDisjointFromPruneScan`。**`table/block_based/block_based_table_{iterator.h,cc}`**：`FindKeyForward()` 在 **`SeekToFirst` 后、连续 `Next()`** 下跳过不相交 key；**`st_prune_key_level_forward_active_`** 仅在 **`SeekImpl(nullptr)` 成功且 Valid** 后置位；**`Seek(key)` / `SeekForPrev` / `SeekToLast`** 清位（与 `ReadOptions` 注释「不对 `Seek(key)` 生效」一致）。**`allow_unprepared_value_` + `SeekToFirst`**：若开启 **`experimental_st_prune_scan`**，**不再**走「只持 index 首 key、延迟读块」路径，避免在 **Materialize** 前暴露窗外首 key。**`include/rocksdb/options.h`**：补充说明块内 key 行为。 |
| **构建** | **`st_meta_sst_diag` 属于 rocksdb 工程**，不在 rocks-demo。在 **x64 Native Tools** 或已配置好 MSVC 的环境中分两步：`cmake --build D:\Project\rocksdb\build --target rocksdb --config Release`（或加 `--target st_meta_sst_diag` 只编工具）；`cmake --build D:\Project\rocks-demo\build --target st_meta_read_bench --config Release`。**不要**在 `rocks-demo\build` 上对 **`st_meta_sst_diag`** 调用 cmake（会报 `unknown target`）。 |
| **复现实验命令** | **宽窗 prune（与改前对比 `prune_scan keys` 应下降）**：`D:\Project\rocks-demo\build\st_meta_read_bench.exe --db D:\Project\data\verify_traj_st_full --no-full-scan --prune-t-min 1224600000 --prune-t-max 1224800000 --prune-x-min 116.2 --prune-x-max 116.4 --prune-y-min 39.9 --prune-y-max 40.1`。**Manifest 演示（文件级 DISJOINT）**：`powershell -NoProfile -ExecutionPolicy Bypass -File D:\Project\tools\st_manifest_disjoint_demo.ps1` |
| **是否符合期望** | **已验证**（见下节「实测」）：**`prune_scan keys`** 由仅块级时的 **1279138** 降为 **3803**（宽窗）；**`BLOCK_CACHE_DATA_MISS=19193`** 与 **两 SST 全部 data block 仍读入** 一致；**`bytes_read` / `block_read_count`** 未因块内过滤而下降（整块 IO 不变）。**语义**：迭代计数 = **窗内 (t,x,y) 点**，非块 MBR 粗筛条数。 |
| **限制** | **`Prev` / `SeekToLast`** 路径 **未** 做对称块内过滤；**反向扫** 若需与正向一致可后续补 **`FindKeyBackward`**。 |

## 2026-04-06 — 块内 ST key 过滤：实测（`verify_traj_st_full`，Release）

| 项 | 内容 |
|----|------|
| **验证目的** | 重编 **rocksdb（含 `FindKeyForward` 块内过滤）** + **`st_meta_read_bench`** 后，对照 **改前** **`prune_scan keys=1279138`**，并跑 **full+prune**、**Manifest 演示窗**、**diag 宽/尖**。 |
| **宽窗仅 prune** | `--no-full-scan`，`t=[1224600000,1224800000]` `x=[116.2,116.4]` `y=[39.9,40.1]`。**`prune_scan keys=3803`**（原 **1279138**），**`bytes_read=78704054`**，**`block_read_count=5`**，**`wall_us≈421128`**，**`read_nanos≈359908900`**（偏 **冷 OS 缓存**）。**`BLOCK_CACHE_DATA_MISS=19193`**，**`INDEX_MISS=3` `HIT=1`**。 |
| **宽窗 full + prune** | 同窗。**`full_scan keys=1279214`**，`bytes_read=78704054`，`block_read_count=5`，`wall_us≈233590`。**`prune_scan keys=3803`**，`bytes_read=78704054`，`block_read_count=5`，**`wall_us≈69078`**。**`key_selectivity(prune/full)=0.002973`**，**`block_read_ratio=1`**，**`wall_time_ratio(prune/full)≈0.2957`**。**`BLOCK_CACHE_DATA_MISS=38387`**（**full+prune 两轮**叠加 ≈ **2×19193**）。**`INDEX_MISS=4` `HIT=2`**。 |
| **Manifest 演示窗（`st_manifest_disjoint_demo.ps1`）** | **`st_meta_sst_diag`**：`000009` **INTERSECTS**，块级 **skip 2763 / keep 10684**；`000011` **文件 DISJOINT**。**`st_meta_read_bench --no-full-scan`**：**`prune_scan keys=1`**，**`bytes_read=54975545`**，**`block_read_count=902`**，`wall_us≈48199`，**`BLOCK_CACHE_DATA_MISS=10684`**（≈ **keep 块数**）。 |
| **diag 宽/尖（`st_diag_align_read_bench.ps1`）** | 与 **块级-only 时代** 一致：**宽/尖** 均为 **`000009` skip 1 keep 13446**；**`000011` skip 0 keep 5747**；两文件 **file 均 INTERSECTS**（块级统计 **不** 反映块内 key 过滤）。 |
| **是否符合期望** | **是**。**宽窗**：块级曾 **少 76 key**；块内过滤后 **窗内真实点仅 3803**，**`key_selectivity≈0.3%`**。**IO**：**`block_read_ratio=1`**、**`bytes_read` 不变** 符合「仍读全部相交块」。**墙钟**：prune 路径 **wall 明显低于 full**（少 **127 万次** 对外可见迭代/值侧工作）。**东窗**：**文件级跳过 `000011`** + **块内过滤** → **`keys=1`**（**x∈[120,122]** 与数据分布下 **窗内点极少**；此前 **无 key 过滤** 时 bench 曾 **~71 万** 为 **块 MBR 相交块内全计数**）。 |
| **下一步** | 若需 **降 `bytes_read`**：需 **更细块界 / 块内早停 / 不整块解码** 等；**热缓存** 复跑看 **`read_nanos` 趋势**；**compact 副本** 上重复 **full+prune** 做 **多 SST** 对照。 |

## 2026-04-05 — Manifest 演示脚本与执行策略

| 项 | 内容 |
|----|------|
| **脚本** | **`tools/st_manifest_disjoint_demo.ps1`**：`x=[120,122]` 等窗使 **`000011` 文件级 DISJOINT**、**`000009` INTERSECTS**，并跑 **`st_meta_read_bench --no-full-scan`**。块内 key 过滤启用后，**`prune_scan keys` 可远小于** 仅块级时的 **~71 万**（见上节 **keys=1** 实测）。 |
| **PowerShell** | 若 **禁止脚本**：`powershell -NoProfile -ExecutionPolicy Bypass -File D:\Project\tools\st_manifest_disjoint_demo.ps1` |

## 2026-04-06 — 有效性扫窗（`st_validity_sweep.ps1`，`verify_traj_st_full`）

| 项 | 内容 |
|----|------|
| **实验目的** | **§1.0 / `docs/st_validity_experiment_design.md` E1**：在 **12 个不同查询窗** 上跑 **`st_meta_read_bench --no-full-scan`**，检验 **文件级 / 块级 / 块内 key** 与 **`bytes_read`、`keys`、`data_miss`** 是否随窗 **一致、可解释**。 |
| **实验结果** | 库 **`D:\Project\data\verify_traj_st_full`**（脚本默认解析为 `tools\..\data\...`）。**TSV 摘要**（列：`label` / `keys` / `block_read` / `wall_us` / `bytes_read` / `read_nanos` / `data_miss` / `index_miss`）：**wide_baseline** 3803, 5, 76537, 78704054, 23158300, 19193, 3；**sharp_spatiotemporal** 26, 5, 70305, 78704054, 20384500, 19193, 3；**east_x_file_disjoint** 1, 902, 44832, 54975545, 14585200, 10684, 3；**west_x_both_intersect** 2, 1736, 30622, 31697143, 10475500, 7524, 3；**tight_y_band** 111, 5, 63440, 78704054, 14959000, 19193, 3；**early/mid_t_slice** 各 keys=2, br=5, bytes=78704054, dm=19193；**late_t_slice** 4011, 7, 67413, 78724426, 17191400, 19181, 3；**south/north_y_shift** keys=2；**x_near_119_boundary** 2, 903, 45181, 54979627, 15620300, 10685, 3；**micro_box** 2, 11, 71843, 78765300, 20590500, 19150, 3。**error** 列为空（解析成功）。 |
| **是否达到预期** | **达到**（**西向 / 近 119°** 经 **下节 diag 复核** 已解释）。**东向 / 宽尖** 结论同前。 |
| **为什么** | **east** 见上。**wide/sharp**：块级对尖窗未多 skip，**块内 key** 拉大 **`keys` 差**。**west / x_near**：**文件级两 SST 均为 INTERSECTS**；**IO 下降来自块级 ST 剪枝**（见 **下节** **`would keep` 加总 = `BLOCK_CACHE_DATA_MISS`**）。 |
| **下一步** | **`verify_traj_st_compact_work`** 上 **E2** 双库 sweep；扫窗 **`-OutTsv`** 归档。 |

## 2026-04-06 — 复核：`west_x_both_intersect` 与 `x_near_119_boundary` 的 `st_meta_sst_diag`

| 项 | 内容 |
|----|------|
| **实验目的** | 解释 **`st_validity_sweep`** 中 **`west_x_both_intersect`**（`x∈[114,115.5]`）、**`x_near_119_boundary`**（`x∈[118.8,119.3]`）**`bytes_read` 低于宽窗** 的原因：区分 **文件级 DISJOINT** vs **块级大量 skip**。 |
| **实验结果** | **窗** `t=[1224600000,1224800000]` `y=[39.9,40.1]`。**west**：**`000009`** **file INTERSECTS**；块级 **skip 11670 / keep 1777**。**`000011`** **file INTERSECTS**；块级 **skip 0 / keep 5747**。**x_near**：**`000009`** **INTERSECTS**；**skip 2763 / keep 10684**。**`000011`** **INTERSECTS**；**skip 5746 / keep 1**。命令：`st_meta_sst_diag.exe --window 1224600000 1224800000 114 39.9 115.5 40.1` 与 `... 118.8 39.9 119.3 40.1`，两 SST 路径同 **`verify_traj_st_full`**。 |
| **是否达到预期** | **是**。与 bench 一致：**west** **`1777+5747=7524`** ≈ **`BLOCK_CACHE_DATA_MISS=7524`**；**x_near** **`10684+1=10685`** ≈ **`data_miss=10685`**。 |
| **为什么** | **两窗下文件级均为 INTERSECTS**，故 **非**「整 SST 跳过」。**west**：**`000009`** 内 **多数 data block 的块 MBR 与西向 x 窗不相交**（**skip 11670**），仅 **1777** 块保留；**`000011`** 仍 **5747** 块全保留 → **总读块远少于宽窗（≈19193）**，故 **`bytes_read≈32MB` 低于 78.7MB**。**x_near**：**`000011`** 仅 **1** 块与窗相交（**5746** 块 skip），**`000009`** **10684** 块保留 → **总 keep≈10685**，与 **东向单文件主导** 量级类似，**`bytes_read≈55MB`**。 |
| **下一步** | 论文表述：**西向 / 近 119°** 体现 **SST index 块级 ST 尾** 在 **文件仍打开** 时即可 **大幅减少 data block 读取**；与 **东向（文件级 DISJOINT）** 机制并列写清。 |

## 2026-04-06 — E2：双库扫窗（`verify_traj_st_full` + `verify_traj_st_compact_work`）

| 项 | 内容 |
|----|------|
| **实验目的** | **`docs/st_validity_experiment_design.md` E2**：同一 **12 窗 CSV**，在 **2×L0 原件** 与 **3×L6 compact 副本** 上各跑 **`st_meta_read_bench --no-full-scan`**，观察 **多 SST / 不同 compaction 布局** 下 **IO、`keys`、`block_read`** 是否仍符合设计叙事。 |
| **实验结果** | 命令：**`-RocksDbPathsCsv "...\verify_traj_st_full,...\verify_traj_st_compact_work"`**（**勿用** `-File` + `@(...)`），**`-OutTsv`**：`data/experiments/validity_sweep_two_dbs.tsv`。**full**：与此前单库扫窗一致（略）。**compact_work**（摘要）：**wide_baseline** `keys=3804`，`block_read=9`，`bytes_read=78737063`，`data_miss=19112`，`wall_us≈428500`（首行 **`read_nanos` 极大**，偏 **该库首次冷读**）；**east_x_file_disjoint** `keys=3`，`bytes_read=67269463`，`block_read=4618`，`data_miss=12234`；**west_x_both_intersect** `keys=3`，**`bytes_read=78737063`（与宽窗同量级）**，`block_read=9`，`data_miss≈19110`；**x_near_119_boundary** `keys=3`，`bytes_read=67277657`，`block_read=4616`，`data_miss=12238`；其余窗 **`keys` 多比 full 多 1**、`block_read=9`、**`bytes` 多为 78737063** 量级。 |
| **是否达到预期** | **部分达到**。**多 SST → `block_read` 由 5 增至 9**（打开更多表/index），**符合预期**。**东向 / 近 119°** 在 compact 上 **`bytes` 仍低于宽窗**（约 **67MB vs 78.7MB**），**文件级/块级组合仍省读盘**，**符合**。**西向窗** 在 compact 上 **`bytes` 回到满宽窗量级**，与 **2-SST 上西向 ≈32MB** 形成 **对比** —— **符合「布局改变则块级 skip 集合会变」**（见 **`Record.md`** compact 节）；需在论文中写 **依赖数据划分与 SST 边界**。 |
| **为什么** | **compact** 后 **三块 L6** 的 **块 MBR 与文件 bounds** 与 **L0 两文件** 不同：**west** 类窗在 **新布局下** 可能 **更多块与窗相交** → **少块级 skip**。**east** 仍可能 **跳过部分 SST 或大量块** → **67MB**。**首条 compact 行墙钟/`read_nanos` 大**：**同一进程内先跑完 full 再打开 compact**，**OS 页缓存对第二库仍偏冷** 或 **首次全量触盘**，**非逻辑错误**。 |
| **下一步** | **compact + west** 的 **`st_meta_sst_diag`** 见下节；**east + compact** 可选再跑一条 diag。 |

## 2026-04-06 — compact + `west_x`：`st_meta_sst_diag`（三 SST）

| 项 | 内容 |
|----|------|
| **实验目的** | 解释 **E2** 中 **`verify_traj_st_compact_work` + 西向窗** 的 **`bytes_read` 与宽窗同量级**：用 **diag** 核对 **块级 `would skip`/`would keep` 加总** 是否与 **`BLOCK_CACHE_DATA_MISS≈19110`** 一致。 |
| **实验结果** | **`--window 1224600000 1224800000 114 39.9 115.5 40.1`**，**`000040` / `000041` / `000042`**。三文件 **`file vs query window` 均为 INTERSECTS**。块级 **would skip**：**1+2+0=3**；**would keep**：**8247+8246+2617=19110**（与 bench **west 行 `data_miss≈19110`** **一致**）。 |
| **是否达到预期** | **是**。diag 与 **E2** 交叉验证 **闭环**。 |
| **为什么** | **Compaction 重划块** 后，西向窗下 **几乎每个 data block 的 MBR 仍与窗相交**（仅 **3** 块 **disjoint**），故 **读块量 ≈ 扫全库相交块**，**`bytes` ≈ 宽窗**；与 **2×L0** 下 **`000009` 单文件可 skip 上万块** 对比鲜明——**同一几何窗，收益强依赖布局**。 |
| **下一步** | 论文 **Discussion**：写明 **块级剪枝效果依赖块界与分布**；可选 **east + compact** 三 SST diag 对齐 **E2 东向行**。 |

## 2026-04-06 — PKDD（Porto）轨迹：`st_validity_experiment_design` E1 / E2 / E3 真实验证

| 项 | 内容 |
|----|------|
| **实验目的** | 按 **`docs/st_validity_experiment_design.md`**，在 **PKDD 加工子集**（`data/processed/pkdd/`，**10 万轨迹 / ~484 万点**）上复刻 **E1 单库扫窗、E2 双库（原件 + compact）、E3 diag 对照**，证明流程与 **Geolife** 解耦后仍成立。 |
| **数据与窗** | **`st_meta_smoke --st-keys`** 灌库 → **`data/verify_pkdd_st`**（**6 SST**，L0/L6 混合，与 flush 历史一致）。**窗表**：**`tools/st_validity_experiment_windows_pkdd.csv`**（12 窗；**`east_x_file_disjoint`** 取 **lon ∈ [-4.5,-3.5]**，与数据 **lon_max≈-6.96** 不相交）。**compact 副本**：**`copy_compact_traj_st.ps1 -SourceDb verify_pkdd_st -TargetDb verify_pkdd_compact_work`** → **9×L6 SST**。 |
| **实验结果（E1）** | **`-OutTsv`**：**`data/experiments/validity_sweep_pkdd_e1.tsv`**。**`error` 列全空**。**`wide_baseline`**：`keys=3466181`，`bytes_read≈263.6MB`，`data_miss=60653`。**`east_x_file_disjoint`**：**`keys=0`**，`bytes_read≈0.88MB`，`block_read=2`，`data_miss=0` —— **相对宽窗 IO 骤降**。**`sharp_spatiotemporal`**：`keys=82`。**`west_x_both_intersect`**：`keys=1`。**多样 t/y 窗**下 **`keys` 非单一常数**（如 **early 517001**、**late 354795**、**south 168** 等）。 |
| **实验结果（E2）** | **TSV**：**`data/experiments/validity_sweep_pkdd_e2_two_dbs.tsv`**。**compact** 上 **东向**：**`keys=0`，`bytes_read=0`，`block_read=0`**（**文件级全跳过**）。**宽窗**两库 **`keys` 同量级**（**3466181**），**`block_read`** 由 **1563→27**（打开表/index 次数变化），**`bytes_read`** compact **略高于** full（**~296MB vs ~263MB**，与 **多 SST / 缓存未命中** 有关）；**东向仍远小于宽窗**，**定性趋势与 Geolife E2 一致**。 |
| **实验结果（E3）** | **`st_meta_sst_diag`**，**宽窗** `1372637000 1374000000 -8.72 41.08 -8.52 41.22`：**5/6 文件 INTERSECTS**；**`000023.sst` 整文件 DISJOINT**（与宽窗 **t 上界 1374000000** 及该文件 **t 下界** 组合一致）。**东向窗** `... -4.5 41.0 -3.5 41.25`：**6/6 文件 `file vs query window: DISJOINT`**，与 **bench `keys=0`** **无矛盾**。 |
| **是否达到预期** | **是**（有效性标准）：**无崩溃/解析失败**；**东向相对宽窗可重复 IO 下降**；**多样窗 `keys` 有分布**；**diag 与 bench 一致**。 |
| **为什么** | PKDD 与 Geolife **共用同一套 ST user key + 剪枝语义**；仅 **窗的几何与 t 范围** 按 **Porto 包络** 重标定。**东向窗** 刻意落在 **数据 lon 包络以东**，触发 **Manifest 文件级跳过** 与 **极低 `bytes_read`**。 |
| **下一步** | 若需论文并列：**`docs/st_validity_visualization.md` 式汇总表**可增 **PKDD 一节**；**`copy_compact_traj_st.ps1` 尾部 diag** 仍写死 **Geolife 宽窗**，对 PKDD 库会误报全 DISJOINT —— **应以本记录中的 `--window` 为准** 或后续给脚本加 **`-DiagWindow*` 参数**。 |

## 2026-04-06 — Phase A：与「无剪枝全表扫」硬对比（`full_scan` vs `prune_scan`）

| 项 | 内容 |
|----|------|
| **实验目的** | 在 **同一 fork、同一库、同一窗** 下，用 **`st_meta_read_bench`** 先跑 **`ReadOptions` 无 `experimental_st_prune_scan`** 的 **全 CF 迭代**，再跑 **剪枝迭代**，量化 **`bytes_read` / `wall_us`** 相对 **「不启用时空剪枝的标准扫表」** 的倍数（**非**上游未改 RocksDB 直接打开 ST-SST）。 |
| **实现** | **`st_meta_read_bench`** 增加 **`--no-prune-scan`**（仅全表）；默认仍为 **full+prune**。**`tools/st_prune_vs_full_baseline_sweep.ps1`** 对窗表逐行调用 bench（**不带** `--no-full-scan`），解析 **`IOStatsContext delta`**，写出 **`data/experiments/prune_vs_full_pkdd.tsv`**。设计见 **`docs/st_rocksdb_hard_baseline_experiment.md`**。 |
| **实验结果（PKDD `verify_pkdd_st`，12 窗）** | **`full_bytes_read` 每窗约 **297–298MB**（与窗无关，扫全库）**。**`ratio_bytes`（prune/full）** 摘录：**`east_x_file_disjoint` ≈ **0.003**（**~0.3% 读盘**）**；**`west` ≈ **0.006**；**`north_y_shift` ≈ **0.019**；**`x_near_eastern_boundary` ≈ **0.015**；**`wide_baseline` ≈ **0.886**；**`late_t_slice` ≈ **0.122**。**`key_selectivity`** 与 **`ratio_bytes`** 在 **强选择性窗** 上同向。**`block_read_ratio` 可 >1**（剪枝路径 index 打开次数多），**以 `bytes_read` 为主叙事**（见设计文档脚注）。 |
| **是否达到预期** | **是**：**东向 / 稀疏窗** 上 **相对「无剪枝全表」读盘下降两个数量级以上**；**宽窗** 上 **仍有 ~11–12% 的 `bytes` 节省**（本次机器 **~264/298MB**），证明 **不只「避开空窗」叙事**，而是 **对「有命中的大窗」仍可少读部分块**。 |
| **为什么** | **全表扫** 必须沿 **user key 序** 覆盖 **整 CF** 的表/index，**与查询窗无关**；**剪枝** 在 **文件/块/键** 上跳过 **与窗不相交** 的存储单元，故 **`bytes_read` 可远小于全表**。 |
| **下一步** | **Phase B**（可选）：**无 ST 尾巴** 的第二库 + **vanilla RocksDB** 打开，与本文 **Phase A** 并列写论文；**Geolife** 库若重建可跑同一脚本换 **`-WindowsCsv`**。重建 **`st_meta_read_bench`** 需本机 **正常 MSVC**（当前部分环境 `cl` 路径异常时编译失败，不影响已存在 exe 跑 sweep）。 |

## 2026-04-06 — E5：随机查询窗 + Phase A 分布（PKDD smoke）

| 项 | 内容 |
|----|------|
| **实验目的** | 削弱 **手选 12 窗** 可能 **利好剪枝** 的质疑：在 **PKDD 子集全局包络** 内 **均匀随机** 轴对齐窗，再跑 **`st_prune_vs_full_baseline_sweep.ps1`**，用 **`summarize_prune_vs_full_tsv.py`** 报告 **`ratio_bytes` 分位数**。 |
| **实现** | **`tools/generate_random_query_windows.py`**（**`--seed` / `--include-anchor wide_pkdd`**）、**`tools/run_random_prune_vs_full_pkdd.ps1`** 一键；设计见 **`docs/st_validity_experiment_design.md` §E5**。 |
| **实验结果（smoke：n=6 随机 + 1 anchor，seed=7）** | 窗 CSV：**`data/experiments/random_windows_pkdd_n6_s7.csv`**；TSV：**`data/experiments/prune_vs_full_pkdd_random_n6_s7.tsv`**。**`summarize`**：**random_w* 6 条** **`ratio_bytes` p50≈0.0059，p90≈0.028，max≈0.030**（本批随机窗多 **极小 / 弱相交**）；**`wide_baseline_anchor`≈0.886** 与手设计 **wide** 一致。 |
| **是否达到预期** | **部分**：流程跑通；**小样本随机窗** 易 **偏空/偏稀**，**p50 极低不具代表性** —— 正式应用 **`RandomCount 50–200`** 并报告 **全样本 + random_w 子集** 分位；可选 **分层随机**（固定窗面积分桶）避免 **过小窗主导分布**。 |
| **为什么** | 均匀随机 **时空盒子** 与 **真实查询分布** 仍不同，但比 **固定 12 标签** 更难 **刻意挑东向 DISJOINT**；**与 Phase B（vanilla）正交**。 |

## 2026-04-04 — PKDD 大规模子集（300k 轨迹 / ~14.29M 键）+ Phase A（`bytes` + `wall_us`）

| 项 | 内容 |
|----|------|
| **实验目的** | 在 **大于默认 10 万轨迹子集** 的 PKDD 数据上，复现 **Phase A**：**无剪枝全 CF 迭代（`full_scan`）** vs **`experimental_st_prune_scan`（`prune_scan`）**，对比 **`IOStatsContext bytes_read`** 与 **墙钟 `wall_us`**，支撑论文 **§1.1** 规模叙事。 |
| **数据** | **`process_pkdd_train.py --max-segments 300000 --out data/processed/pkdd_large`** → **300000** 轨迹、**14288269** 点；包络 **lon [-12.15,-6.09] lat [38.56,51.04] t [1372636853,1378656304]**。灌库：**`st_meta_smoke --db data/verify_pkdd_st_large ... --st-keys --max-points -1`**（**~316s** 本机一次）。 |
| **设计窗（12）** | **TSV**：**`data/experiments/prune_vs_full_pkdd_large_design_windows.tsv`**（**`st_validity_experiment_windows_pkdd.csv`**）。**摘录**：**`full_wall_us`≈2.71–2.83s/窗**（热缓存下近似常数）；**`east_x_file_disjoint`**：**prune `wall_us`≈23.6µs**，**`ratio_wall`≈0**，**`ratio_bytes`≈0**；**`wide_baseline`**：**`ratio_wall`≈0.39**，**`ratio_bytes`≈0.53**；**`sharp_spatiotemporal`**：**`ratio_wall`≈0.13**，**`ratio_bytes`≈0.28**。整表 sweep **~44s**（12 窗 × 每窗 full+prune）。 |
| **随机窗（25 + anchor，seed=43）** | **窗 CSV**：**`data/experiments/random_windows_pkdd_large_n25_s43.csv`**（**`generate_random_query_windows.py --envelope pkdd_large_300k`**）。**TSV**：**`data/experiments/prune_vs_full_pkdd_large_random_n25_s43.tsv`**。**`summarize_prune_vs_full_tsv.py`**：**random_w* `ratio_wall` p50≈0.0009，p90≈0.141**；**`ratio_bytes` p50≈0.0019，p90≈0.318**；**锚点 `wide_baseline_anchor`**：**`ratio_wall`≈0.38**，**`ratio_bytes`≈0.53**。 |
| **工具变更** | **`summarize_prune_vs_full_tsv.py`** 增 **`ratio_wall` 分位数**；**`generate_random_query_windows.py`** 增 **`--envelope pkdd_100k | pkdd_large_300k`**。 |
| **是否达到预期** | **是**：大数据集上 **东向 / 稀窗** 仍 **墙钟与读字节大幅下降**；**宽窗** 上 **prune 墙钟约为 full 的 ~0.38–0.39×**（本批协议）。 |
| **为什么** | 与 **§1.0 PKDD** 同机制；键数 **~3×**（相对 ~484 万点库）→ **全表 `bytes_read`≈877MB**、**`full_wall`≈2.7–2.8s**（热 OS 缓存）。**`ratio_*` 趋势与设计窗几何一致**。 |
| **下一步** | 论文中写清 **热缓存、单进程多窗** 协议；若审稿要求：**冷启动分进程**、**多 seed**、或 **`--all-segments` 全量**（~83M 点，磁盘与时间成本显著更高）。 |

## 2026-04-07 — 严格消融：SST / Manifest / SST+Manifest（PKDD 14.29M）

| 项 | 内容 |
|----|------|
| **实验目的** | 在同一数据集（PKDD 14.29M keys）+ 同一 12 窗口 workload 上，做**严格消融**：只启用 SST 剪枝、只启用 Manifest（文件级）剪枝、以及两者叠加；用 **RocksDB 真实 wall_us 查询时间**计算 **吞吐（QPS）**，并对比基线 `full_scan`。 |
| **数据集与库** | 300k 轨迹子集：`data/processed/pkdd_large`；RocksDB：`data/verify_pkdd_st_large`。数据库中 `.sst` 总数：**14**（`Get-ChildItem -Filter '*.sst' -Recurse -File`）。 |
| **窗口集** | `tools/st_validity_experiment_windows_pkdd.csv`（12 窗）。 |
| **严格消融定义（代码级）** | 在 fork 内把 `experimental_st_prune_scan` 拆为三开关：`file_level_enable`、`block_level_enable`、`key_level_enable`。并在 `st_meta_read_bench` 增加 `--prune-mode`：<br/>- `sst`：`file=false, block=true, key=true`<br/>- `manifest`：`file=true, block=false, key=false`<br/>- `sst_manifest`：三者全开（默认；对应原行为）。 |
| **命令** | `st_prune_vs_full_baseline_sweep.ps1` 传入 `-PruneMode`；如：<br/>`-PruneMode sst_manifest` → `prune_vs_full_pkdd_large_ablation_sst_manifest.tsv`<br/>`-PruneMode sst` → `prune_vs_full_pkdd_large_ablation_sst.tsv`<br/>`-PruneMode manifest` → `prune_vs_full_pkdd_large_ablation_manifest.tsv` |
| **关键结果：吞吐与壁钟加速（12 窗）** | 总结脚本：`tools/summarize_prune_modules_throughput.py`。<br/>**SST+Manifest**：avg full `3074688.3us`、avg prune `337446.9us`；sum full 36.896s、sum prune 4.049s；吞吐 full `0.325 QPS`、prune `2.963 QPS`；speedup `9.112x`；`ratio_wall` p50 `0.1274`（p90 `0.2190`）。<br/>**SST only**：speedup `8.089x`（prune QPS `3.042`，`ratio_wall` p50 `0.1251`）。<br/>**Manifest only**：speedup `1.180x`（`ratio_wall` p50 `0.9836`），说明在该 12 窗几何下主要收益来自 SST 侧（块级/块内键过滤），文件级离散跳过并不占主导。 |
| **是否达到预期** | **是（就消融叙事而言）**：SST-only 与 SST+Manifest 的 wall-time 启发式收益显著，Manifest-only 接近 full（除少数文件级 DISJOINT 场景），与我们预期的“关键剪枝路径在 SST 侧”一致。 |
| **下一步（建议）** | 把这份消融结果补进你刚写的对外说明中，并增加一页图（从上述 TSV 生成或沿用现有浏览器条形图风格）。如果要更强说服力，再对比 `manifest` only 在那些出现明显下降的窗上做 `st_meta_sst_diag --window` 验证其 DISJOINT 文件数量。 |

## 2026-04-07 — 消融重跑：baseline = `full_scan --full-scan-mode window`（与 prune 同窗计数）

| 项 | 内容 |
|----|------|
| **实验目的** | 在 **`st_meta_read_bench` 新 baseline**（全 CF 迭代，但 **`full_keys` = 窗内 ST 点数**；`full_keys_scanned_total`≈14.29M）下，重跑 **SST / Manifest / SST+Manifest** 三份严格消融，便于与 Phase A 叙事一致。 |
| **命令** | `tools/run_pkdd_large_ablation_window_baseline.ps1`（内部三次调用 `st_prune_vs_full_baseline_sweep.ps1`，**`-FullScanMode window`**）。 |
| **产出 TSV** | `data/experiments/prune_vs_full_pkdd_large_ablation_sst_manifest_window.tsv`、`..._sst_window.tsv`、`..._manifest_window.tsv`。 |
| **summarize_prune_modules_throughput.py** | **SST+Manifest**：speedup **8.258×**，prune QPS **2.869**，full QPS **0.347**；`ratio_wall` p50 **0.1353**。<br/>**SST only**：speedup **7.185×**，prune QPS **2.546**。<br/>**Manifest only**：speedup **1.052×**，prune QPS **0.387**（与旧批一致：该 12 窗下文件级剪枝不占主导）。 |
| **说明** | **`manifest`** 模式下块/键级剪枝关闭，**`prune_scan keys` 常为整库条数**（与 **`full_keys` 窗内计数**不可比）；**`key_selectivity` 列会失真**，解读以 **`ratio_wall` / `ratio_bytes` / QPS** 为主。 |
| **是否达到预期** | **是**：与旧消融（`all_cf` baseline）相比，**SST / SST+Manifest 的加速比同量级**；baseline 与 Phase A 对齐。 |

## 2026-04-07 — 消融对拍正确性：key+value（12/12）

| 项 | 内容 |
|----|------|
| **实验目的** | 在同一数据集与同一 12 窗口 workload 下，验证 **full pass** 与 **prune pass** 返回的**窗内 `(user_key,value)` 多重集合**一致（严谨 key+value 对拍）。 |
| **命令** | `tools/run_pkdd_large_ablation_window_baseline.ps1 -VerifyKVResults`（内部调用 `st_prune_vs_full_baseline_sweep.ps1 --verify-kv-results`，对每窗 full 与 prune 做哈希对比）。 |
| **产出 TSV** | `prune_vs_full_pkdd_large_ablation_*_window_verifykv.tsv`（SST+Manifest / SST-only / Manifest-only 三份）。 |
| **验证结果** | 三种模块在 **12/12** 个窗口上均为 `kv_correct=OK`，且 `kv_full_inwindow_kv` 与 `kv_prune_inwindow_kv` 完全一致。 |
| **是否达到预期** | **是**：无剪枝与三种消融模式下，查询语义（窗内点集合的 key+value）保持一致。 |

## 2026-04-08 — 无锡数据全量处理 + 段对象最小接入（不灌库）

| 项 | 内容 |
|----|------|
| **实验目的** | 将 `data/无锡轨迹数据集`（日 zip）按车辆 `id` 处理成可用于后续模型接入的段级数据；并打通“段对象 ST key”的最小读写链路。 |
| **处理与代码** | 新增 `tools/process_wuxi_trajectory.py`：清洗 `\\t` 脏符、解析时间、按 `id` + 时间差（`gap_seconds=300`）切段，输出 `segments_points.csv` / `segments_meta.csv`。新增段 key 编码 `rocks-demo/st_segment_key.hpp`（`0xE6 + t_start/t_end + MBR + id`）。`rocksdb/table/st_meta_user_key.cc` 扩展支持 `0xE6` 的段级判交与聚合；`st_meta_smoke` 新增 `--segment-meta-csv --st-segment-keys` 灌段模式。 |
| **全量处理结果** | `process_wuxi_trajectory.py --in-dir data/无锡轨迹数据集 --out data/processed/wuxi --gap-seconds 300`：`processed_zip_files=31`，`rows_in=39705419`，`rows_out=39705419`，`rows_bad=0`，`vehicles=695`，`segments=81768`，extent `lon[76.7792450,133.1606633] lat[12.1525216,45.0662583] unix[1595030400,1597679999]`。产物：`data/processed/wuxi/segments_points.csv`（约 4.17GB）与 `segments_meta.csv`。 |
| **段对象链路 smoke** | `st_meta_smoke --segment-meta-csv data/processed/wuxi_smoke/segments_meta.csv --st-segment-keys --max-points 200` → `Wrote 98 rows ...`、`Verified 98 Get round-trips ... OK`。`st_meta_read_bench` 在 `verify_wuxi_segment_smoke` 上可运行并得到 `full_keys=34`、`prune_keys=34`（示例窗）。 |
| **是否达到预期** | **是（阶段一）**：数据已处理完，段对象编码与读路径 smoke 可用；尚未进入全量灌库/性能实验阶段。 |

## 2026-04-08 — 第二阶段：无锡全量段对象建库 + smoke 查询

| 项 | 内容 |
|----|------|
| **实验目的** | 用 `data/processed/wuxi/segments_meta.csv` 建立全量段对象库，验证段对象路径在全量数据上稳定（先 smoke，不做大规模实验）。 |
| **建库命令与结果** | `st_meta_smoke --db data/verify_wuxi_segment_full --segment-meta-csv data/processed/wuxi/segments_meta.csv --st-segment-keys --max-points -1`；结果：`Wrote 81768 rows`，`Verified 81768 Get round-trips ... OK`。 |
| **smoke 查询 #1（相交窗）** | `st_meta_read_bench ... t=[1596200000,1596203600] x=[120,120.6] y=[31.4,31.95] --verify-kv-results`：`full_keys=91`，`prune_keys=92`，`verify_kv_results correctness=OK full_inwindow_kv=91 prune_inwindow_kv=91`，`ratio_wall≈0.2610`。 |
| **smoke 查询 #2（不相交窗）** | `st_meta_read_bench ... x=[140,141] y=[50,51] --verify-kv-results`：`full_keys=0`，`prune_keys=0`，`prune_bytes_read=0`，`prune_block_read_count=0`，`verify_kv_results correctness=OK`，`ratio_wall≈0.004075`。 |
| **是否达到预期** | **是（阶段二 smoke）**：全量段对象 DB 可用，查询路径稳定，且 full/prune 的窗内 key+value 对拍一致。 |

## 2026-04-08 — 无锡段对象严格消融（吞吐 + 速率 + 准确性）

| 项 | 内容 |
|----|------|
| **实验目的** | 在无锡段对象全量库上，按 12 窗口做严格消融（`sst_manifest` / `sst` / `manifest`），同时保留吞吐（QPS）、速率（speedup）与 key+value 对拍准确性。 |
| **命令** | `run_pkdd_large_ablation_window_baseline.ps1 -RocksDbPath data/verify_wuxi_segment_full -OutDir data/experiments/wuxi_segment -WindowsCsv tools/st_validity_experiment_windows_wuxi.csv -VerifyKVResults`。 |
| **输出** | `data/experiments/wuxi_segment/prune_vs_full_pkdd_large_ablation_*_window_verifykv.tsv`（三份）。 |
| **吞吐汇总** | `summarize_prune_modules_throughput.py`：<br/>**SST+Manifest**：full QPS `55.939`，prune QPS `179.697`，speedup `3.212x`，`ratio_wall` p50 `0.2952`（p90 `0.5866`）。<br/>**SST-only**：full QPS `56.081`，prune QPS `173.193`，speedup `3.088x`。<br/>**Manifest-only**：full QPS `56.296`，prune QPS `59.008`，speedup `1.048x`。 |
| **准确性汇总** | 三份 TSV 的 `kv_correct` 均为 **12/12 OK**（每窗 full vs prune 的窗内 `(user_key,value)` 集合一致）。 |
| **图表** | `docs/st_ablation_module_throughput_chart_wuxi.html`（真实吞吐柱图 + 速率 + 12/12 准确性）。 |
| **是否达到预期** | **是**：SST 侧仍是主要收益来源；Manifest-only 在该窗口集上收益有限；准确性对拍通过。 |

## 2026-04-11 — 无锡三库三模式消融重跑 + HTML 柱图更新

| 项 | 内容 |
|----|------|
| **实验目的** | 重跑 `1sst + 164sst + 736sst`（776 目录缺失故回退 736）× `manifest/sst/sst_manifest`，用 cov 12 窗刷新 `docs/st_ablation_wuxi_1sst_vs_manysst.html` 数据，替代旧跑次中可能过时或易误读的倍率。 |
| **命令** | `powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_wuxi_segment_ablation_1_164_776.ps1 -OutDir data/experiments/wuxi_ablation_html_refresh_20260415 -SummarizePooled` |
| **产出** | `ablation_{manifest,sst,sst_manifest}.tsv`、`pooled_p50_summary.txt`、`wuxi_ablation_run_meta.json`（`used_736sst_fallback=true`）。 |
| **按库 sum_full/sum_prune（full_keys≥50，n=12）** | **1 SST**：Global **0.989×**，Local **2.752×**，L+G **2.967×**（Global&lt;1 表示该窗集合计 wall 上 manifest 未优于 full 相对 prune 的对比语义，属正常可观察现象）。**164**：4.459× / 21.431× / 26.781×。**736**：177.725× / 25.821× / 267.435×。 |
| **是否达到预期** | **是**：跑次完整、汇总与柱图已对齐新 TSV。 |
| **下一步** | 若对外报告需强调「无加速」侧：单独讨论 1-SST 上 Global；可选 `-VerifyKVResults` 与多次重复。 |

## 2026-04-11 — 无锡三库三模式消融 10 次重复（verify_kv + 均值进 HTML）

| 项 | 内容 |
|----|------|
| **实验目的** | 在相同 cov 设定下重复 10 次消融，对 speedup / QPS 取算术平均；全量开启 KV 对拍以确认准确性；更新柱图与图下汇总表。 |
| **命令** | `powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_wuxi_segment_ablation_repeat_n.ps1 -N 10 -BaseOutDir data/experiments/wuxi_ablation_10r_verifykv_20260411`（默认 `-VerifyKVResults`）。 |
| **聚合** | `python tools/aggregate_wuxi_ablation_runs.py --parent data/experiments/wuxi_ablation_10r_verifykv_20260411 --min-full-keys 50 --json data/experiments/wuxi_ablation_10r_verifykv_20260411/aggregate.json` |
| **HTML** | `python tools/refresh_wuxi_ablation_chart_html.py data/experiments/wuxi_ablation_10r_verifykv_20260411/aggregate.json docs/st_ablation_wuxi_1sst_vs_manysst.html` |
| **均值 speedup（full_keys≥50）** | **Global**：1 SST **1.054×**，164 **4.440×**，736 **159.86×**。**Local**：**2.896×** / **20.173×** / **27.347×**。**L+G**：**2.846×** / **25.828×** / **317.54×**。 |
| **准确性** | 三模式 × 三库 × 12 窗：10 跑次均 **verify_kv 全 OK**（见各 run 下 TSV 的 `kv_correct`）。 |
| **是否达到预期** | **是**：重复、聚合与文档页（柱图 + 表）已对齐。 |

## 2026-04-11 — 单跑 L+G（sst_manifest）+ 与原生 RocksDB 差异备忘

| 项 | 内容 |
|----|------|
| **目的** | 单独跑一轮 Local+Global，确认路径可用；整理本 fork 相对 **上游 RocksDB** 的增量部件（与效率无关的说明向汇总）。 |
| **命令** | `st_prune_vs_full_baseline_sweep.ps1`，`-PruneMode sst_manifest`，`-VerifyKVResults`，`-RocksDbPaths data/verify_wuxi_segment_1sst`，`-WindowsCsv tools/st_validity_experiment_windows_wuxi_stratified12_n4m4w4.csv`（正式默认；历史跑次曾为 `random12_cov_s42`），与无锡主脚本一致的 VM/736 桶/adaptive gate 参数。 |
| **产出** | `data/experiments/wuxi_lg_only_single_run/ablation_sst_manifest.tsv`（12 窗）、`run_meta.json`；**kv_correct 12/12 OK**。 |
| **与 vanilla 对照** | 见当次对话中的分层汇总（manifest 文件元数据 + LevelIterator 跳过 + 可选桶内 R-tree；SST 内块/索引与 user key 级 ST 判定；ReadOptions 子开关；段式 key 与建库工具链）；**full scan** 指 fork 内关剪枝的同一二进制，**不等于**未改 schema 的上游库上的同一 workload。 |

## 2026-04-12 — 上游 Vanilla 壁钟缓存（1/164/736 三档 replica）+ 10×消融聚合 + HTML

| 项 | 内容 |
|----|------|
| **实验目的** | 按 `EXPERIMENTS_AND_SCRIPTS.md` §0.2 / `docs/VANILLA_ROCKSDB_BASELINE.md`：在 **原生态 RocksDB** 可读目录上测 **同窗查询壁钟** `vanilla_wall_us`；三档 SST 布局分别对应 **fork dump → `st_vanilla_kv_stream_ingest`** 的 `*_vanilla_replica`；每 **(库×窗)** 重复 **10 次**取 **中位数** 写入缓存；再跑 **10 次**独立消融，`aggregate_wuxi_ablation_runs.py --baseline vanilla` 与柱图对齐。 |
| **工程修复** | `st_segment_window_scan_vanilla`：上游迭代器在部分情况下已返回 **user key**（如 25 字节段键），误用「剥 8 字节 internal footer」会破坏 E6 编码导致 `keys=0`；新增 **`UserKeyFromIteratorKey`** 纠偏。 |
| **Replica 与缓存** | `tools/build_wuxi_vanilla_replica_dbs.ps1` → `verify_wuxi_segment_{1sst,164sst,736sst}_vanilla_replica`（776 缺失故 fork 侧与 736 对齐）。`cache_wuxi_vanilla_wall_baseline.ps1`（`-RepeatCount 10`，cov 12 窗）→ **`data/experiments/wuxi_vanilla_wall_cache.json`**（**36** 条：3×12）。 |
| **批量消融** | `tools/run_wuxi_vanilla_cached_ablation_batch.ps1`（已改：缓存步用 **replica** 路径，消融步仍用 **fork** 三库；`-AblationRuns 10`，`-SkipBuildVanillaCache` 可跳过重建缓存）。 |
| **产出目录** | `data/experiments/wuxi_ablation_vanilla_baseline_20260412_120508/`（`run_01`…`run_10`，各含 `ablation_*.tsv`、`wuxi_ablation_run_meta.json`）、**`aggregate.json`**。 |
| **HTML** | `docs/st_ablation_wuxi_1sst_vs_manysst.html`（已由 `refresh_wuxi_ablation_chart_html.py` 更新；说明段含缓存路径与父目录）。 |
| **注意** | **`VanillaAsBaseline` 时 `-VerifyKVResults` 不跑 fork full**，聚合表「准确性」为未做 full↔prune KV 对拍；剪枝自洽需另跑不带 Vanilla 基线的 sweep。 |
| **是否达到预期** | **是**：Vanilla 查询开销已持久化进 JSON + 多 run TSV + 聚合与图；与 **§0.1a** 点级真值无关时仍以 MBR 口径理解 `full_keys`。 |

## 2026-04-12 — 文件级 R-tree skip-ratio 门控 A/B（无锡 w01 + bucket3600，`iterator_repeat=50`）

| 项 | 内容 |
|----|------|
| **验证目的** | 验证 `ReadOptions::experimental_spatio_temporal_prune_scan` 中 **`file_level_rtree_skip_ratio_gate_enable`** / **`file_level_rtree_min_skip_ratio`**：当 **`disjoint/eligible`** 低于阈值时跳过 per-level bucket+BVH、回退线性文件级 disjoint；在 **无锡多 SST 段库** 上与关门控对比 **壁钟与 IO**。 |
| **库与窗** | DB：**`D:\Project\data\verify_wuxi_segment_bucket3600_sst`**。窗：**`wx_strat_narrow_01`**（`tools/st_validity_experiment_windows_wuxi_stratified12_n4m4w4.csv`）：`t=[1596200000,1596203600]`，`x=[120.02,120.34]`，`y=[31.52,31.88]`。 |
| **命令** | `--prune-mode manifest --time-bucket-count 736 --rtree-leaf-size 8 --iterator-repeat 50`；**A** 无 gate；**B** 加 **`--file-level-rtree-skip-ratio-gate --file-level-rtree-min-skip-ratio 0.2`**。 |
| **验证结果** | **`keys(last_iter)=752`**、**`keys_in_window=60`**、**`block_read_count=600`**、**`bytes_read=221249950`** 两组一致。**`wall_us_per_iter_avg`**：**关门控 3642.78µs**，**开门控 3037.45µs**（约 **−16.6%**）；累计 **`read_nanos`**：**70037400 vs 58895400**。**`prune_file_skipped=730`**，**`time_disjoint=730`**（736 文件中 730 个时间维与窗不相交）。 |
| **是否符合期望** | **是**：键与 IO 规模不变；门控无退化；该高选择性窗下平均壁钟更低。 |
| **原因** | 文件级剪枝已能跳过绝大多数 SST；门控避免在低收益场景维持 BVH 构建路径的开销，50 次重复后趋势稳定。 |
| **团队约定** | **`EXPERIMENTS_AND_SCRIPTS.md` §0.5**：正式 ST 读路径测试 **默认无锡 stratified 窗**；**`st_meta_read_bench` 默认 `--prune-*`** = **`wx_strat_narrow_01`**（**Geolife 窗不再作为项目默认**）。 |

## 2026-04-12 — 正式 12 窗切换为 stratified12 + 工具链默认 + Vanilla 缓存须重建

| 项 | 内容 |
|----|------|
| **实验目的** | 将 **`tools/st_validity_experiment_windows_wuxi_stratified12_n4m4w4.csv`**（4 窄 + 4 中 + 4 宽）定为 **唯一正式默认 12 窗**；消融与 Vanilla 壁钟缓存与 **`st_meta_read_bench` 默认单窗**（`wx_strat_narrow_01`）全部对齐。 |
| **代码与脚本** | **`run_wuxi_segment_ablation_1_164_776.ps1`**、**`cache_wuxi_vanilla_wall_baseline.ps1`**、**`run_wuxi_vanilla_cached_ablation_batch.ps1`**、**`run_wuxi_random12_vm_p50_ablation.ps1`**、**`run_wuxi_global_vm_auto_vs_vanilla.ps1`**：未传 **`-WindowsCsv`** 时 **优先 stratified12**。**`st_meta_read_bench.cpp`** 默认 `--prune-*` = narrow_01。文档：**`EXPERIMENTS_AND_SCRIPTS.md`**（§0.3 / §0.5 / §2.2）、**`AGENT_HANDOFF`**、**`REPRODUCTION`**、**`VANILLA_ROCKSDB_BASELINE`**、**`BUILD_AND_EXPERIMENTS`** 已同步。 |
| **Vanilla** | **更换窗 CSV 后必须重跑** **`cache_wuxi_vanilla_wall_baseline.ps1`**（对 `*_vanilla_replica` 路径，与消融 replica 对齐），再跑 **`run_wuxi_segment_ablation_1_164_776.ps1 -VerifyKVResults`**（默认即 Vanilla 基线，除非 **`-AllowForkFullBaseline`**）。 |
| **消融重跑命令（单次 + 汇总）** | ① **`cache_wuxi_vanilla_wall_baseline.ps1`** 的 **`-RocksDbPathsCsv`** 须指向 **`*_vanilla_replica`**（三档与 fork 对齐）。② **`run_wuxi_segment_ablation_1_164_776.ps1 -SummarizePooled`**；**`-VerifyKVResults`** 在 **`VanillaAsBaseline` 时不会跑 fork full↔prune 对拍**（脚本会告警），剪枝自洽需另跑 **不带 Vanilla 基线** 的 sweep。 |
| **本机已执行（2026-04-12）** | 缓存：**`wuxi_vanilla_wall_cache.json`** 重建 **36** 条（stratified12 × 3 replica，`RepeatCount=3`）。消融目录：**`data/experiments/wuxi_ablation_stratified12_vanilla_20260412`**（`wuxi_ablation_run_meta.json` 中 **`windows_csv`=stratified12**、**`compare_baseline`=vanilla**、**736 回退**）。`summarize_wuxi_ablation_three_modes.py --pooled-by-db` 已写入同目录 **`pooled_p50_summary.txt`**（含 Vanilla/prune 比）。 |
| **是否达到预期** | **是**：正式窗与工具链默认已切换；Vanilla 缓存与三模式消融已用新 12 窗跑通。 |

## 2026-04-13 — `st_meta_compact_existing`（164 段库）开启新 Compaction 参数后的首轮结果

| 项 | 内容 |
|----|------|
| **实验目的** | 在 `verify_wuxi_segment_164sst` 上验证新接入的 Compaction 路径（事件时间切分 + 空间软/硬切分 + read-repair-first 调度入口）是否能稳定执行，并观察输出 SST 形态是否从 L0 小文件收敛到更规则的 L6 文件。 |
| **实验结果** | 运行日志摘要：`L0 /000009.sst num_entries=266`；`KEY_PROBE sample=4096 e6=4096 e5=0 other=0 e6_t_first=1597374720 e6_t_last=1596264205`；`CompactRange` 完成后 **`AFTER live_sst=8`**，均在 **L6**：`004034..004041.sst`，`num_entries` 分别为 `11689, 11331, 11561, 11338, 11048, 11217, 11223, 2361`。 |
| **是否达到预期** | **部分达到**：Compaction 成功且输出文件形态明显变化（L0→L6，多文件），说明参数链路可执行；但当前仅看到文件数与条目分布，尚未完成读路径收益验证。 |
| **为什么** | 该结果证明新参数至少未阻断 compaction 主路径；L6 输出按较均匀条目分布拆分，符合“分桶/切分会打散单一大文件”的方向。是否真正减少读放大仍取决于这些新 SST 的 `st_file_bounds` 与目标查询窗相交关系。 |
| **下一步** | 1) 先跑 `st_meta_sst_diag` 对 `004034..004041.sst` 做 **file-level 与 block-level** 诊断；2) 再跑 `st_meta_read_bench`（同窗）对比 compaction 前后 `block_read_count/bytes_read/wall_us`；3) 若要复现，保留本次命令参数组并追加到 `EXPERIMENTS_AND_SCRIPTS.md` 的 Compaction 实验段。 |

## 2026-04-13 — 验证：Compaction 后 `verify_wuxi_segment_164sst`（默认无锡窄窗）

| 项 | 内容 |
|----|------|
| **实验目的** | 按 `AGENT_HANDOFF.md` / `EXPERIMENTS_AND_SCRIPTS.md` 约定，用默认无锡窗（`wx_strat_narrow_01`）对刚完成 compaction 的 `verify_wuxi_segment_164sst` 做 **read_bench + sst_diag**，验证是否出现可解释的剪枝收益。 |
| **实验结果** | **`st_meta_read_bench --db D:\Project\data\verify_wuxi_segment_164sst`**：`full_scan keys=54 keys_scanned_total=81768 block_read_count=24 wall_us=229829 bytes_read=486217361`；`prune_scan keys(last_iter)=62 keys_in_window=54 block_read_count=11960 wall_us_total=93975.5 bytes_read=145492160`；`wall_time_ratio=0.4089`。文件级诊断：`prune_file_skipped=0`，`considered=8`，`time_disjoint=0`。块级诊断：`index_examined=45036`，`index_skipped_st_disjoint=31897`，`Block_Skip_Rate=70.8256%`。 |
| **SST diag（同窗）** | 对 `004034..004041.sst` 逐文件诊断：8/8 文件均 `file vs query window: INTERSECTS`，`with ST tail = index entries`（无缺失 tail）。各文件 `would skip`/`would keep` 分别为：`4540/1890`、`4465/1811`、`4507/1866`、`4467/1806`、`4367/1743`、`4332/1824`、`4304/1821`、`915/378`。合计约 `skip=31897`、`keep=13139`，与 bench 块级计数一致。 |
| **是否达到预期** | **部分达到**：块级剪枝效果明显（~70.8%），并且 wall/bytes 相对 full 明显下降；但文件级跳过为 0（当前窗对 8 个文件全部相交）。 |
| **为什么** | 这次收益主要来自 **SST 内 block-level ST 剪枝**，不是文件级。`st_file_bounds` 仍覆盖默认窗，因此 `file_skipped=0`；但块级 MBR 能跳过大量不相交数据块，故 `bytes_read` 与 `wall_us` 下降。 |
| **下一步** | 1) 增加一个更“东向/时间窄”的窗，验证是否触发 `file_skipped>0`；2) 在同库跑 `--prune-mode manifest` / `sst` / `sst_manifest` 三模式，对比当前 compaction 后 Global 与 Local 的贡献占比。 |

## 2026-04-15 — 点级 Verify 与 Compaction 策略适配首轮对照（`natural_compact_off` vs `natural_compact_on`）

| 项 | 内容 |
|----|------|
| **实验目的** | 在开始 Manifest 分片实验前，先验证“新点级 `-VerifyKVResults`”与现有 Compaction 策略（`natural_compact_off/on`）是否兼容，作为后续改造基线。 |
| **实验命令** | 1) `st_meta_read_bench --db D:\Project\data\verify_wuxi_segment_natural_compact_off --prune-mode sst_manifest --verify-kv-results`；2) `st_meta_read_bench --db D:\Project\data\verify_wuxi_segment_natural_compact_on --prune-mode sst_manifest --verify-kv-results`（默认窗 `wx_strat_narrow_01`）。 |
| **实验结果（compact_off）** | `verify_kv_results correctness=OK`，`full_inwindow_kv=49`，`prune_inwindow_kv=49`，`full_decode_failed=0`，`prune_decode_failed=0`，`full_legacy_segment_value=0`，`prune_legacy_segment_value=0`。`prune_file_skipped=14/15`，`block_index_skip=3231/3283`。 |
| **实验结果（compact_on）** | `verify_kv_results correctness=OK`，`full_inwindow_kv=49`，`prune_inwindow_kv=49`，`full_decode_failed=0`，`prune_decode_failed=0`，`full_legacy_segment_value=0`，`prune_legacy_segment_value=0`。`prune_file_skipped=71/74`，`block_index_skip=1818/1870`。 |
| **是否达到预期** | **是**：两种 compaction 形态均通过点级真值 Verify，说明当前段 value（V2）与点级校验链路可稳定工作。 |
| **为什么** | 新 Verify 走“`0xE6` value 点列 `∃点∈W`”判定；两库均无 decode/legacy 异常，表明 compaction 后数据布局变化未破坏点级语义一致性。 |
| **下一步** | 1) 以这组结果作为 Manifest 分片实验基线；2) 分片实现中把“Compaction 输出边界 + `st_file_meta` 一致性”设为验收门槛；3) 补跑 `manifest` 单模式与更窄时间窗，观察 Global 路径对分片收益的敏感度。 |

## 2026-04-15 — Manifest 分片 Phase 1（兼容读取层）首个真实库 smoke

| 项 | 内容 |
|----|------|
| **实验目的** | 验证“兼容读取层”是否生效：`CURRENT` 指向 `MANIFEST_SET-*` 时，DB 仍可正常恢复打开（保持对单 MANIFEST 向后兼容）。 |
| **实验方法** | 从 `verify_wuxi_segment_1sst` 复制副本到 `data/experiments/manifest_set_smoke_db`；新增 `MANIFEST_SET-000001`（内容：`version=1` + `MANIFEST-016277`）；将副本 `CURRENT` 改为 `MANIFEST_SET-000001`。 |
| **实验命令** | `st_meta_read_bench --db D:\Project\data\experiments\manifest_set_smoke_db --no-prune-scan --full-scan-mode window`。 |
| **实验结果** | 初次运行报 `CURRENT file corrupted`（原因：`st_meta_read_bench` 仍链接旧 `rocksdb.lib`）；重编 `st_meta_read_bench` 后复跑成功：`full_scan keys=7339`，`keys_scanned_total=81768`，`block_read_count=3`，`wall_us=57108.8`。 |
| **是否达到预期** | **是（Phase 1 smoke）**：兼容读取链路已在真实库副本上工作，`CURRENT -> MANIFEST_SET-*` 可被识别并回放。 |
| **为什么** | 新增的 `GetCurrentManifestPaths` 支持两种 `CURRENT` 语义（单 MANIFEST / MANIFEST_SET），`VersionSet::Recover` 按 manifest 列表顺序回放，保持旧格式兼容。 |
| **下一步** | 1) 补充 `ListColumnFamilies` 的 manifest-set 回归验证；2) 开始 Phase 2：写路径路由（先不改默认行为，增加开关与兜底分片）；3) 加入与 Compaction 输出边界一致性的联动校验。 |

## 2026-04-15 — Manifest 分片路由触发多 descriptor（`manifest_set_route_smoke_force4`）

| 项 | 内容 |
|----|------|
| **实验目的** | 验证 Phase 2 的写路径分片路由是否可以在同一轮 batch 中产出“分片 descriptor + global 回退 descriptor”，并确认 `MANIFEST_SET-*` 能同时列出多个 MANIFEST。 |
| **实验命令** | `findstr /C:"experimental_manifest_shard_route enabled=1" D:\Project\data\experiments\manifest_set_route_smoke_force4\LOG`；`type D:\Project\data\experiments\manifest_set_route_smoke_force4\MANIFEST_SET-*.*`。 |
| **实验结果** | 日志出现：`batch_edit_shards=[1,0] reasons=[ok,new_files_empty]`（同文件中前一条为 `[0]`）；`MANIFEST_SET-000010` 内容含两条 descriptor：`MANIFEST-000010` 与 `MANIFEST-000011`。 |
| **是否达到预期** | **是**：已成功触发多 MANIFEST 路由，且 CURRENT 指向的 manifest-set 可承载并消费多 descriptor。 |
| **为什么** | 第一个 edit 命中 shard 1（`ok`）；第二个 edit 无 `new_files` 走 global 回退（`new_files_empty`）；最终 manifest-set 同时记录 shard/global 两条 MANIFEST，验证了“分片 + 回退”并存路径。 |
| **下一步** | 1) 去除 `FORCE/FORCE_NEW_DESCRIPTOR` 实验强制依赖，收敛到真实 compaction 触发；2) 将路由启用条件固定在 `experimental_compaction_event_time_split` 场景，保留 `new_files_empty -> global` 回退；3) 补跑自然触发回归（wall/bytes + recover 一致性）。 |

## 2026-04-15 — 去 FORCE 后自然触发验证（`manifest_set_route_smoke_noforce*`）

| 项 | 内容 |
|----|------|
| **实验目的** | 在移除 `FORCE/FORCE_NEW_DESCRIPTOR` 代码依赖后，直接验证“仅靠真实 `experimental_compaction_event_time_split`”是否仍可触发 Manifest 分片路由与多 descriptor `MANIFEST_SET-*`。 |
| **实验命令** | 1) 重编：`cmake --build D:\Project\rocks-demo\build --target st_meta_compact_existing --config Release`。2) 无 FORCE 环境：`ROCKSDB_EXPERIMENTAL_MANIFEST_SET_WRITE=1`、`ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE=1`、`ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC=3600`、`ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_COUNT=8`，并显式清空 `ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_FORCE*`。3) 运行：`st_meta_compact_existing --db ...manifest_set_route_smoke_noforce --st-time-split --st-bucket-sec 3600 --st-min-keys 1`；`st_meta_smoke --db ...manifest_set_route_smoke_noforce_fresh --csv ...segments_points.csv --st-keys --max-points 2000 --st-compaction-time-split --st-compaction-bucket-sec 3600 --compact-after-ingest`。 |
| **实验结果** | `manifest_set_route_smoke_noforce`：成功 compact，但 `LOG` 无 `experimental_manifest_shard_route enabled=1` 路由命中行。`manifest_set_route_smoke_noforce_fresh`：出现 `batch_edit_shards=[0] reasons=[new_files_empty]`；`CURRENT=MANIFEST_SET-000005`，且 `MANIFEST_SET-000005` 仅包含 `MANIFEST-000005`（单 descriptor）。 |
| **是否达到预期** | **部分达到**：验证了“无 FORCE 可正常运行并保持兼容”，但本轮未自然触发 `batch_edit_shards=[1,0]` 这类多 descriptor 路由。 |
| **为什么** | 去掉 `FORCE_NEW_DESCRIPTOR` 后，分片路由受 `new_descriptor_log` 与批次内容约束更严格。本轮观测到的批次主要是 `new_files_empty`（global 回退），未形成“同批次内包含可路由 new_files edit + global 回退 edit”的组合。 |
| **下一步** | 1) 在自然 workload 中制造“同一轮 LogAndApply 含可路由 new_files edit + 一个 metadata/edit 空变更”的批次；2) 增加轻量日志（`new_descriptor_log`、每个 `VersionEdit` 的 `new_files/deleted_files` 计数）定位为何落到 `[0]`；3) 补一次 recover/read_bench 回归，确认单 descriptor `MANIFEST_SET` 与旧 `CURRENT->MANIFEST` 均稳定。 |

## 2026-04-15 — `MANIFEST_SET` 协议升级（`generation + entry(commit_seq,manifest)`）smoke

| 项 | 内容 |
|----|------|
| **实验目的** | 按 manifest sharding 计划的 Phase 3 基础步，验证 `MANIFEST_SET-*` 新协议（`version=2` + `generation` + `entry=commit_seq,MANIFEST-*`）可正确写出，并可被恢复链路消费。 |
| **实验命令** | 1) 建库：`ROCKSDB_EXPERIMENTAL_MANIFEST_SET_WRITE=1` + `st_meta_smoke --db D:\Project\data\experiments\manifest_set_proto_v2_smoke --csv D:\Project\data\processed\segments_points.csv --st-keys --max-points 2000 --compact-after-ingest`。2) 验证文件：读取 `CURRENT` 与 `MANIFEST_SET-000005`。3) 回放验证：重编 `st_meta_read_bench` 后运行 `st_meta_read_bench --db ...manifest_set_proto_v2_smoke --no-prune-scan --full-scan-mode window`。 |
| **实验结果** | `CURRENT=MANIFEST_SET-000005`；`MANIFEST_SET-000005` 内容为：`version=2`、`generation=5`、`entry=1,MANIFEST-000005`。`st_meta_read_bench` 重编后可成功打开并扫描：`keys_scanned_total=2000`，`block_read_count=3`，`wall_us=1373.3`。 |
| **是否达到预期** | **是**：新协议已落盘并可被恢复链路正确解析；旧流程（bench 打开 + 扫描）未回归失败。 |
| **为什么** | `manifest_ops` 已兼容解析 `entry=` 与 `generation=`；`version_set` 写 `CURRENT` 时调用 `SetCurrentFileToManifestSet` 并传入 `commit_seq` 列表与 generation，因此写读两端协议一致。 |
| **下一步** | 1) 继续推进 Phase 3：在多分片场景补齐 `commit_seq` 合并回放约束（尤其跨分片原子组）；2) 增加“旧 `version=1` / 新 `version=2` 混合”恢复回归；3) 在自然触发多 descriptor 场景验证 `entry` 顺序与 replay 顺序一致。 |

## 2026-04-15 — 多 entry `MANIFEST_SET`（v2）回放验证 + 回放顺序接入

| 项 | 内容 |
|----|------|
| **实验目的** | 验证 `MANIFEST_SET v2` 在多 entry 场景可被恢复链路消费；并推进 Recover 侧按 `commit_seq` 稳定排序回放（而非仅文件行序）。 |
| **实验命令** | 在 `manifest_set_route_smoke_force4` 上临时写入 `MANIFEST_SET-999997`：`version=2`、`generation=999997`、`entry=20,MANIFEST-000010`、`entry=10,MANIFEST-000011`，将 `CURRENT` 临时切换后执行 `st_meta_read_bench --db ... --no-prune-scan --full-scan-mode window`，结束后恢复原 `CURRENT`。 |
| **实验结果** | bench 可成功打开并扫描：`keys_scanned_total=1000`，`block_read_count=2`，`wall_us=373`；`LOG` 出现 `Recovering from manifest set: count=2`，说明多 entry v2 结构可被恢复链路读取。 |
| **是否达到预期** | **是（功能层面）**：多 entry v2 manifest-set 可回放打开；兼容路径稳定。 |
| **为什么** | `manifest_ops` 已支持 `entry=commit_seq,MANIFEST-*` 解析；Recover 侧已接入 `commit_seq` 序列并在多清单回放前执行稳定排序。 |
| **下一步** | 1) 在“真实自然触发多 descriptor”场景确认 `entry` 顺序与提交顺序一致；2) 补 `experimental_manifest_sharding_enable` 灰度开关的端到端验证（默认写 `version=1`，开启后写 `version=2 + generation/entry`）。 |

## 2026-04-15 — `generation + commit_seq` 状态机闭环（递增与重启恢复）

| 项 | 内容 |
|----|------|
| **实验目的** | 在开启 `ROCKSDB_EXPERIMENTAL_MANIFEST_SHARDING_ENABLE=1` 后，验证 `MANIFEST_SET v2` 的 `entry(commit_seq, MANIFEST)` 在多次写入中持续单调递增，并在重启打开后保持可恢复。 |
| **实验命令** | 目标库：`D:\Project\data\experiments\manifest_set_proto_v2_seq`。1) 先前状态确认：`CURRENT -> MANIFEST_SET-000017`，内容 `entry=4,MANIFEST-000017`。2) 再跑一轮 `st_meta_compact_existing --st-time-split --st-bucket-sec 3600 --st-min-keys 1`。3) 读取 `CURRENT` 与对应 `MANIFEST_SET-*`。4) 运行 `st_meta_read_bench --no-prune-scan --full-scan-mode window` 做 reopen 验证。 |
| **实验结果** | 新一轮后 `CURRENT -> MANIFEST_SET-002062`；`MANIFEST_SET-002062` 内容为 `version=2`、`generation=5`、`entry=5,MANIFEST-002062`。`st_meta_read_bench` 可成功打开并扫描：`keys_scanned_total=3000`（`block_read_count=4074`，大量小 SST 形态下属预期）。 |
| **是否达到预期** | **是**：`commit_seq` 从 `4 -> 5` 继续单调递增，且 DB 重启读取链路正常。 |
| **为什么** | `VersionSet` 已维护 `manifest_set_next_commit_seq_` 与 `manifest_set_generation_`，写 `MANIFEST_SET` 时按状态机分配全局序号，Recover 时再由文件元数据回填下一序号，形成“写入递增 + 恢复续写”闭环。 |
| **下一步** | 1) 进一步跑“自然触发多 descriptor（同一 MANIFEST_SET 含多条 entry）”并核对回放排序；2) 增加崩溃点回归（分片已写但 CURRENT 未切换 / CURRENT 已切换）完成 Phase 5 故障用例。 |

## 2026-04-15 — 灰度开关与故障语义回归（Phase 5）

| 项 | 内容 |
|----|------|
| **实验目的** | 补齐上线前两类关键回归：1) `experimental_manifest_sharding_enable` 开关语义（默认关/显式开）；2) 崩溃语义 A/B（分片文件已写但 `CURRENT` 未切换；`CURRENT` 已切换）。 |
| **实验命令** | **开关语义**：建两库 `manifest_set_gate_off_smoke` / `manifest_set_gate_on_smoke`（均 `ROCKSDB_EXPERIMENTAL_MANIFEST_SET_WRITE=1`，仅后者额外 `ROCKSDB_EXPERIMENTAL_MANIFEST_SHARDING_ENABLE=1`），读取 `MANIFEST_SET-000005`。**故障语义**：在干净库 `manifest_set_crash_semantics_base` 上手工写 `MANIFEST_SET-123450/123451`；A) 保持 `CURRENT` 不变直接 reopen；B) 将 `CURRENT` 切到新 set 后 reopen。 |
| **实验结果** | **开关语义**：`gate_off` 写出 `version=1 + MANIFEST-*`；`gate_on` 写出 `version=2 + generation + entry=`。**故障语义 A**（`CURRENT` 不切）：reopen 成功（`CASE_A_EXIT=0`）。**故障语义 B**（`CURRENT` 已切）：当 `entry` 指向不存在 descriptor 会失败（预期保护）；改为真实 descriptor（`MANIFEST-000017`）后 reopen 成功（`CASE_B_REAL_DESCRIPTOR_EXIT=0`）。 |
| **是否达到预期** | **是**：灰度开关生效、兼容路径正常；故障语义与设计一致（可见性以 `CURRENT` 原子切换为准，且切换后必须引用存在的 descriptor）。 |
| **为什么** | 读路径始终由 `CURRENT` 决定回放入口：A 场景仍读旧 set，因此稳定；B 场景切换后读取新 set，若 entry 无效则立即 fail-fast，entry 有效则可恢复打开。 |
| **下一步** | 1) 继续追“自然触发同一 `MANIFEST_SET` 多 entry”（当前多发生单 entry）；2) 增加自动化崩溃注入（Sync 后/`CURRENT` Rename 前后）脚本化回归，减少手工构造。 |

