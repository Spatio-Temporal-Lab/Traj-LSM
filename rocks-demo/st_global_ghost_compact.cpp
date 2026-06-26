// Logical compaction for Global Master/Ghost mode.
//
// This tool scans the DB for ghost segment values (V3 with kSegmentValueFlagGhost),
// probes their master_key, and deletes dangling / inconsistent ghosts.
//
// It is intentionally conservative: it never deletes masters and it does not
// attempt to "merge" ghosts because the current stage stores ghosts as distinct
// keys (their identity includes bucket via StSegmentGhostId at ingestion time).
//
// Usage:
//   st_global_ghost_compact --db <path> [--dry-run]
//       [--max-probes N] [--max-deletes N] [--verify-meta]

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include "segment_value_codec.hpp"
#include "segment_value_global.hpp"

namespace {

void Logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  std::fprintf(stdout, "\n");
  std::fflush(stdout);
  va_end(ap);
}

bool LooksLikeStSegmentKey(const rocksdb::Slice& user_key) {
  if (user_key.size() < 1) {
    return false;
  }
  const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
  return magic == 0xE7;
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_path;
  bool dry_run = false;
  bool verify_meta = false;
  bool fail_on_issues = false;
  uint64_t max_probes = 1'000'000;
  uint64_t max_deletes = 100'000;
  uint64_t simulate_delete_masters = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--db" && i + 1 < argc) {
      db_path = argv[++i];
    } else if (a == "--dry-run") {
      dry_run = true;
    } else if (a == "--verify-meta") {
      verify_meta = true;
    } else if (a == "--fail-on-issues") {
      fail_on_issues = true;
    } else if (a == "--max-probes" && i + 1 < argc) {
      max_probes = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (a == "--max-deletes" && i + 1 < argc) {
      max_deletes =
          static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (a == "--simulate-delete-masters" && i + 1 < argc) {
      simulate_delete_masters =
          static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (a == "-h" || a == "--help") {
      std::cout
          << "st_global_ghost_compact --db <path> [--dry-run] [--verify-meta]\n"
             "    [--fail-on-issues]\n"
             "    [--max-probes N] [--max-deletes N] [--simulate-delete-masters N]\n"
             "  --dry-run: do not delete, only report.\n"
             "  --verify-meta: if master exists but meta mismatches, delete ghost.\n";
      return 0;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      return 1;
    }
  }

  if (db_path.empty()) {
    std::cerr << "Required: --db <path>\n";
    return 1;
  }

  rocksdb::Options opt;
  opt.create_if_missing = false;
  // IMPORTANT: open in "raw" mode (no GlobalMaterializingIterator), so we can
  // actually observe ghost KVs and delete them.
  opt.experimental_global_master_ghost = false;
  opt.experimental_global_verify_ghost = false;
  // Match the ingest path used by rocks-demo tools (BlockBasedTable + ST tails).
  rocksdb::BlockBasedTableOptions tb;
  tb.experimental_spatio_temporal_meta_in_index_value = true;
  opt.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));

  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status s = rocksdb::DB::Open(opt, db_path, &db);
  if (!s.ok()) {
    std::cerr << "DB::Open failed: " << s.ToString() << "\n";
    return 2;
  }

  rocksdb::ReadOptions ro;
  ro.total_order_seek = true;
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));

  if (!dry_run && simulate_delete_masters > 0) {
    uint64_t deleted = 0;
    for (it->SeekToFirst(); it->Valid() && deleted < simulate_delete_masters;
         it->Next()) {
      const rocksdb::Slice uk = it->key();
      const rocksdb::Slice val = it->value();
      if (!LooksLikeStSegmentKey(uk)) {
        continue;
      }
      segval::DecodedSegmentValue decoded{};
      if (!segval::DecodeSegmentValue(std::string(val.data(), val.size()),
                                      &decoded)) {
        continue;
      }
      if (decoded.is_ghost) {
        continue;
      }
      rocksdb::Status ds = db->Delete(rocksdb::WriteOptions(), uk);
      if (ds.ok()) {
        ++deleted;
      }
    }
    if (!it->status().ok()) {
      std::cerr << "Iterator error (simulate-delete-masters): "
                << it->status().ToString() << "\n";
      return 3;
    }
    Logf("simulate-delete-masters done deleted=%" PRIu64, deleted);
    it.reset(db->NewIterator(ro));
  }

  uint64_t seen = 0;
  uint64_t ghosts = 0;
  uint64_t probes = 0;
  uint64_t delete_dangling = 0;
  uint64_t delete_meta_mismatch = 0;
  uint64_t delete_master_is_ghost = 0;
  uint64_t delete_decode_failed = 0;

  Logf("scan start db=%s dry_run=%d verify_meta=%d max_probes=%" PRIu64
       " max_deletes=%" PRIu64,
       db_path.c_str(), static_cast<int>(dry_run), static_cast<int>(verify_meta),
       max_probes, max_deletes);

  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ++seen;
    const rocksdb::Slice uk = it->key();
    const rocksdb::Slice val = it->value();
    if (!LooksLikeStSegmentKey(uk)) {
      continue;
    }

    segval::DecodedSegmentValue decoded{};
    if (!segval::DecodeSegmentValue(std::string(val.data(), val.size()),
                                    &decoded)) {
      continue;
    }
    if (!decoded.is_ghost) {
      continue;
    }
    ++ghosts;
    if (probes >= max_probes) {
      break;
    }

    ++probes;
    segval::MaterializedSegmentValue mat{};
    if (!segval::DecodeAndMaterializeSegmentValue(db.get(), ro, uk, val, &mat)) {
      // Could not decode ghost itself; safest remediation is delete the entry.
      ++delete_decode_failed;
      if (!dry_run && (delete_dangling + delete_meta_mismatch +
                           delete_master_is_ghost + delete_decode_failed) <=
                          max_deletes) {
        rocksdb::Status ds = db->Delete(rocksdb::WriteOptions(), uk);
        (void)ds;
      }
      continue;
    }

    if (!mat.was_ghost) {
      continue;
    }

    // Dangling: master not found.
    if (mat.master_lookup_attempted && !mat.master_lookup_ok) {
      ++delete_dangling;
      if (!dry_run &&
          (delete_dangling + delete_meta_mismatch + delete_master_is_ghost +
           delete_decode_failed) <= max_deletes) {
        rocksdb::Status ds = db->Delete(rocksdb::WriteOptions(), uk);
        (void)ds;
      }
      continue;
    }

    // master exists but is also a ghost (corruption / double indirection).
    if (mat.master_is_ghost) {
      ++delete_master_is_ghost;
      if (!dry_run &&
          (delete_dangling + delete_meta_mismatch + delete_master_is_ghost +
           delete_decode_failed) <= max_deletes) {
        rocksdb::Status ds = db->Delete(rocksdb::WriteOptions(), uk);
        (void)ds;
      }
      continue;
    }

    if (verify_meta && mat.master_meta_mismatch) {
      ++delete_meta_mismatch;
      if (!dry_run &&
          (delete_dangling + delete_meta_mismatch + delete_master_is_ghost +
           delete_decode_failed) <= max_deletes) {
        rocksdb::Status ds = db->Delete(rocksdb::WriteOptions(), uk);
        (void)ds;
      }
      continue;
    }
  }

  if (!it->status().ok()) {
    std::cerr << "Iterator error: " << it->status().ToString() << "\n";
    return 3;
  }

  Logf("scan done seen=%" PRIu64 " ghosts=%" PRIu64 " probes=%" PRIu64
       " delete_dangling=%" PRIu64 " delete_meta_mismatch=%" PRIu64
       " delete_master_is_ghost=%" PRIu64 " delete_decode_failed=%" PRIu64,
       seen, ghosts, probes, delete_dangling, delete_meta_mismatch,
       delete_master_is_ghost, delete_decode_failed);

  const uint64_t issues =
      delete_dangling + delete_meta_mismatch + delete_master_is_ghost +
      delete_decode_failed;
  if (fail_on_issues && issues > 0) {
    std::fprintf(stderr,
                 "FAIL: detected issues=%" PRIu64
                 " (use without --dry-run to delete, or inspect counts)\n",
                 issues);
    return 10;
  }

  if (!dry_run) {
    rocksdb::FlushOptions fo;
    fo.wait = true;
    rocksdb::Status fs = db->Flush(fo);
    if (!fs.ok()) {
      std::cerr << "Flush after deletes failed: " << fs.ToString() << "\n";
      return 4;
    }
  }

  return 0;
}

