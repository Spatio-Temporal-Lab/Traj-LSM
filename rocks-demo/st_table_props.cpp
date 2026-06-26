// Per-SST TableProperties: data blocks, sizes, avg KV/block (read-only ST DB).

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

namespace {

#ifdef _WIN32
#include <stdlib.h>
static void ClearLegacyManifestShardEnv() {
  _putenv_s("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE", "");
  _putenv_s("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC", "");
}
#else
static void ClearLegacyManifestShardEnv() {
  unsetenv("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE");
  unsetenv("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC");
}
#endif

rocksdb::Options StReadOptions() {
  rocksdb::Options o;
  o.create_if_missing = false;
  rocksdb::BlockBasedTableOptions tb;
  tb.experimental_spatio_temporal_meta_in_index_value = true;
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
  return o;
}

struct Row {
  std::string file;
  int level = -1;
  uint64_t file_size = 0;
  uint64_t entries = 0;
  uint64_t data_blocks = 0;
  uint64_t data_size = 0;
  uint64_t index_size = 0;
  uint64_t filter_size = 0;
  double avg_data_block_bytes = 0.0;
  double avg_kv_per_block = 0.0;
};

}  // namespace

int main(int argc, char** argv) {
  std::string db_path;
  std::string out_tsv;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--db" && i + 1 < argc) {
      db_path = argv[++i];
    } else if (a == "--out-tsv" && i + 1 < argc) {
      out_tsv = argv[++i];
    } else if (a == "-h" || a == "--help") {
      std::cout << "st_table_props --db <dir> [--out-tsv <path>]\n"
                   "  Read-only; prints per-SST block/KV stats from TableProperties.\n"
                   "  entries = RocksDB KV count (ST segment-related keys per block).\n";
      return 0;
    }
  }
  if (db_path.empty()) {
    std::cerr << "Missing --db\n";
    return 1;
  }

  ClearLegacyManifestShardEnv();
  rocksdb::Options opt = StReadOptions();
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status st = rocksdb::DB::OpenForReadOnly(opt, db_path, &db);
  if (!st.ok()) {
    std::cerr << "OpenForReadOnly: " << st.ToString() << "\n";
    return 2;
  }

  std::vector<rocksdb::LiveFileMetaData> live;
  db->GetLiveFilesMetaData(&live);

  rocksdb::TablePropertiesCollection props;
  st = db->GetPropertiesOfAllTables(&props);
  if (!st.ok()) {
    std::cerr << "GetPropertiesOfAllTables: " << st.ToString() << "\n";
    return 3;
  }

  std::vector<Row> rows;
  rows.reserve(props.size());
  uint64_t tot_entries = 0, tot_blocks = 0, tot_data = 0, tot_file = 0;
  uint64_t tot_index = 0, tot_filter = 0;

  for (const auto& kv : props) {
    const std::string& name = kv.first;
    const auto& p = kv.second;
    Row r;
    r.file = name;
    for (const auto& lf : live) {
      const std::string& fn =
          lf.relative_filename.empty() ? lf.name : lf.relative_filename;
      if (fn == name || fn.find(name) != std::string::npos ||
          name.find(fn) != std::string::npos) {
        r.level = lf.level;
        r.file_size = lf.size;
        break;
      }
    }
    r.entries = p->num_entries;
    r.data_blocks = p->num_data_blocks;
    r.data_size = p->data_size;
    r.index_size = p->index_size;
    r.filter_size = p->filter_size;
    r.avg_data_block_bytes =
        r.data_blocks == 0 ? 0.0
                           : static_cast<double>(r.data_size) /
                                 static_cast<double>(r.data_blocks);
    r.avg_kv_per_block =
        r.data_blocks == 0 ? 0.0
                           : static_cast<double>(r.entries) /
                                 static_cast<double>(r.data_blocks);
    tot_entries += r.entries;
    tot_blocks += r.data_blocks;
    tot_data += r.data_size;
    tot_file += r.file_size;
    tot_index += r.index_size;
    tot_filter += r.filter_size;
    rows.push_back(std::move(r));
  }

  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.file < b.file; });

  std::ostream* out = &std::cout;
  std::ofstream f;
  if (!out_tsv.empty()) {
    f.open(out_tsv, std::ios::binary);
    if (!f) {
      std::cerr << "Cannot write " << out_tsv << "\n";
      return 4;
    }
    out = &f;
  }

  (*out) << "file\tlevel\tfile_bytes\tentries\tdata_blocks\tdata_bytes\t"
            "index_bytes\tfilter_bytes\tavg_data_block_bytes\tavg_kv_per_block\n";
  (*out) << std::fixed << std::setprecision(2);
  for (const Row& r : rows) {
    (*out) << r.file << "\t" << r.level << "\t" << r.file_size << "\t"
           << r.entries << "\t" << r.data_blocks << "\t" << r.data_size << "\t"
           << r.index_size << "\t" << r.filter_size << "\t"
           << r.avg_data_block_bytes << "\t" << r.avg_kv_per_block << "\n";
  }
  const double g_avg_b =
      tot_blocks == 0 ? 0.0 : static_cast<double>(tot_data) / tot_blocks;
  const double g_avg_kv =
      tot_blocks == 0 ? 0.0 : static_cast<double>(tot_entries) / tot_blocks;
  (*out) << "TOTAL\t\t" << tot_file << "\t" << tot_entries << "\t" << tot_blocks
         << "\t" << tot_data << "\t" << tot_index << "\t" << tot_filter << "\t"
         << g_avg_b << "\t" << g_avg_kv << "\n";

  std::cout << "\n# summary n_sst=" << rows.size() << " configured_data_block_size="
            << rocksdb::BlockBasedTableOptions().block_size
            << " (rocksdb default if not overridden at build)\n";
  return 0;
}
