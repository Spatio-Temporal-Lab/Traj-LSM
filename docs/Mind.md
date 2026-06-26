# Mind.md — 历史设计定稿（已过时，仅供参考）

> ⚠️ 本文件已过时。当前设计终稿见 **[DESIGN.md](DESIGN.md)**。
> 
> 过时要点：
> - 单 Manifest 方案 → 现为 M_manager + 多 MANIFEST-b
> - m_manager 不持有空间信息 → 现 M_manager 持有 MBR 粗筛
> - 跨桶无复制 → 现仍为多 MANIFEST-b per-bucket 分片
> 
> 保留此文件仅为历史参考和部分 Local Index / Compaction 细节。

本文件是当前版本的“最终口径”，用于对齐代码实现与实验设计。任何与本文件冲突的旧思路（例如：**仅起始桶存 value**、Master/Ghost 跨桶引用、**跨日历桶完整 value 复制**、每桶一类 SST、或仅用 Seek(q_start) 导致漏召回）都视为过时，应删除/弃用。

**架构一览（当前）**：

```text
Ingest → 每 segment_uid 一条 E7 KV（canonical bucket_id + 完整 value）
              → 统一 memtable/EOF flush；ingest **完成时 L0=0**，数据在 **L1+**（通常 1×L1 SST；K≈N）
              ↓
ST-compaction（L1+ 整层归并）→ 层间整理 / 块 MBR / 文件 meta；删旧 file / 写新 file
              ↓ 同一 Version 边界（完成条件，不单指写完 .sst）
         Manifest 与 SST 同步：单 MANIFEST + 每 SST 的 st_file_meta [t_min,t_max]（一次 replay / IO）
              ↓
Query → m_manager（仅选出 MANIFEST 文件）→ MANIFEST → 按 SSTable `st_file_meta` 时间窗 **AND** 空间 MBR 粗筛 → Local（查询窗→bucket_id 粗定位 → index block **value** 时空信息硬判定）→ KV → 点真判
```

**召回策略（定稿，2026-06）**：**m_manager 仅选出 MANIFEST 文件** + **Manifest 按 SSTable 级 `st_file_meta` 时间窗 AND 空间 MBR 粗筛**；**禁止**依赖跨桶完整复制保证不漏解。**不再**按日历桶切 MANIFEST-b / M_manager 分片（旧方案见 Global Index「历史 / 可选扩展」）。不漏解的充分条件见专节 **「不漏解充分条件（无复制）」**。

**根因口径（实现优先级）**：漏召回的主因是 **Manifest / Version 未与 SST 真实变化对齐**（live file 遗漏、`st_file_meta` 窗过窄），而非「查询桶内没有该段的物理副本」。详见 **「ST-compaction 完成条件：Manifest 与 SST 同步」**。

---

数据集（当前）：`data/generated_trajs_segments*.jsonl`（一行一个轨迹段，value 为完整 points）。

**逻辑段唯一 id（去重键，定稿）**：`segment_uid = (vehicle_id, segment_seq, chunk_seq)`。查询合并时 **不含 `bucket_id`**。定稿 ingest **每 uid 仅一条 KV**；`query_result_dedup_count` 正常应 **≈0**（仅幂等重试 / 历史库迁移时兜底）。

查询语义（必须保证）：
- 输入：时间窗口 \([q_start, q_end]\) + 空间窗口（MBR）+（可选）vehicle_id。
- 输出：**相交即返回整段轨迹段（完整 value）**。
- 允许索引阶段 false positive，不允许 false negative（**100% 召回**）。
- **不**依赖跨桶复制；召回由 **单 Manifest → SST `st_file_meta` 时间窗 → Local 块 meta + MBR + 点真判** 保证（见下节）。

---

## 存储语义（单 key + 日历桶软索引，无跨桶复制）

**基本规则（定稿）**：
- 每个 **`segment_uid` 恰好一条** E7 KV，`value` 为 **完整 points**（含跨日历桶长段的全程 `[t_start, t_end]`）。
- **`bucket_id`（key 内）** = **canonical 日历桶** `b* = floor(t_start / W)`；**不**为相交的其它日历桶再写副本。
- **时间桶 `W`**：仍用于 E7 **`bucket_id` 粗排序**、compaction 块断流；**不是**「每桶一份数据」的硬分区；**不再**用于切 Manifest 分片。
- Ingest：**统一 memtable / EOF flush** → ingest **结束时 L0 文件数必须为 0**；物理条数 **K≈N** 在 **L1+**（通常 1×L1 SST）。**禁止** ingest 完成后仍留大量 L0 SST（如 ~146 个）。

**跨桶长段（时间跨度覆盖多个日历桶）**：
- 盘上 **仍只有一条 KV**；段的全段时间在 **Value 头** `t_start/t_end` 与 **st_file_meta / 块 tail** 中体现。
- 查询落在「中间日历桶」时：**不靠** 该桶内有一条副本，而靠 **Manifest 中 SST `F` 的 `st_file_meta [t_min,t_max]` 与查询窗相交** 进入候选（**一次** Manifest replay，无需按桶扇出登记）。

**Key 与唯一性（写入侧）**：
- E7 **`bucket_id` = b***（canonical）；`t_start_offset` 相对 `b*×W`。
- **禁止**同一 `segment_uid` 重复 insert（ingest 幂等）。
- **禁止**跨相交桶 **完整 value 复制**（`--legacy-ghost-ingest` / 旧 replicate 路径仅历史库兼容，新库不用）。

**与 Global / Local 的关系**：
- **Global（m_manager + Manifest）**：`m_manager` **仅负责选出 MANIFEST 文件**（不做时空过滤，**不持有空间信息**）；`manifest` 再按 **SSTable 级时间窗 AND 空间 MBR** 做粗筛，时间相交 **且** 空间相交才进入候选 SSTable；每个 live SST `F` 仍 **AddFile 一次**，`st_file_meta` 记录 **F 内全段** `[t_min,t_max]` + **全局 MBR**（来自 Value 头并集，**非** key 的 `bucket_id` 推短窗）。
- **Local（SST 内）**：查询在已打开 SST 内，先将查询窗口转换为 `bucket_id`，用最小 `bucket_id` 做 `Seek` 定位起始 block；然后**向后**遍历 index block，检查每个 block entry 的 **value 中全局时空信息**（硬性条件）；`bucket_id` 仅做粗粒度终止判定——当 index block key 的 `bucket_id` < 查询窗口最小 `bucket_id` 时，**之后**的 block 一定不包含查询窗口（因为 key 有序单增），可以安全停止；但**起始位置不能仅靠 `bucket_id` 判断**，因为可能存在跨多个 `bucket_id` 的长轨迹段。
- ST-compaction 变更 file 后，同 Version 边界 **Manifest VersionEdit 与 `st_file_meta` 对齐**；**false_negative_count 必须恒为 0**。

---

## 不漏解充分条件（无复制，定稿）

对查询窗 `W_q` 与段 `s`，**不漏解** ⟺ 下列 **AND**（与是否跨桶复制 **无关**）：

| 跳 | 合同 | 说明 |
|----|------|------|
| **G1 Manifest** | 单 MANIFEST replay 得到 **全部 live SST**；对每个 `F`，`st_file_meta.[t_min,t_max]` 与 `W_q` 时间相交 **AND** MBR 空间相交 ⇒ `F` 进入候选；meta 缺失 → **直接报错** | **一次** IO / replay；**禁止**按日历桶切分导致漏 file |
| **L1 meta** | `st_file_meta` / 块 tail 的 `t_min/t_max`、MBR 来自 **Value 头全段** `[t_start,t_end]`，非仅用 key 的 `bucket_id` 推短窗 | 跨桶长段不误剪 |
| **L2 保守** | meta 缺失 / value 解码失败 → **直接报错**（架构高度依赖元信息完整） | 禁止偏窄假边界；不降级为 match-all |
| **点真** | MBR 相交后解压 value，存在一点 ∈ `W_q` 才命中 | 控 FP，与 FN 无关 |

