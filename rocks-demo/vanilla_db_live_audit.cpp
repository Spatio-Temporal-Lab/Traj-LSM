// Open a **upstream / plain BlockBasedTable** RocksDB (no fork MANIFEST_SET),
// VerifyChecksum, print GetLiveFilesMetaData (level, size, entries), sums.
//
//   vanilla_db_live_audit --db D:\path\to\vanilla_replica

#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/metadata.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

namespace {

rocksdb::Options OpenVanillaReadOptions() {
  rocksdb::Options o;
  o.create_if_missing = false;
  o.disable_auto_compactions = true;
  o.persist_user_defined_timestamps = false;
  rocksdb::BlockBasedTableOptions tb;
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_path;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--db" && i + 1 < argc) {
      db_path = argv[++i];
    } else if (std::string(argv[i]) == "-h" ||
               std::string(argv[i]) == "--help") {
      std::cout
          << "vanilla_db_live_audit --db <dir>\n"
             "  Uses upstream RocksDB (plain SST). For fork DB dirs use traj_db_audit.\n";
      return 0;
    }
  }
  if (db_path.empty()) {
    std::cerr << "Required: --db <dir>\n";
    return 1;
  }

  rocksdb::Options opt = OpenVanillaReadOptions();
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status s = rocksdb::DB::Open(opt, db_path, &db);
  if (!s.ok()) {
    std::cerr << "DB::Open: " << s.ToString() << "\n";
    return 1;
  }

  std::cout << "VerifyChecksum...\n";
  s = db->VerifyChecksum();
  if (!s.ok()) {
    std::cerr << "VerifyChecksum FAILED: " << s.ToString() << "\n";
    return 2;
  }
  std::cout << "VerifyChecksum: OK\n";

  std::vector<rocksdb::LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);

  std::array<int, 12> per_level{};
  uint64_t sum_entries = 0;
  uint64_t sum_size = 0;
  std::cout << "live_sst count=" << files.size() << "\n";
  for (const auto& f : files) {
    const std::string& fn =
        f.relative_filename.empty() ? f.name : f.relative_filename;
    int lv = f.level;
    if (lv >= 0 && lv < static_cast<int>(per_level.size())) {
      per_level[static_cast<size_t>(lv)]++;
    }
    std::cout << "  L" << f.level << " " << fn << " size=" << f.size
              << " num_entries=" << f.num_entries << " del=" << f.num_deletions
              << "\n";
    sum_entries += f.num_entries;
    sum_size += f.size;
  }
  std::cout << "sum_num_entries=" << sum_entries << " sum_file_size=" << sum_size
            << "\n";
  std::cout << "sst_count_per_level:";
  for (size_t i = 0; i < per_level.size(); ++i) {
    if (per_level[i] > 0) {
      std::cout << " L" << i << "=" << per_level[i];
    }
  }
  std::cout << "\n";
  return 0;
}
