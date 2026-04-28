本文件是当前版本的“最终口径”，用于对齐代码实现与实验设计。任何与本文件冲突的旧思路（例如：只按 SST 落桶、不考虑跨桶长段、或仅用 Seek(q_start) 导致漏召回）都视为过时，应删除/弃用。

---

数据集（当前）：`data/generated_trajs_segments*.jsonl`（一行一个轨迹段，value 为完整 points）。

查询语义（必须保证）：
- 输入：时间窗口 \([q_start, q_end]\) + 空间窗口（MBR）+（可选）vehicle_id。
- 输出：**相交即返回整段轨迹段（完整 value）**。
- 允许索引阶段 false positive，不允许 false negative（**100% 召回**）。

---

## Key（UserKey / E7，定长，排序决定性能）

目标：
- 以时间为主轴 + 空间聚簇（Z-order）+ 显式 vehicle_id（支持 `id=866...` 语义）。
- Key 定长，便于前缀压缩、范围扫描、以及 Ghost 直接存 `Master_Key` 引用。
- **移除 end_time 出 key**，由元数据（t_end_max）保证不漏召回。

E7 Key Layout（big-endian，memcmp 可排序）：
- `[magic]` = 0xE7 (1B)
- `[bucket_id]` u32 (4B) = floor(t_start / bucket_seconds)
- `[zorder]` u64 (8B) = 空间编码（用于聚簇/排序，不承担精准剪枝）
- `[t_start_offset]` u32 (4B) = t_start - bucket_start（秒）
- `[vehicle_id]` u64 (8B) = 86627503122318 这种“车 ID”（显式保留）
- `[segment_seq]` u32 (4B) = 段序号（来自原始 `..._k` 的 k）
- `[chunk_seq]` u16 (2B) = 切分序号（`#p001` 的 1；不切分则 0/1）

Key 的“时间轴”：
- 只排序 `t_start`（通过 bucket_id + t_start_offset）。
- `t_end` 不进入 key（保存在 Value 或 Meta 中）。

---

## Global Index（真实时间桶分片 Manifest，Master + Ghost）

动机（跨桶治本）：
- 长段可能跨多个桶；如果只在起始桶记录，会导致查询其他桶“发现不了”该段（漏召回）。
- 不能切分逻辑段（返回整段），也不能复制完整 value（浪费空间）。

治本方案 B：Master + Ghost（不切分逻辑数据，只切分索引权）

### Master（主片段）
- 轨迹段的完整数据（value）只存一份，物理归属到它的起始桶 bucket(t_start)。
- Master 的定位字段为 `Master_Key`（即 E7 定长 user key）。该 key 不随 compaction 改变。

### Ghost（引用片段）
- 对于该段覆盖的所有后续桶（B、C、D…），在这些桶的 Manifest 中写入 Ghost 引用项：
  - `Master_Key`（定位字段，逻辑全路径）
  - `t_end` 副本（用于时间快速过滤）
  - `MBR` 副本（用于空间快速过滤）

### Ghost 的二段式过滤（减少跨桶回表）
1) Level-1（Global Filter）：扫描桶 Manifest 时，先用 Ghost 自带 `t_end/MBR` 判断是否可能相交；不相交直接丢弃。
2) Level-2（Remote Fetch）：仅对相交的 Ghost，拿 `Master_Key` 去起始桶取完整段并返回。

### Ghost 副本一致性（必须强约束）
- Ingestion 时 Master 与 Ghost 同时生成：`t_end/MBR` **只计算一次**，形成 `MasterMeta`。
- Ghost 的副本字段必须直接从 `MasterMeta` 拷贝，禁止二次计算。
- 推荐提供 `--verify-ghost`（默认开）：发现 Ghost 副本与 MasterMeta 不一致，立即 fail-fast。

---

## Local Index（SST 内，块/文件级元数据 + t_end_max 加固）

Local 的职责：
- SST 内部的 block / file 级过滤与加速（IndexBlock/Filter/Meta）。
- 在保持 100% 召回的前提下尽量减少读取 Data Block 的次数。

关键加固：`t_end_max`（解决“移除 end_time 出 key”导致的漏召回）
- SST/file 级必须记录 `t_end_max`
- DataBlock 级也建议记录 `t_end_max`（更细粒度跳过）

查询规则（保证不漏召回）：
- 当查询窗口为 \([q_start, q_end]\)：
  - 若某 file/block 的 `t_end_max < q_start`：可安全跳过
  - 否则：即便其 key 的 `t_start < q_start`，也必须进入扫描/下钻（因为存在“早开始、晚结束”的长段覆盖窗口）

空间过滤（精准性来源）：
- Global/Local 的精准过滤仍以 MBR（以及后续可加 bitmap）为主；
- Z-order 只用于排序聚簇，不作为“是否相交”的唯一判定依据。

---

## 当前阶段的实现优先级（只保留有效口径）
- P0：落地 E7 Key 编码与 ingestion（保证唯一性与定长）。
- P1：落地 Global：真实桶分片 Manifest + Master/Ghost + 启动恢复。
- P2：落地 Local：t_end_max + MBR/bitmap 过滤与统计指标。
- P3：Compaction 策略在 P0/P1/P2 验证后接入（本文件不再保留旧的 compaction 细化草案，避免误导）。