**旧误区（废弃）**：「段跨桶 → 必须在每个相交桶各写一条 KV，MANIFEST-b 才有数据。」  
**正解**：「段在 SST `F` 上只有一条 key；**Manifest 登记 `F` 一次**，`st_file_meta` 覆盖 **全段** `[t_min,t_max]`，查询用 file 时间窗召回。」

**历史误区（废弃）**：「每个日历桶一个 MANIFEST-b + M_manager 扇出 S(F) 才能不漏解。」  
**正解（定稿）**：**单 Manifest + per-SST 时间 meta** 即可；S(F) / M_manager 仅为旧实现或 **超大规模可选扩展**。

**验收**：`false_negative_count=0`；全窗 `finalSize` 与 manifest truth 一致；`query_result_dedup_count≈0`。

---

## 跨桶长段、查询与 Uid-Merge（定稿）

本节描述 **无复制 ingest**、**单 Manifest 查询召回**、**Manifest 对齐**；历史 **跨桶复制 + Uid-Merge 收敛** 见下节 **Uid-Merge（历史 / 迁移）**。

### canonical `bucket_id`（定稿，勿与「每桶一 SST」混用）

| 概念 | 定稿 |
|------|------|
| **`bucket_id` in E7 key** | **canonical** `b* = floor(t_start/W)`；每 `segment_uid` **仅一条** key |
| **日历桶 `W`** | E7 **`bucket_id` 排序**、块断流；**不是** Manifest 分片维度；查询 **不** 要求 `Seek bucket_id = 查询桶` |
| **每桶一类 SST / per-bucket flush** | **废弃**（unified EOF flush） |
| **`FlushOptions.st_manifest_host_bucket`** | **废弃为主路由**（单 Manifest 下无分片登记）；历史库兼容只读 |
| **SST 时间 meta** | `st_file_meta.[t_min,t_max]` = F 内所有 segment Value 头 **全段并集** |

### 问题陈述（无复制）

| 现象 | 说明 |
|------|------|
| 盘上条数 | **K≈N**；每 `segment_uid` 一条 KV |
| 跨桶长段 | **一条** key + 全 value；召回靠 **SST `st_file_meta` 时间窗 + Local 块 meta** |
| RocksDB 同 key 合并 | 仅 **完全相同 user key**；ingest 幂等防重复 |
| 写放大 | ingest **不**接受跨桶复制；compaction 主要整理层与 meta |

**聚簇语义（勿混淆）**：ST-compaction 按 **时空** 选输入 / 切文件 / 块 MBR；**不按 `segment_uid` 聚簇**。`segment_uid` 用于 **逻辑标识**、异常去重、（历史库）Uid-Merge 清理。

### 查询路径（定稿：m_manager + Manifest + file meta）

- **禁止**「查询日历桶 B ⇒ 只 Seek `bucket_id=B` ⇒ 只开一个 SST」作为唯一路径。
- **定稿链**：**m_manager（仅选出 MANIFEST）→ Manifest replay → 按 `st_file_meta` 时间窗 AND 空间 MBR 粗筛 SST → Local（查询窗→bucket_id Seek 起始 + index block value 时空信息硬判定）→ KV → 点真判**。
- `m_manager` 与 Manifest 不同步，或 `st_file_meta` **窗过窄** → **漏解**（与段是否跨桶无关）。

### Uid-Merge（历史 / 迁移；新库非主路径）

**历史**：跨桶完整复制时，compaction 用 Uid-Merge 将 **K→N**。  
**定稿（新库）**：ingest **无复制** → **不需要** Uid-Merge 作收敛手段；实现可保留 **ingest 幂等 / 历史 DB 清理**（同 uid 重复 key 时丢条）。

| 项 | 定稿 |
|----|------|
| **新库 ingest** | 每 uid 一条；**禁用**跨桶 replicate |
| **Uid-Merge 启用** | 可选；仅 **重复 uid**（重试、旧库迁移） |
| **canonical 保留** | 若启用 Merge，仍优先 **b*** 那条 key |
| **job 级 `seen`** | 若仍启用 Merge，**须** CompactionJob 级（非 per-subcompaction） |

### ST-compaction 归并流（实现口径，非 uid 聚簇）

一场 ST 整层 compaction（输入该层 **全部 SST**）在代码上近似为：

1. **输入范围（整层泄洪）**：Lx 触发时 **该层 Version 内全部 SST** 进 job；若 Lx+1 存在 **键域 overlap** 的旧文件，一并作为 **第二层输入**（经典 Leveled 语义）。**下层无 overlap 时** 等价于 **单层一条有序流** 重切输出 SST，**不是**按输入 SST 边界 1:1 换皮。
2. **多路归并（RocksDB 实现）**：
   - **L0**：每个 SST **单独** child，经 `CompactionMergingIterator` **最小堆** 按 InternalKey 序归并。
   - **L1+**：**每层一个 `LevelIterator`**（层内按 smallest key 序 **串联** 各 SST，因同层 key range 不 overlap）；堆上通常是 **「start level 一路 + output level overlap 一路」**，**不是**「每张输入 SST 一路堆」。
   - 堆顶 key 最小者输出一条 → 该路 `Next()` → 再比；**与「哪张 SST 先入 job」无关**。
3. **E7 有序流语义（memcmp）**：排序键为 **`bucket_id` → `zorder` → `t_start_offset` → `vehicle_id/segment_seq/chunk_seq`**（见 Key 专节）。**跨桶** 近似 **日历时间序**；**同桶内** 以 **Z-order 空间曲线** 为主、**非**全库严格时间序。归并 **不按 MBR 重新聚簇**；空间局部性主要来自 **ingest 写入 key 时的 zorder**。
4. **边扫边写**：`CompactionIterator` 从有序流 **SeekToFirst 起逐条** `AddToOutput`；**不是**先物化整层再切块。每写一条 KV **前** `ShouldStopBefore` → `ShouldStopBeforeEventTime` 判断是否 **切开新输出 SST**（**仅文件级**：`target_file_size` / grandparent / 可选 **时间跨度·max_keys**；**不在此切 MBR 文件**）：
   - **文件级（保留）**：`target_file_size` / grandparent 边界；可选 **时间跨度** `effective_max_time_span`、**点数上限** `max_point_keys`（`experimental_compaction_event_time_*`）。
   - **文件级（废弃）**：~~输出 SST 内 MBR soft/hard 膨胀切文件~~ → **已收拢至 Data Block `FlushBlockPolicy`**（见下节）。
   - **块级（定稿）**：`BlockBasedTableBuilder::Add` 内 **`FlushBlockPolicy::Update`**：读 **Value Segment 头 MBR** + E7 **`bucket_id`**；跨桶或 MBR 膨胀超阈 → **Flush 当前 Data Block**（不拆 SST）。
5. **原生去重**：仅 **user key 完全相同** 时由引擎合并。
6. **簇（Mind 合同）**：指 **选入 job 的 SST 集合** + **时空切分**；**不是**先按 uid 分堆再各自排序。

```text
整层 Lx SST ──LevelIterator──┐
                              ├── 最小堆 → CompactionIterator → TableBuilder（块级 MBR 断流 + 文件级大小/时间）
Lx+1 overlap SST ─LevelIterator┘                              → 新 SST
                                                                    ↓
                                                          Version 边界: Manifest + st_file_meta
```

**Local 账本（与切分一致）**：`index_builder` 在每条 KV 写入后 **`MergeSpatioTemporalMetaFromKeyValue(user_key, value)`** 累加块 meta；**Flush** 时 **`FinalizeBlockStMetaForDataBlock`** 将全局 `[t_min, t_max]` + MBR 写入 **index block entry 的 value**（查询时用于硬性条件判定）；查询 **index block value 硬判定 + 解压后点真判**（见 Local 专节）。

### ST-compaction 代码现状（归并 / 切分，`compaction_outputs.cc` 等）

