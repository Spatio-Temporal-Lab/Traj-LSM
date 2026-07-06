# 实验规范 v2.0

## 一、统一数据集与查询窗口

### 1.1 数据集

| 字段 | 值 |
|------|-----|
| 源 | unified (cd + tdrive + geolife) + wuxi |
| 总段数 | 284,989 |
| 总点数 | 80,262,788 |
| 时间跨度 | 2000-01 ~ 2020-08（7534天） |
| 空间跨度 | (-179.7, 1.0) ~ (180.0, 64.8) |
| bucket宽度 | 86400s（1天） |

### 1.2 查询窗口

| 窗口ID | 层级 | 时间窗(s) | 空间窗 | 段数 |
|--------|------|----------|--------|:---:|
| narrow_02 | narrow | 3600 | 0.5°×0.5° | 1226 |
| narrow_03 | narrow | 3600 | 0.5°×0.5° | 48208 |
| narrow_04 | narrow | 3600 | 0.5°×0.5° | 0 |
| medium_02 | medium | 13320 | 1.2°×1.2° | 278 |
| medium_03 | medium | 13320 | 1.2°×1.2° | 7867 |
| medium_04 | medium | 13320 | 1.2°×1.2° | 0 |
| wide_02 | wide | 43200 | 3.6°×3.6° | 9511 |
| wide_03 | wide | 43200 | 3.6°×3.6° | 497 |
| wide_04 | wide | 43200 | 3.6°×3.6° | 0 |

> 9个窗口，3个时间窗口 × 3个空间尺度。narrow_04/medium_04/wide_04段数为0，用于测试空查询。

---

## 二、实验分类

### A. 对比实验：ST-LSM vs TMan

| 项目 | 说明 |
|------|------|
| 对比对象 | ST-LSM (STFP-R4) vs TMan (TR-index + tiered compaction) |
| 数据集 | 统一数据集（见上） |
| 查询窗口 | 9个标准窗口 |
| 部署位置 | TMan: 服务器, ST-LSM: 本地 |
| 指标 | Read吞吐(MB/s), Write吞吐(MB/s), 写放大, P50/P99延迟 |
| 输出 | `Result/comparison/tman_vs_stlsm.tsv` |

**需要准备**：
- [ ] 数据上传到TMan服务器
- [ ] 统一查询窗口CSV发给TMan侧
- [ ] 约定重复次数和报告格式

### B. 消融实验：三层独立验证

| 实验 | 消融维度 | 变体 | 指标 |
|------|---------|------|------|
| B1-Global | st_file_meta裁剪 | no-file vs with-file | files_skipped, wall_time |
| B2-Local | block/key层裁剪 | file-only vs file+block vs file+block+key | data_blocks_read, keys_examined, wall_time |
| B3-Local | 前截断suffix-max | suffix-off vs suffix-on | index_entries_examined, suffix_max_stops |
| B4-Compaction | 策略对比 | 7种策略 | read_mbs, write_mbs, write_amp |

| 输出 | 路径 |
|------|------|
| B1 | `Result/ablation/global.tsv` |
| B2 | `Result/ablation/local.tsv` |
| B3 | `Result/ablation/cutoff.tsv` |
| B4 | `Result/ablation/compaction.tsv` |

### C. 参数实验

| 实验 | 参数 | 变体 | 指标 |
|------|------|------|------|
| C1-max_runs | max_runs | R=2, R=4, R=6, R=inf | read_mbs, write_mbs, write_amp |
| C2-file_size | target_file_size | 16MB, 32MB, 64MB, 128MB | read_mbs, compaction_freq |
| C3-SST_pyramid | L1 key cap | 5000, 10000, 20000 | read_mbs, runs_per_level |

| 输出 | 路径 |
|------|------|
| C1 | `Result/params/max_runs.tsv` |
| C2 | `Result/params/file_size.tsv` |
| C3 | `Result/params/sst_pyramid.tsv` |

---

## 三、目录规范

```
Result/
├── comparison/           # A. 对比实验
│   └── tman_vs_stlsm.tsv
├── ablation/             # B. 消融实验
│   ├── global.tsv
│   ├── local.tsv
│   ├── cutoff.tsv
│   └── compaction.tsv
├── params/               # C. 参数实验
│   ├── max_runs.tsv
│   ├── file_size.tsv
│   └── sst_pyramid.tsv
└── picture/              # 图表
    ├── comparison/
    ├── ablation/
    └── params/

data/
├── processed/
│   └── unified/
│       ├── segments_meta.csv        (284K条)
│       ├── segments_points.csv      (80M点)
│       └── unified_stratified12_st_windows.csv (9个标准窗口)
└── shared/                          # TMan共享
    └── README.md                    # 数据格式说明
```

---

## 四、待办

- [ ] TMan 服务器数据上传
- [ ] 统一查询窗口发给 TMan
- [ ] 约定指标格式（TSV列名）
- [ ] 约定重复次数（建议 ≥ 5次，trimmed mean）
- [ ] 约定报告时间