---

## Compaction 策略（最终口径，Level-aware + Master/Ghost 闭环）

总体原则：
- Compaction 分为两条通道：  
  1) 物理 Compaction（RocksDB SST 重写/合并）；  
  2) 逻辑 Compaction（Global Manifest 的 Master/Ghost 维护与清理）。
- 查询正确性优先：**不允许 false negative**。

### A. 物理 Compaction（SST 层）——按 L 层放宽跨桶
- 不再严格禁止跨桶；跨桶程度随层级放宽：
  - L0-L1：严格，优先同桶，最多允许相邻桶；
  - L2-L3：中等放宽，允许小范围跨桶；
  - L4-L6：明显放宽，L5-L6 跨桶视为常态。
- 每层候选受三类约束（参数表后续落地）：
  - `max_bucket_span[level]`
  - `max_time_expansion[level]`
  - `max_mbr_expansion[level]`
- 每次输出新 SST 后，必须重算并持久化 Local 元数据（至少 `t_end_max`、MBR）。

### B. Master 桶物理 Compaction 对 Ghost 的影响
- **零影响**（前提：`Master_Key` 稳定不变）。
- Master 从旧 SST 迁移到新 SST 时，Ghost 不需要改写引用字段。
- 只需更新 Global 中该桶相关文件元数据（MBR、`t_end_max`、文件集合）。

### C. Ghost 逻辑 Compaction（Manifest 层）
- Ghost 不参与 RocksDB 底层 SST compaction，需单独维护：
  1) 冗余合并：同桶内多个 Ghost 指向同一 `Master_Key` 时合并为一条；
  2) 悬挂清理（Dangling Clean）：对老 Ghost 做轻量探针 `Exists(Master_Key)`，`NotFound` 则删除；
  3) 查询兜底自愈：查询时若 Ghost 回表 `Get(Master_Key)=NotFound`，忽略并异步标记清理。
- 清理采用预算控制（限额、后台低优先级），避免影响前台读写。

### D. 防漏召回约束（与 Compaction 强绑定）
- 因 key 不含 `t_end`，Compaction 必须确保 file/block 级 `t_end_max` 完整可用。
- 查询时仅当 `t_end_max < q_start` 才可安全跳过；否则必须扫描/下钻。

### E. 一致性要求
- Compaction 版本切换与 Manifest 变更（Master/Ghost 增删改）需保持同一版本边界可见。
- 失败时允许降级但必须告警，不允许 silent inconsistency。

### F. 关键指标（实验前必须接入）
- `cross_bucket_compaction_count`（按层）
- `bucket_span_histogram_by_level`
- `manifest_master_add/del`
- `manifest_ghost_add/del/merge`
- `dangling_probe_count`, `dangling_clean_count`
- `remote_fetch_count`
- `ghost_level1_filter_drop_ratio`
- `false_negative_count`（必须恒为 0）

---

## 位图设计（最终口径：Local 最后一道快速判定）

目标：
- 在 block MBR 粗过滤后，进一步降低假阳性读块；
- 保持 100% 召回（只允许 false positive，不允许 false negative）；
- 兼顾 compaction 更新成本。

### A. 位图形态（首版）
- 使用 block 级固定网格位图：`16 x 16 = 256 bit = 32B`。
- 坐标系：以该 Data Block 的 MBR 归一化映射到网格。
- 置位规则：block 内轨迹点映射到对应格子并置位（并集）。

### B. 查询链路（Local）
- 顺序：`t_end_max` -> `MBR` -> `Bitmap` -> `DataBlock真实点验证`
- 位图判定：
  - 查询窗口映射为 `query_mask`
  - 若 `(block_bitmap & query_mask) == 0`：安全跳过
  - 否则：继续读取/验证

### C. 防漏召回约束
- 位图仅做“拒绝判定”，不能单独作为命中判定。
- 查询窗口映射需覆盖边界格（floor/ceil 一致规则），避免边界误差错杀。
- 与 MBR/t_end_max 联合使用，保证不漏召回。

### D. Compaction 更新
- 新输出 block 的位图优先从真实点重建（最准确）。
- 若做文件级聚合位图，采用 block 位图按位 OR。
- Ghost 侧不放位图（避免 Manifest 膨胀）；Ghost 继续使用 `t_end + MBR`。

### E. 位图实验参数
- 默认 `grid_size = 16`
- 消融：`8 / 16 / 32`
- 指标：
  - `bitmap_reject_blocks`
  - `bitmap_false_positive_ratio`
  - `block_read_count`
  - `query_latency_p50/p95`

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
- L0-L1：`d_level` 小、`mbr_expansion` 严格
- L2-L3：中等
- L4-L6：`d_level` 放宽，允许更大空间跨度（与高层跨桶策略一致）

### D. 安全回退
- 若严格条件下选不出足够候选：
  - 逐级放宽 `d_level` 与扩张阈值；
  - 最终回退到 RocksDB 原生 compaction 选择，避免 compaction 饥饿。