| 机制 | 状态 |
|------|------|
| L1+ **整层 picker**（`TryPickWholeLevelNonL0ST`） | **已接入** |
| Ingest **L0 全清**（`DrainL0AllFiles` @ EOF） | **已接入**（`st_bucket_ingest_build::final_st_drain`）；**须验收 L0=0** |
| Ingest **L0 单文件下移**（`StageDownNewestL0File`，`file_number` 最大） | **已接入** |
| **LevelIterator + CompactionMergingIterator** 堆归并 | **RocksDB 原生**；ST job 走同路径 |
| **E7 时间切分（文件级）** | bucket_span / time_span / max_keys @ `ShouldStopBeforeEventTime` | **可选**（`experimental_compaction_event_time_*`） |
| **E7 空间聚簇（块级）** | **`StMbrFlushBlockPolicy`**：`bucket_id` 变化 或 Value MBR 膨胀率 ≥ soft ratio | **已实现** |
| **E7 空间切分（文件级）** | ~~soft/hard MBR @ `ShouldStopBeforeEventTime`~~ | **废弃**（避免双切、保大文件顺序 I/O） |
| 输出 SST **`st_file_meta`（查询 pruning）** | **已接入**（建表 / index 路径读 value） |
| **Uid-Merge** | **历史/可选**（新库无复制时不依赖）；若启用须 **job 级 `seen`** |
| **近邻簇 picker**（锚点 + 时空近邻子集） | **已接入**（`TryPickWholeLevelNonL0ST`） |

### 指标

- **P0**：`false_negative_count`（必须 0）、`manifest_sync_files_touched`（本 job Add/Delete 的 file 数）
- **ingest**：`live_kv_count` ≈ N；`cross_bucket_replicate_count` **新库应为 0**；**`ingest_l0_file_count=0`**
- **查询**：`query_result_dedup_count` **≈0**（异常时告警）
- **历史 / 迁移**：`compaction_uid_dropped`（仅旧复制库清理时显著）

### 实现分期

| 阶段 | 内容 |
|------|------|
| **P0** | **单 Manifest** + per-SST `st_file_meta`；G1–L2 不漏解；ingest **单 key** |
| **P1** | 查询点真 + `false_negative_count` 探针 |
| **（可选）Uid-Merge** | 历史复制库迁移清理 |
| ~~跨桶 replicate~~ | **废弃（新库）** |

**运行态**：
- **定稿态**：ingest 后 **K≈N**；Manifest **live SST 完整**、`st_file_meta` 窗正确；`false_negative_count=0`。
- **历史复制态**（旧 DB）：可经 Uid-Merge + Manifest 同步迁移至定稿态；**新实验不采用**。

---

## ST-compaction 完成条件：Manifest 与 SST 同步（定稿）

### 问题本质

- **SST 真相**由 Version + 磁盘 `.sst` 决定；**查询**走 **单 MANIFEST replay** → 按 **`st_file_meta` 时间窗** 筛 SST → Local → KV。
- **Ingest / 浅层 ST（L0–L2）**：Flush 可暂写 L0，但 **ingest 结束须 L0=0**；Manifest **VersionEdit** 与 SST **同边界**追加；新 file 须带 **全段** `st_file_meta`。
- **L3+ ST**：一场 job 可 **删除旧 file、产生新 file**；若提交边界 **Manifest live 集合 ≠ Version**，或 **新 file 的 `st_file_meta` 窗过窄** → 漏解。
- **跨桶段**：不再表现为「多分片扇出」；只要 **Manifest 中 live 的 `F` 的 `[t_min,t_max]` 覆盖 Value 全段**，查询窗落在中间日历桶也能召回 **同一条 KV**。

**ST-compaction 完成** = 物理 SST 写毕 **且** Manifest / `st_file_meta` / Local 在同一 Version 边界对齐。

### 术语（定稿）

| 名称 | 是什么 |
|------|--------|
| **`MANIFEST-*`** | 标准 RocksDB descriptor log（**单条链**，`CURRENT` 指向当前生效文件）；replay 得 **live SST 列表** + level |
| **`st_file_meta`** | 每个 SST 的全局 **`[t_min,t_max]`** + MBR（建表时从 **Value 头全段** 并集；**禁止**仅用 key `bucket_id` 推短窗） |
| **Manifest 同步** | 对本 job：`DeleteFile(旧)` / `AddFile(新)` 写入 **同一 MANIFEST**；与 VersionEdit **同边界** |

### 完成条件清单（每场 ST job 提交边界）

1. **VersionEdit**：Add/Delete/level 变更（已有）。
2. **Manifest**：对本 job 触及的每个 `file_number` 写入一致的 **DeleteFile / AddFile**（**每个物理 SST 登记一次**）。
3. **`st_file_meta`**：每个 **新输出 SST** 持久化 **全段** `[t_min,t_max]`、MBR（来自 Value 头并集）。
4. **Local**：输出 SST 的 index block **value** 写入全段 `[t_min,t_max]` / `t_end_max`、MBR；缺失 → **直接报错**（不降级为 match-all）。
5. **原子性**：`CURRENT` / MANIFEST / Version **同边界**可见（§E）。
6. **（可选）Uid-Merge**：仅历史复制库 / 重复 uid 清理；与 2–4 同 job、同边界。

```text
ST job 结束:
  VersionEdit: DeleteFile(旧…); AddFile(新…)
  → 同一 MANIFEST 追加 VersionEdit
  → 新 SST: 写入 st_file_meta + Local index tail
```

### 与跨桶、Manifest 的关系

| 变化类型 | Manifest 同步 |
|----------|----------------|
| 同桶内 file 合并 | MANIFEST **Delete 旧 + Add 新**；`st_file_meta` 为新 file **全段并集** |
| 跨时间窗输出（跨多个日历桶） | **仍只 AddFile 一次**；`[t_min,t_max]` 须覆盖 **全段**，查询靠 file meta 筛 SST |
| 仅 L0→L1 下移 | MANIFEST **AddFile**；`st_file_meta` 来自该 file 内所有 segment Value 头 |

**禁止**：为跨桶段向多个 MANIFEST-b **扇出同一 `file_number`**（旧 S(F) 路径，**定稿废弃**）。

### 代码现状与修复方向（`version_set.cc` / `manifest_ops.cc`，与实现对齐）

| 机制 | 状态 |
|------|------|
| `CURRENT` → **`MANIFEST-*`（单链）** | **定稿目标**；当前 fork 仍 **`CURRENT` → `M_manager-*`**，**待改** |
| `M_manager-*` + 分片 MANIFEST-b | **历史 / 待废弃**；新库不应依赖 |
| `FanOutManifestSyncToIntersectingShards` / S(F) | **定稿关闭**；由 **单 Manifest AddFile** 替代 |
| `TryResolvePendingHostBucket` / `st_manifest_host_bucket` | **废弃为主路由** |
| 输出 SST **`st_file_meta`** | **已接入**（建表读 Value 头） |
| Uid-Merge（`CompactionOutputs`） | **历史/可选**；若启用须 **job 级 `seen`** + canonical 优先 |
| ingest `CompactFiles` + manifest shard | 迁移至单 Manifest 前：工具侧 **`allow_trivial_move=false`** |

**修复原则**：`LogAndApply` / flush 路径 **不再** 按日历桶切 Manifest；**每个新 SST AddFile 一次**，并保证 **`st_file_meta` 来自 Value 头全段**；查询 Global 层 **一次 replay + file 时间筛**。

### 指标（Manifest 同步）

- `manifest_sync_files_added` / `manifest_sync_files_deleted`
- 与 §F 共用：`false_negative_count`（必须 0）、`manifest_entry_rewrite_count`

---

## Key（UserKey：TMan-tsp ST RowKey，定稿新库）

