// Best-effort in-place RocksDB::RepairDB for ST trajectory DB dirs.
//
// Repairs manifest from SST (+ WAL replay) matching traj_db_audit table options.
// Missing SST entries are dropped → possible key loss / layout reset (often all L0).
//
// ALWAYS copy the DB first if you cannot afford rollback:
//   robocopy <db> <db>_pre_repair /E /COPY:DAT /DCOPY:T
//
// Usage:
//   traj_db_repair --db D:\...\wuxi_manifest_route_full_db

#include <cstring>
#include <iostream>
#include <string>

#include <rocksdb/db.h>
#include <rocksdb/table.h>

namespace {

rocksdb::Options OptionsForRepair() {
  rocksdb::Options o;
  o.create_if_missing = false;
  rocksdb::BlockBasedTableOptions tb;
  tb.experimental_spatio_temporal_meta_in_index_value = true;
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
      std::cout << "traj_db_repair --db <dir>\n"
                   "In-place RocksDB RepairDB using ST-compatible table_factory.\n"
                   "WARN: SST files missing from disk are discarded; WAL is replayed best-effort.\n";
      return 0;
    }
  }
  if (db_path.empty()) {
    std::cerr << "Required: --db <dir>\n";
    return 1;
  }
  rocksdb::Status s = rocksdb::RepairDB(db_path, OptionsForRepair());
  if (!s.ok()) {
    std::cerr << "RepairDB failed: " << s.ToString() << "\n";
    return 2;
  }
  std::cout << "RepairDB: OK (" << db_path << ")\n";
  std::cout << "Next: traj_db_audit --db " << db_path << "\n";
  return 0;
}
