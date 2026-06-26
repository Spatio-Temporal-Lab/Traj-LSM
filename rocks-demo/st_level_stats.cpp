// Print live SST counts/sizes per LSM level by replaying M_manager MANIFEST shards.

#include <iostream>
#include <string>

#include <rocksdb/env.h>

#include "db/manifest_ops.h"

int main(int argc, char** argv) {
  std::string db_path;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--db" && i + 1 < argc) {
      db_path = argv[++i];
    } else if (std::string(argv[i]) == "-h" || std::string(argv[i]) == "--help") {
      std::cout << "st_level_stats --db <rocksdb_dir>\n";
      return 0;
    }
  }
  if (db_path.empty()) {
    std::cerr << "Required: --db <dir>\n";
    return 1;
  }

  rocksdb::FileSystem* fs = rocksdb::Env::Default()->GetFileSystem().get();
  rocksdb::StLiveSstLevelStats stats;
  const rocksdb::Status s =
      rocksdb::GetLiveSstLevelStatsFromMManager(db_path, fs, false, &stats);
  if (!s.ok()) {
    std::cerr << "GetLiveSstLevelStatsFromMManager: " << s.ToString() << "\n";
    return 1;
  }

  std::cout << "db=" << db_path << "\n";
  std::cout << "m_manager_shards=" << stats.m_manager_shards << "\n";
  std::cout << "disk_sst_files=" << stats.disk_sst_files << "\n";
  std::cout << "unique_live_sst(on_disk)=" << stats.unique_live_files << "\n";
  std::cout << "manifest_refs_not_on_disk=" << stats.manifest_refs_not_on_disk
            << " disk_sst_without_manifest_level="
            << stats.disk_sst_without_manifest_level << "\n";
  std::cout << "with_st_file_meta=" << stats.with_st_file_meta
            << " without_st_file_meta=" << stats.without_st_file_meta << "\n";
  std::cout << "per_level:";
  uint64_t total_bytes = 0;
  for (const auto& row : stats.per_level) {
    std::cout << " L" << row.level << "=" << row.file_count;
    total_bytes += row.total_bytes;
  }
  std::cout << "\n";
  std::cout << "detail:\n";
  for (const auto& row : stats.per_level) {
    std::cout << "  L" << row.level << " files=" << row.file_count
              << " bytes=" << row.total_bytes << " entries=" << row.total_entries;
    if (row.file_count > 0) {
      std::cout << " avg_bytes=" << (row.total_bytes / row.file_count);
    }
    std::cout << "\n";
  }
  std::cout << "total_bytes=" << total_bytes << " ("
            << (total_bytes / 1024.0 / 1024.0) << " MiB)\n";
  return 0;
}