目标（与 TMan-tsp `TrajPutUtil` ST 主索引一致，**不用 Redis**）：
- 保留 **segment V2 value**、单 Manifest、`st_file_meta`、ST-compaction、prune 全链路；**仅替换 user key**。
- Key 变长：`[2B period BE][8B spatial BE][tid UTF-8]`；排序 = 先日历 period，再 LocS 空间索引，再 tid。
- `period` = `TimeRangeIndex.periodNumber(t_start_ms)`；`spatial` = `shape | (location << alpha*beta)`（z_order 默认 resolution=10, α=β=4）。
- **移除 end_time 出 key**；`t_end`/MBR 仍在 value 头 + `st_file_meta` / block meta。

灌库（**仅 ST-RocksDB / rocks-demo，不依赖 TMan-tsp / Redis**）：
- `st_bucket_ingest_build --user-key-schema tman_st`（默认）：C++ 内建 `[period][spatial][tid]`。
- `period` 与 TMan `TimeRangeIndex` 同公式；`spatial` 为 fork 原生 Morton 质心格网（**非** HBase LocSIndex 字节）。
- 环境变量 `ROCKSDB_ST_USER_KEY_SCHEMA=tman_st` 供 prune Index Seek 下界（period lookback）。
- 可选：`tools/emit_tdrive_tman_st_keys.py` 仅在与 HBase/TMan-tsp **字节级** 对齐 key 时需要。

Legacy E7（32B，仅旧库 / `--user-key-schema e7`）：

E7 Key Layout（big-endian，memcmp 可排序；**legacy 总长 32B**）：
- `[magic]` = 0xE7 (1B)
- `[type_flags]` (1B)：**新库恒为 0**（保留扩展；**不再**使用 bit0 ghost）。历史库可能有 bit0=1 的 ghost key（`--legacy-ghost-ingest`）或 **31B** 无此字段的旧 E7（**无 type 字节时 `segment_uid` 解码偏移与 32B 不同**，Uid-Merge / 查询 dedup 须分支或拒绝旧 key）。
- `[bucket_id]` u32 BE (4B) = **canonical 日历桶** `b* = floor(t_start / bucket_seconds)`；**每 segment_uid 仅一条 key**，用于 E7 粗排序，**非**查询硬分区
- `[zorder]` u64 BE (8B) = 空间编码（用于 **key 内排序**与局部性，**不是** ST 时空分簇的唯一依据；ST 切文件看 file/block 元数据）
- `[t_start_offset]` u32 BE (4B) = `t_start - bucket_start(b*)`（秒）
- `[vehicle_id]` u64 BE (8B) = 86627503122318 这种“车 ID”（显式保留）
- `[segment_seq]` u32 BE (4B) = 段序号（来自原始 `..._k` 的 k）
- `[chunk_seq]` u16 BE (2B) = 切分序号（`#p001` 的 1；不切分则 0/1）

Key 的“时间轴”与排序（E7 memcmp）：
- **粗时间**：`bucket_id`（canonical 日历桶）排在最前 → 跨桶全局近似按小时推进。
- **空间聚簇（key 内）**：同桶内 **`zorder`** 决定先后顺序（非 key 内经纬度 float）。
- **细时间**：`t_start_offset`（相对 canonical 桶起点）。
- **id**：`vehicle_id` / `segment_seq` / `chunk_seq` 打破平局。
- `t_end`、段 **MBR** **不进入 key**（在 Value 头 + file/block `st_file_meta`）；**compaction 写盘切分** 若要用 MBR 膨胀，须从 **value** 累计（当前 **未完成**，见「ST-compaction 代码现状（归并 / 切分）」）。

**跨桶段（与上节一致）**：
- **一条** E7 key（`bucket_id = b*`）+ **完整 value**；相交日历桶靠 **SST `st_file_meta` 全段时间窗** 召回，**不**再写多份 KV。
- 已废弃：Master/Ghost、ghost bit、跨桶完整 value 复制、仅起始桶存 value。

---

## Global Index（单 Manifest + SST 级时间 meta）

Global 负责 **列出 live SST**，并用 **每个 SST 的 `st_file_meta [t_min,t_max]` + 空间 MBR** 做 file 级 **时间 AND 空间** 粗筛；**不**再维护 Master/Ghost；**不**按日历桶切 Manifest 分片。`m_manager` **仅选出 MANIFEST 文件，不持有空间信息、不做时空过滤**。

### 与原生 RocksDB 对齐（定稿）

| 概念 | 含义 |
|------|------|
| **`CURRENT`** | **1 个文件**；单行指针，指向当前生效的 **`MANIFEST-*`**（与原生 RocksDB 相同） |
| **`MANIFEST-*`** | 标准 descriptor log；**增量追加** VersionEdit（`AddFile` / `DeleteFile` / level）；replay 得 **全库 live SST** |
| **`st_file_meta`** | 每个 SST 的 **全局** `[t_min,t_max]` + MBR；建表时从 **Value 头全段** 并集；查询 **第一跳** file 级时间筛 |
| **内存缓存（可选）** | 可缓存最近 replay 的 Version / file meta；**不是**按桶分片 LRU 的合同要求 |

**定稿收益**：`m_manager` 仅做轻量 MANIFEST 文件选择（无时空过滤开销）；Manifest 一次 IO / replay 后按 **per-SST 时间窗 + 空间 MBR** 做 file 级粗筛；**避免**旧 M_manager 第一跳 + 多 MANIFEST-b 第二跳导致的多次 IO 与 S(F) 扇出漏登记。

### 组件

| 组件 | 职责 |
|------|------|
| **`m_manager`** | **仅负责选出 MANIFEST 文件**；不做时空过滤，**不持有空间信息** |
| **`MANIFEST-*`** | LSM 元数据真相：live SST 列表、level、file size；每个 SST 的 `st_file_meta`（**全局时间窗 + 空间 MBR**）；**每个物理 SST AddFile 一次** |
| **`st_file_meta`** | 附在 SST / TableProperties / index 路径；**`[t_min,t_max]` 须覆盖 file 内所有 segment 的 Value 头全段**；**MBR 为 file 内所有 segment MBR 的并集** |
| **`CURRENT`** | 指向当前 MANIFEST 基名，例如 `MANIFEST-000042` |

**磁盘命名**：与原生一致 — `MANIFEST-%06llu`；切换时写新 MANIFEST → rename → 原子更新 `CURRENT`。

### 写入 / 更新

1. **Ingest**：**一条** E7 KV（canonical `bucket_id` + 完整 value）；unified memtable / EOF flush → **AddFile** 写入 **同一 MANIFEST**。
2. **ST-compaction**：对本 job 输出/删除的 file，在同一 Version 边界 **Append VersionEdit** 至 MANIFEST；**禁止**为跨桶 SST 向多个分片重复 AddFile。
3. **新 SST**：建表路径必须写入 **`st_file_meta`**（Value 头并集）；与 Manifest AddFile **同边界**。
4. **一致性（§E）**：`CURRENT`、MANIFEST、Version、磁盘 `.sst` **同边界**一致。

### 查询（单跳 Global + Local）

1. **Global — m_manager**：**仅选出 MANIFEST 文件**（不做时空过滤）。
2. **Global — Manifest**：replay（或 Version 缓存）得 live SST；对每个 `F`，`st_file_meta.[t_min,t_max]` 与 `[q_start,q_end]` **时间相交** **AND** `st_file_meta.MBR` 与查询空间窗口 **空间相交** ⇒ 候选；meta 缺失 → **直接报错**（架构高度依赖元信息）。
3. **查询窗→bucket_id 转换**：Manifest 筛出候选 SSTable 后，将查询窗口 `[q_start, q_end]` 转换为 `bucket_id` 范围，与完整查询信息（时间窗 + 空间 MBR）一并传入 SSTable。
4. **Local**：打开候选 SST；用最小 `bucket_id` Seek 到 index block 起始位置 → **向后遍历** index block → 检查每个 entry 的 **value 中全局时空信息**（硬性条件）；当 index block key 的 `bucket_id` < 查询最小 `bucket_id` 时可安全终止（key 有序单增保证后面一定没有）。
5. **KV + 点真判**：index block value 时空信息判定通过后读 Data Block，解压 value，任一点 ∈ 查询窗才命中。
6. **去重**：定稿 ingest **K≈N**，`query_result_dedup_count` 应 **≈0**。

### 端到端查询链路（汇总）

| 阶段 | 输入 | 动作 | 漏召回策略 |
|------|------|------|------------|
| Global — m_manager | 无 | **仅选出 MANIFEST 文件** | 不持有空间信息 |
| Global — Manifest | `q_start, q_end`, 空间 MBR | MANIFEST → live SST；**file meta 时间相交 AND 空间相交** 筛候选 | meta 缺失 → **直接报错** |
| 查询窗转换 | `q_start, q_end` | 窗口→`bucket_id` 范围；与完整查询信息一并传入 SST | — |
| Local — 粗定位 | 最小 `bucket_id` | index block Seek 到起始位置；**向后**遍历 | 从最小 bucket_id 开始，不依赖起始判定 |
| Local — 硬判定 | index block value | 检查 value 中 block 全局时空信息 | 硬性条件；不满足则跳过 block |
| Local — 终止 | index block key `bucket_id` | key `bucket_id` < 查询最小 `bucket_id` → 安全停止 | key 有序单增保证后面一定没有 |
| KV | 候选 block | 解压 value；**遍历轨迹点** | 相交即保留整段 |
| **后处理** | 命中 KV | 按 `segment_uid` 去重（**异常兜底**） | 定稿态应 ≈0 |

### 历史：`M_manager` + 分片 MANIFEST-b（废弃 / 可选扩展）

- **旧方案（fork 当前代码）**：`CURRENT` → `M_manager-*`；每日历桶一行 `entry` → 分片 `MANIFEST-b`；SST 经 **S(F) 扇出** 登记到多个分片。
- **定稿（新库）**：**不采用**；迁移时可离线 replay 合并为 **单 Manifest + per-SST `st_file_meta`**。
- **超大规模（未实现）**：若单 MANIFEST replay 成为瓶颈，可再引入 **分片**；合同仍要求 **每个 live SST 可达** 且 **file 时间 meta 全段保守**，而非回到「按桶切数据」。

### 与历史 `MANIFEST_SET` 的关系（迁移口径）

- 更旧方案：`CURRENT` → `MANIFEST_SET`（混合分片 + masters/ghosts）。
- **定稿**：`CURRENT` → **`MANIFEST-*`**（单链）；**masters/ghosts 与 `experimental_global_*` 废弃**。
- 自 fork 的 `M_manager-*` 迁移：离线合并各 MANIFEST-b 的 VersionEdit，或 **Open + Repair** 重建单 Manifest；验证 `false_negative_count=0`。

---

## Local Index（SST 内，index block 有序定位 + 块级元数据硬判定）

Local 的职责：
- 先利用 **index block 的有序 key**（含 `bucket_id`）做 Seek 粗定位；
- 再检查 **index block value 中的全局时空信息**（硬性条件）做精确判定；
- 在保持 100% 召回的前提下尽量减少读取 Data Block 的次数。

### Index Block 改造

我们对 RocksDB 原生 index block 做了改造：在 index entry 的 **value** 中加入了该 block 的 **全局时间空间信息**（`[t_min, t_max]` + MBR）。这使得 index block 本身就能承担硬性过滤，无需先读 Data Block。

| 字段 | 位置 | 用途 |
|------|------|------|
| `bucket_id` | index block **key** | 粗粒度定位 + 终止判定（key 有序单增） |
| 全局 `[t_min, t_max]` + MBR | index block **value** | **硬性条件**：时间 AND 空间都相交才进入 Data Block 读取 |

### 两阶段剪枝（不漏召回）

**阶段 1 — 粗定位（index block key 的 `bucket_id`）**：
- 将查询窗口转换为 `bucket_id` 范围，用**最小 `bucket_id`** 做 `Seek` 找到起始 block；
- 从该位置**向后**遍历 index block；
- `bucket_id` **只能判定终止位置**：当 index block key 的 `bucket_id` < 查询最小 `bucket_id` 时，**之后**的所有 block 一定不包含查询窗口（因为 key 有序单增），可以安全停止；
- `bucket_id` **不能判定起始位置**：因为可能存在跨多个 `bucket_id` 的长轨迹段，起始 block 的 `bucket_id` 可能远小于查询最小 `bucket_id`，仍可能覆盖查询窗口。

**阶段 2 — 硬判定（index block value 的时空信息）**：
- 对 Seek 定位后的每个 index entry，检查其 **value** 中的全局 `[t_min, t_max]` 和 MBR；
- **时间相交 AND 空间相交** → 该 block 进入候选，读 Data Block；
- 不满足 → 跳过该 block（安全，不会漏召回）；
- 这是**硬性条件**，不是粗筛。

### 查询规则（保证不漏召回）

- 查询窗口为 \([q_start, q_end]\) + 空间 MBR：
  - 将查询窗口转为 `bucket_id` 范围；
  - 用最小 `bucket_id` Seek 到 index block 起始位置；
  - **向后**遍历，对每个 index entry：检查 value 中时空信息（硬性条件）；
  - 当 index block key `bucket_id` < 查询最小 `bucket_id` → **安全终止**（后面一定没有）；
  - **禁止**用最大 `bucket_id` 判定起始（可能漏跨桶长段）。
- 若某 file/block 的 `t_end_max < q_start`：可安全跳过（来自 index value 信息）。

### 空间过滤（精准性来源）

- **Local（候选 block）**：index block value 中的 MBR + 时间窗做硬性剪枝；
- **真命中**：先看 value header（`t_start/t_end/MBR`）做最后一层轻量判断；仍不能排除时，再解压完整 Segment value 并遍历全部轨迹点，**任一点**落入 \([q_t]×[q_{xy}]\) 窗口才计为真；
- Z-order 只用于排序聚簇，不作为”是否相交”的唯一判定依据。

**Value（Segment）**：
- 存完整 points；段级 `t_end`、MBR 等用于 Local 元数据构建（写入 SST index meta + index block value），**不**写入 E7 key。

---

## 当前阶段的实现优先级（只保留有效口径）

| 阶段 | 内容 | 状态（相对代码） |
|------|------|------------------|
| **P0** | E7 定长 key；**canonical `bucket_id`**；**单 key ingest（无跨桶复制）**；ingest 幂等 | Key 部分已有；ingest 单 key **待改代码** |
| **P1** | **单 Manifest**（`CURRENT` → `MANIFEST-*`）；per-SST `st_file_meta`；查询 **file 时间筛** | fork 仍 **M_manager 路径**，**待改** |
| **P2** | Local：`t_end_max`、MBR + 解压后点真判 | 已接入 |
| **P3** | ST-compaction + **Manifest 同步** + 指标 §F | 物理 ST + 整层 picker 已有；**单 Manifest 闭环** 待完成 |

- 废弃：`experimental_global_master_ghost`、`type_flags` ghost 语义、Master/Ghost ingest、**跨桶完整 value 复制**、per-bucket flush 主路由。

---

## Compaction 策略（最终口径：L0→L1 下移 + L1+ ST 归并 + Manifest 闭环）

### 术语（避免歧义）

- **RocksDB 自动 compaction**：引擎后台按 **score / picker / 各层预算** 自行决定「何时压、选哪些文件」，应用侧不逐次发起。可通过 `disable_auto_compactions` 等关闭。**ST 方案下 L1+ 的驱动以字节阈值与 ST-compaction 为准时，等价于不再依赖该自动决策来推进浅层（具体是否关自动以部署为准）。**
- **ST-compaction**：本项目对 **L1 及以下层（L1+）物理 SST 归并** 的约定命名，包含 **调度触发、整层范围、时空近邻簇、输出粒度与优先级**；不等同于「只做逻辑 Manifest」。
- **「高层」**：靠近 L0 的层（当前讨论范围 **L1–L3**，后续可再扩）。

总体原则（最新版）：

- Compaction 分为三条通道（概念上）：  
  1) **L0→L1 下移通道**（只做阶段迁移，不做跨文件归并语义）；  
  2) **ST-compaction 通道（L1+）**（SST 物理归并：调度、整层范围、簇与输出策略）；  
  3) **Manifest 逻辑通道**（**ST 完成条件**：单 MANIFEST VersionEdit 与 `st_file_meta` 与 SST 变更对齐）。
- 查询正确性优先：**不允许 false negative**。

### ST-compaction（调度、并行、阈值与簇 — 当前定稿）

**触发（按阶段）**

1. **L0 → L1（下移阶段）**：L0 **仅作 memtable flush 的暂存层**，**不是**数据长期驻留层。每次 Flush 产生 L0 SST 后，须 **StageDown 到 L1**（`StageDownNewestL0File`，`allow_trivial_move=false`）。**Ingest 完成条件：L0 文件数 = 0**（见下节 **「Ingest 与 L0：禁止堆积」**）。  
2. **Lx → Lx+1（x≥1，归并阶段）**：从 L1 开始，才进入 ST-compaction 的逐层归并与约束放宽；当 **Lx 层上所有 SST 的合计字节数**（Version 内该层 `file_size` 之和口径）**达到该层字节阈值**时，触发向下归并。**阈值以字节为准，不以文件个数为主指标**。  
  - **阈值定义（定稿）**：对 **第 2 类** 涉及的每一层 **Lx（x≥1）**，**触发阈值 = 该层「层容量 / 层字节预算」的 90%**。其中「层容量」与 RocksDB 对该 CF 的 **`MaxBytesForLevel(Lx)`**（或与之等价的配置推导值）对齐；即当 **sum(file_size on Lx) ≥ 0.9 × MaxBytesForLevel(Lx)** 时，满足本类触发条件（实现中可用同一口径的缓存值，以 Version 为准）。**L0 不适用本条 90%**，L0 仅承担下移阶段职责。

**调度优先级**

- 同一 CF 内：优先完成 **L0→L1 下移**（清 L0），再处理 **L1 及以下各层的字节阈值归并**，避免 L0 无限堆积与 flush stall 风险。

**范围（整层 — 当前实现锚点）**

- 某层 **Lx** 因字节超阈触发时：**输入包含该层在 Version 中的全部 SST**，并按 RocksDB 不变量扩展 **Lx+1 中与上述键域重叠** 的文件；输出写到 **Lx+1**。即 **一次任务对该层「整层泄洪」**（`TryPickWholeLevelNonL0ST`），而不是原生 picker 常见的「只选子集文件」作为 ST 侧合同。

**并行**

- **ST-compaction 在单 CF 内串行排队**（一次只跑一个由 ST 调度认可的任务）。  
- **并行度优先用单次任务内的 `max_subcompactions`**（同一次归并多路输出/子任务），**不**先依赖「同 CF 内两个 compaction job 并行跑多层」。若启用 Uid-Merge，`seen` 表须 **job 级** 共享。

**簇（cluster）与输出粒度（合同 vs 当前代码）**

| 层次 | 定稿语义 | 当前实现 |
|------|----------|----------|
| **选输入** | 时空近邻 **簇**（锚点 + 近邻 SST 子集） | **已接入** `TryPickWholeLevelNonL0ST` |
| **写输出** | **文件**：大小/grandparent/可选时间·max_keys；**块**：MBR 聚集度 Flush | **已实现** `StMbrFlushBlockPolicy`；文件级无 MBR 硬切 |
| **簇级中止** | 仅超大簇本轮跳过，不中止整场 | 与整层泄洪并存时，指标上需区分 `whole_level` vs `cluster_skip`（待接） |

- **输出**：在归并写盘时 **优先单输出 SST**，必要时因 `target_file_size` / 硬上限 **再拆**；**不是**按桶边界主动切很多小文件。  
- **勿混淆**：「簇」指 **时空选入 / 切分**；**不是**按 `segment_uid` 聚簇。

**与「自动 compaction」关系（对外说明）**

- L1+：**由字节阈值与上述优先级定义的 ST-compaction 驱动**，不依赖「每层 score 触发的 RocksDB 自动选层」作为合同行为（若线上关闭自动 compaction，则由工具/线程显式发起等价任务）。  
- L0：**ingest 工具显式 drain**（`disable_auto_compactions=true` 时后台不会清 L0）；合同语义是 **逐文件 StageDown 到 L1**，不是「留在 L0 等自动 compaction」。

### Ingest 与 L0：禁止堆积（定稿，2026-05）

**现象（错误态）**：全量 ingest 结束后 **L0 仍有一百多个 SST**（实测曾出现 **~146**）。这是 **ingest 未完成**，不是正常 ST 形态。

**定稿态**：`st_level_stats` / Version 口径 **L0 file_count = 0**；全库 live SST 在 **L1+**（unified ingest 目标 **1×L1** 或少数 L1 文件）。

| 根因 | 机制 | 定稿 |
|------|------|------|
| **`--per-bucket-flush`（废弃）** | 每个日历桶窗口 Flush → **+1 L0 SST**；桶数 ~146 ⇒ L0≈146 | **禁止**新实验使用 |
| **ingest 期间只 Flush 不 StageDown** | unified 模式下 `FlushMemtableIfPressure` 只写 L0；`DrainL0WhenAtCompactionTrigger` 仅在 **L0 数 ≥ trigger** 时每次移 **1 个** 文件，**不能**清空低于 trigger 的剩余 L0 | 允许 ingest **过程中**暂存多个 L0；**EOF 必须** `DrainL0AllFiles` |
| **EOF drain 未跑完 / 失败** | `final_st_drain`（`DrainL0AllFiles` + L1+ ladder）失败或被跳过 | ingest **不算完成**；须报错并重跑 `--st-ladder-drain-only` |
| **`disable_auto_compactions=true`** | 无后台 L0 compaction | **不能**指望 RocksDB 自动清 L0；工具侧 **必须**同步 StageDown |

**Ingest 完成 checklist（Compaction 侧）**：

1. EOF **Flush**（最后一批 memtable）。
2. **`DrainL0AllFiles`**：循环 `StageDownNewestL0File` 直至 **L0=0**（非 `DrainL0WhenAtCompactionTrigger` 的「减到 trigger 以下」）。
3. **L1+ ST ladder**（按层 90% 字节阈值，可选至目标层）。
4. **验收**：`st_level_stats` 打印 `L0=0`；`L1` 有 live SST；`false_negative_count=0`（Manifest 同步后）。

**与 `level0_file_num_compaction_trigger` 的关系**：该 trigger 用于 **ingest 过程中** 的 `DrainL0WhenAtCompactionTrigger`（缓压，非清场）；**ingest 结束清 L0** 以 **`DrainL0AllFiles`** 为准，**不**以 trigger 为停步条件。

### A. 物理 Compaction（SST 层）——L0 仅下移，L1+ 按层放宽跨桶（实现锚点，与簇语义并存）

- L0→L1 定义为阶段性下移，不承担跨桶合并语义。
- 从 L1 开始逐层放开跨桶限制（与「高层 L1–L3 更严」叙事一致）：
  - L1：最严格，优先同桶/近桶；
  - L2-L3：中等放宽，允许小范围跨桶；
  - L4-L6：明显放宽，L5-L6 跨桶视为常态。
- 每层候选受三类约束；**与代码一致的默认档位（唯一来源：`rocksdb/db/compaction/st_event_split_policy.h`，以 `output_level` 为准）**：
  - **跨桶**：**不限制** `max_bucket_span`（块级 `bucket_id` 变化仍可触发 Data Block Flush）；Manifest **AddFile 一次** + **`st_file_meta` 全段**。
  - **Level 触发输入**：锚点 SST = 该层 **`t_min` 最早 / smallest 最小**；再按时空近邻选候选子集（`TryPickWholeLevelNonL0ST`）。
  - `max_time_expansion`：对 `experimental_compaction_event_time_max_span`，**仅 L4+ 放宽为 2×**；L0–L3 使用基准值。
  - `max_mbr_expansion`：**仅用于 Data Block** `FlushBlockPolicy`（`experimental_compaction_spatial_soft_expansion_ratio`，默认缺省 2.5× 等效膨胀）；**不**再用于文件级 `ShouldStopBeforeEventTime` spatial cut。
- **注意**：历史文档中的「输出切分」指归并写盘时 **多输出 SST 的边界策略**；**ST-compaction 合同**下以 **簇内优先单 SST** 为主表述，二者在实现上需统一到 **CompactionOutputs / 簇选择** 路径，避免 **`output_level` 分档与「物理仅在浅层执行」错位**（分档键应以设计为准：`start_level`、物理层或显式阶段 ID，见实现评审）。
- 可调 CF 选项（工具 `st_meta_compact_existing` 映射）：`experimental_compaction_event_time_split`、`experimental_compaction_event_time_bucket_width`、`experimental_compaction_event_time_min_keys_per_file`、`experimental_compaction_event_time_max_span`、`experimental_compaction_event_time_max_point_keys`、空间 ratio 与 hard min file size。**关闭 ST 物理切分**：不传 `--st-time-split` 且不启用上述非零参数（详见工具帮助）；此时仍为普通 Leveled `CompactRange`，但 **fork 内 ST-aware picker 等改动仍可能存在**，与上游 RocksDB 二进制对比需使用未补丁构建。
- 每次输出新 SST 后，必须重算并持久化 Local 元数据（至少 `t_end_max`、MBR）；index tail 仍保留 bitmap 槽位（写全 1，查询不用）。
- **ST job 完成条件（Manifest 同步，挂载点）**：每场 ST job 在 VersionEdit 之外，同边界 **Append MANIFEST** 且新 SST 带 **全段 `st_file_meta`**（见专节「Manifest 与 SST 同步」）。

### B. 物理归并对 Global 的影响
- SST 经 compaction 迁移或合并后，须在 **同一 MANIFEST** **DeleteFile(旧) / AddFile(新)**，与 Version **同边界**。
- **Ingest 与 ST**：ingest **单 key、无复制**；**Manifest 与 `st_file_meta` 对齐** 为 ST 提交 **必备完成条件**。

### C. Manifest 逻辑 Compaction（= ST 完成条件 + 清理）
- 与 **「Manifest 与 SST 同步」** 专节同义；此处为 Compaction 策略侧摘要：
  1) **VersionEdit**：MANIFEST 内 `DeleteFile`/`AddFile` 与 Version 一致（**每 SST 登记一次**）；
  2) **`st_file_meta`**：新 file 的 `[t_min,t_max]`、MBR 来自 Value 头 **全段并集**；
  3) **陈旧 entry 清理**：以新 Version 为真值源，预算内清理 stale 引用。
- 重写原则：以新 Version 为真值源；**MANIFEST + CURRENT** 与版本切换同边界提交；禁止出现「CURRENT 已切换，但 MANIFEST 仍引用旧 SST 集」的窗口。
- 清理采用预算控制（限额、后台低优先级），避免影响前台读写。

### D. 防漏召回约束（与 Compaction 强绑定）
- 因 key 不含 `t_end`，Compaction 必须确保 file 级 `st_file_meta` 和 index block value 中 `[t_min, t_max]` + MBR 完整可用。
- 查询时：Manifest 级用 `st_file_meta` 做时间+空间粗筛；Local 级用 index block value 做硬判定。
- **元数据缺失时的策略（实现要求）**：架构高度依赖元信息的完整性和正确性。若 **value 缺失、Segment 头解码失败、或 `st_file_meta` / index block value 元信息丢失**，**直接报错**，不得降级为 match-all 或生成偏窄假边界。元信息是查询正确性的唯一保障，损失后无法保证不漏召回。

### E. 一致性要求
- Compaction 版本切换与 **MANIFEST** 变更需保持同一版本边界可见。
- 失败时允许降级但必须告警，不允许 silent inconsistency。

### F. 关键指标（实验前必须接入）
- `false_negative_count`（**必须恒为 0**）
- `manifest_sync_files_added` / `manifest_sync_files_deleted`
- `live_kv_count` ≈ N；`cross_bucket_replicate_count`（**新库应为 0**）
- **`ingest_l0_file_count`**（ingest 结束时 **必须为 0**；`st_level_stats` 验收）
- `query_result_dedup_count`（定稿态 **≈0**）
- `compaction_uid_dropped`（**仅历史复制库**迁移时显著）
- `manifest_manager_update_count`、`manifest_shard_add/del`、`manifest_entry_rewrite_count`
- `manifest_manager_filter_shards_kept` / `manifest_manager_filter_shards_skipped`（**历史** M_manager 路径指标；单 Manifest 下改用 **file 时间筛保留/跳过数**）

---

## Data Block 空间断流（定稿：`FlushBlockPolicy`，非文件级 MBR 切刀）

**原则**：放弃 **SST 文件级** MBR soft/hard 切分（破坏 Native 大文件连续写、与 E7 key 无 MBR 不对齐）；在 **`BlockBasedTableBuilder::Add`** 流式写盘时，用 **Value Segment 头 MBR** 作 **Data Block 断流阀门**（RocksDB 原生 `FlushBlockPolicy::Update`，触发 `Flush` 当前块）。

**影子状态（Policy 内，与 index_builder 分工）**

| 组件 | 职责 |
|------|------|
| **`StMbrFlushBlockPolicy`** | 写入**前**判定是否 Flush；读 **Value MBR** + E7 **`bucket_id`** |
| **`index_builder`** | 写入**后** `MergeSpatioTemporalMetaFromKeyValue` → **`FinalizeBlockStMeta`**（查询用权威账本） |

**断流规则（每条 KV 写入前，`Update(ikey, value)`）**

1. 当前 Data Block **空** → 不 Flush；Policy 重置 tracker。
2. **首条**（tracker 无效）→ 不 Flush；用本条 Value 头初始化 tracker。
3. **跨 `bucket_id`**（E7 key `[2:6)` BE）→ **Flush**（当前块不含本条；本条为下一块首 key）。
4. **空间**：模拟 `union(tracker.mbr, seg_mbr)`，若 **面积膨胀率** `(union−old)/max(old,ε) ≥ experimental_compaction_spatial_soft_expansion_ratio`（缺省 **2.5**）→ **Flush**。
5. 否则 **并入** tracker，继续积攒同一 Block。
6. **与大小策略组合**：`Update` 亦受 **`FlushBlockBySizePolicy`** 约束（先满足 block_size 上限）。

**MBR 来源**：**仅 Value Segment 头**（28B，`t_start/t_end/x_min/…`）；**禁止**对 E7 用 `MergeSpatioTemporalMetaFromUserKey` 做切刀。

**指标（待接）**：`block_mbr_flush_count`、`block_bucket_change_flush_count`、块均 MBR 面积、`key_level_keys_skipped_false_positive` 降幅。

---

## Local 查询剪枝（定稿：index block value 硬判定 + 解压后点真判）

- **Index Seek 粗定位（P1）**：将查询窗口转换为 `bucket_id`，用**最小 `bucket_id`** 构造 Seek 下界，定位 index block 起始位置。新库 **TMan ST key**：下界 `period = max(0, period(q_t_min) - lookback)`；旧库 **E7**：下界 `bucket_id = max(0, floor(q_t_min/W) - lookback)`（`lookback=UINT32_MAX` → 最保守）。从 Seek 位置**向后**遍历 index block。
- **Index block value 硬判定**：对每个 index entry，检查其 **value 中的全局 `[t_min, t_max]` + MBR**；时间 AND 空间都相交才进入 Data Block。这是**硬性条件**，不满足则安全跳过该 block。
- **终止条件（index block key `bucket_id`）**：当 index block key 的 `bucket_id` < 查询最小 `bucket_id` 时，**之后**的 block 一定不包含查询窗口（key 有序单增），可安全停止遍历。**不能**对称地用于判定起始（跨桶长段可能从更早的 bucket_id 开始）。
- **键/值级**：index block value 判定通过后读 Data Block；对 E7 Segment **解压 value，遍历 `point_count` 个 `(t,lon,lat)`**，仅当存在一点落入查询窗时 iterator 才停在当前键（`value_level_point_truth_enable`，默认开）。
- **防漏召回**：index block value 或 `st_file_meta` 元信息缺失 → **直接报错**（架构高度依赖元信息，不降级为 match-all）。
- **指标**：`key_level_keys_skipped_false_positive`（index value 时空相交但无点真命中而跳过）。

---

## 一次性实现清单（思路对齐后改代码，勿靠全量灌库试错）

下列顺序保证 **先修正确性 / 崩溃**，再收敛存储与指标。

### Phase A — Global / Manifest（P0，与 ingest 崩溃直接相关）

**实现目标（定稿）**：`CURRENT` → **单 `MANIFEST-*`**；每个 SST **AddFile 一次**；查询 **file `st_file_meta` 时间筛**；**关闭** S(F) fan-out / M_manager 主路径。

1. **`CURRENT` → `MANIFEST-*`**  
   - 新库 Open / Recover：**不再要求** `M_manager-*`；与原生 RocksDB descriptor 链一致。  
   - 迁移：旧 `M_manager` + MANIFEST-b 离线合并或 Repair 重建。

2. **Flush / ST 写 Manifest**  
   - 对本 job **Delete/Add** 的每个 `file_number`：**单次** VersionEdit 追加至 **同一 MANIFEST**。  
   - **禁止** `FanOutManifestSyncToIntersectingShards` / 按 `floor(t_min/W)..floor(t_max/W)` 扇出多 MANIFEST-b（定稿废弃）。

3. **`st_file_meta` 全段**  
   - 建表路径：每个新 SST 的 `[t_min,t_max]`、MBR = file 内所有 segment **Value 头并集**；**禁止**仅用 key `bucket_id` 推短窗。

4. **查询 Global**  
   - `m_manager` **仅选出 MANIFEST**；replay MANIFEST → live SST；`st_file_meta` 时间窗 AND 空间 MBR 都与查询窗相交 ⇒ 候选；未知 meta ⇒ **直接报错**。

5. **原子性**  
   - `ProcessManifestWrites`：MANIFEST 与 Version **同 recover 边界**；`CURRENT` 切换与 descriptor 追加原子可见。

### Phase B — ST-compaction 写路径

6. **Ingest EOF L0 全清（P0）**  
   - `final_st_drain`：`DrainL0AllFiles` 直至 **L0=0**，再跑 L1+ ladder。  
   - 验收：`st_level_stats` **L0=0**；禁止把「~146 个 L0 SST」当作正常 ingest 结果。

7. **（可选）Uid-Merge job 级** — 仅历史复制库 / 重复 uid 清理；`st_uid_merge_seen_` 须 **CompactionJob** 级共享。

8. **E7 Data Block 空间断流（MBR 膨胀 + 跨 bucket_id）**  
   - **`StMbrFlushBlockPolicy`**（`FlushBlockPolicy::Update`）：读 **Value Segment 头 MBR** + E7 **`bucket_id`**；膨胀率 ≥ `experimental_compaction_spatial_soft_expansion_ratio`（缺省 **2.5**）或跨桶 → **Flush Data Block**（ingest + compaction 共用）。  
   - **废弃** 文件级 `ShouldStopBeforeEventTime` soft/hard spatial cut；文件级仅保留 size/grandparent/可选 time·max_keys。  
   - 验收：块 MBR 更紧 → `key_level_keys_skipped_false_positive` / block 读取次数下降；`false_negative_count` 恒 0。

9. **ST 提交钩子**  
   - 每场 ST job 结束：**MANIFEST VersionEdit + `st_file_meta`**（与 VersionEdit 同边界）。

### Phase C — 查询与工具

10. **`false_negative_count` 探针** + `query_result_dedup_count`（定稿态应 ≈0）。  
11. **ingest 单 key**：移除跨桶 replicate；保持 `allow_trivial_move=false` 直至 Phase A 验证通过。  
12. **§F 指标**：`manifest_sync_*`、`false_negative_count` 等接入。

### Phase D — 可选 / 后续

- 近邻 **簇** picker（替代整层泄洪）与文档「空间临近 SST」对齐。  
- **历史** `M_manager` / `ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_MAX` 迁移清理（单 Manifest 定稿后删除）。  
- 31B E7 兼容策略（迁移或显式拒绝）。

### 验证（小规模，非全量无锡）

| 步骤 | 目的 |
|------|------|
| 单元：Manifest Add/Delete + `st_file_meta` 时间筛 | 不打开 DB |
| unified ingest + EOF drain | `ingest_l0_file_count=0`；`live_kv_count≈N`；`L1` 有 SST |
| `traj_db_audit` / `st_level_stats` | `CURRENT` → `MANIFEST-*`；live SST 与 Version 一致；`st_file_meta` 全段 |
| crossbucket FN 探针 + 强制 ST | `false_negative_count=0` |

---

## Compaction 里“空间临近 SST”判定（补充闭环）

采用“两阶段：粗筛 + 精筛”的判定方式，避免全量两两比较。

### A. 粗筛（低成本）
- 时间先行：仅在满足当前 level 的 `max_bucket_span` 候选中比较。
- 空间粗筛条件（任一满足即可入候选）：
  1) MBR 相交；
  2) MBR 边界最小距离 `edge_distance <= d_level`；
  3) MBR 中心点 Z-order 前缀距离在阈值内（可选辅助，不作为唯一标准）。

### B. 精筛评分（候选 K 内排序）
- 对粗筛候选计算综合分数（越小越近）：
  - `S = wt * D_t + ws * D_s + we * P_e + wb * P_b`
- 空间主评分采用 IoU（交并比）：
  - `IoU = area(intersection(MBR_base, MBR_cand)) / area(union(MBR_base, MBR_cand))`
  - 若 `IoU > 0`：`D_s = 1 - IoU`
  - 若 `IoU = 0`：使用边界最短距离退化：
    - `dx = max(0, bx_min - ax_max, ax_min - bx_max)`
    - `dy = max(0, by_min - ay_max, ay_min - by_max)`
    - `d_edge = sqrt(dx^2 + dy^2)`
    - `D_s = 1 + d_edge / S_norm`
- 其余项定义：
  - `D_t`：时间距离归一化（区间相交时为 0）
  - `P_e`：MBR 膨胀惩罚（union 面积相对扩张）
  - `P_b`：输入 SST 大小不平衡惩罚
- 推荐初始权重：
  - `wt=0.35, ws=0.35, we=0.20, wb=0.10`

### C. 按层放宽
- L0：下移阶段，不参与该节“归并候选近邻评分”的放宽讨论
- L1：`d_level` 小、`mbr_expansion` 严格
- L2-L3：中等
- L4-L6：`d_level` 放宽，允许更大空间跨度（与 **深层（L4+）** 跨桶策略一致；本文档中 **「高层」** 专指近 L0 的 L1–L3，勿与此处混淆）

### D. 安全回退
- 若严格条件下选不出足够候选：
  - 逐级放宽 `d_level` 与扩张阈值；
  - **对已由 ST-compaction 接管的层**：回退行为应在 **ST 调度器内** 定义（例如缩小簇、跳过本簇、延迟重试），**不**隐含为「整场改交 RocksDB 自动 picker」除非该层明确未接管。
  - **对未接管 ST 的层或实验对照路径**：可回退到 RocksDB 原生 compaction 选择，避免 compaction 饥饿。


