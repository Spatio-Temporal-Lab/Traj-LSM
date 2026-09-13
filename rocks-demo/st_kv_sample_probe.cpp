// Dump sample user keys from ST fork DB (tman_st / E7) for inspection.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include "st_tman_st_key.hpp"

namespace {

// Fork DB::Iterator may return user keys without the 8-byte internal footer.
rocksdb::Slice UserKey(const rocksdb::Slice& ikey) {
  constexpr size_t kFooter = 8;
  if (ikey.size() <= kFooter) {
    return ikey;
  }
  const unsigned char m = static_cast<unsigned char>(ikey.data()[0]);
  if (m == 0xE7 && ikey.size() >= 27 && ikey.size() <= 32) {
    return ikey;
  }
  if (m != 0xE5 && m != 0xE6 && m != 0xE7 && ikey.size() >= 11 &&
      ikey.size() <= 64) {
    return ikey;
  }
  return rocksdb::Slice(ikey.data(), ikey.size() - kFooter);
}

void HexDump(const rocksdb::Slice& s, size_t max_bytes) {
  const size_t n = std::min(s.size(), max_bytes);
  for (size_t i = 0; i < n; ++i) {
    std::printf("%02x", static_cast<unsigned char>(s.data()[i]));
    if (i + 1 < n) {
      std::printf(" ");
    }
  }
  if (s.size() > max_bytes) {
    std::printf(" ...");
  }
}

void PrintTmanStKey(const rocksdb::Slice& uk) {
  if (!st_tman::IsTmanStRowKeyLayout(uk.data(), uk.size())) {
    std::cout << "  (not recognized as tman_st layout)\n";
    return;
  }
  const int16_t period = static_cast<int16_t>(
      (static_cast<uint16_t>(static_cast<unsigned char>(uk.data()[0])) << 8) |
      static_cast<unsigned char>(uk.data()[1]));
  const uint64_t spatial =
      (static_cast<uint64_t>(static_cast<unsigned char>(uk.data()[2])) << 56) |
      (static_cast<uint64_t>(static_cast<unsigned char>(uk.data()[3])) << 48) |
      (static_cast<uint64_t>(static_cast<unsigned char>(uk.data()[4])) << 40) |
      (static_cast<uint64_t>(static_cast<unsigned char>(uk.data()[5])) << 32) |
      (static_cast<uint64_t>(static_cast<unsigned char>(uk.data()[6])) << 24) |
      (static_cast<uint64_t>(static_cast<unsigned char>(uk.data()[7])) << 16) |
      (static_cast<uint64_t>(static_cast<unsigned char>(uk.data()[8])) << 8) |
      static_cast<uint64_t>(static_cast<unsigned char>(uk.data()[9]));
  const std::string tid(uk.data() + 10, uk.size() - 10);
  std::cout << "  layout: TMan-ST [period BE][spatial BE][tid]\n";
  std::cout << "  period=" << period << " spatial_index=" << spatial
            << " tid=\"" << tid << "\" key_len=" << uk.size() << "\n";
}

void PrintLevels(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf) {
  rocksdb::ColumnFamilyMetaData meta;
  db->GetColumnFamilyMetaData(cf, &meta);
  std::cout << "Live files (DB::GetColumnFamilyMetaData):\n";
  for (const auto& lv : meta.levels) {
    std::cout << "  L" << lv.level << " n=" << lv.files.size()
              << " bytes=" << lv.size << "\n";
    for (size_t i = 0; i < lv.files.size() && i < 5; ++i) {
      std::cout << "    " << lv.files[i].name << " entries="
                << lv.files[i].num_entries << "\n";
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_path;
  size_t limit = 5;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--db" && i + 1 < argc) {
      db_path = argv[++i];
    } else if (a == "--limit" && i + 1 < argc) {
      limit = static_cast<size_t>(std::stoul(argv[++i]));
    } else if (a == "-h" || a == "--help") {
      std::cout << "st_kv_sample_probe --db <dir> [--limit N]\n";
      return 0;
    }
  }
  if (db_path.empty()) {
    std::cerr << "Required: --db\n";
    return 1;
  }

  rocksdb::Options opts;
  opts.create_if_missing = false;
  rocksdb::BlockBasedTableOptions tb;
  tb.experimental_spatio_temporal_meta_in_index_value = true;
  opts.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));

  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status s = rocksdb::DB::OpenForReadOnly(opts, db_path, &db, false);
  if (!s.ok()) {
    std::cerr << "OpenForReadOnly: " << s.ToString() << "\n";
    return 1;
  }
  rocksdb::ColumnFamilyHandle* cf = db->DefaultColumnFamily();

  PrintLevels(db.get(), cf);

  {
    rocksdb::ReadOptions ro0;
    ro0.total_order_seek = true;
    std::unique_ptr<rocksdb::Iterator> it0(db->NewIterator(ro0, cf));
    it0->SeekToFirst();
    if (it0->Valid()) {
      const rocksdb::Slice raw = it0->key();
      const rocksdb::Slice uk = UserKey(raw);
      std::cout << "First record: internal_key_len=" << raw.size()
                << " user_key_len(strip8)=" << uk.size() << "\n";
      std::cout << "  raw_hex: ";
      HexDump(raw, 48);
      std::cout << "\n  as_user_key (no strip): ";
      HexDump(raw, 48);
      std::cout << "\n";
    }
  }

  rocksdb::ReadOptions ro;
  ro.total_order_seek = true;
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro, cf));
  size_t n = 0;
  size_t len_hist[64] = {};
  size_t e7 = 0, tman = 0, short_k = 0, other = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const rocksdb::Slice uk = UserKey(it->key());
    if (uk.size() < sizeof(len_hist) / sizeof(len_hist[0])) {
      ++len_hist[uk.size()];
    }
    const unsigned char m =
        uk.size() > 0 ? static_cast<unsigned char>(uk.data()[0]) : 0;
    if (m == 0xE7) {
      ++e7;
    } else if (st_tman::IsTmanStRowKeyLayout(uk.data(), uk.size())) {
      ++tman;
    } else if (uk.size() < 11) {
      ++short_k;
    } else {
      ++other;
    }
  }
  std::cout << "\nKey census (full scan):\n";
  std::cout << "  E7=" << e7 << " tman_st=" << tman << " short(<11)=" << short_k
            << " other=" << other << "\n";
  for (size_t i = 0; i < 40; ++i) {
    if (len_hist[i] > 0) {
      std::cout << "  len=" << i << " count=" << len_hist[i] << "\n";
    }
  }

  // Longest tman_st-looking keys (tid should be like "3644_1").
  size_t best_len = 0;
  std::string best_hex;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const rocksdb::Slice uk = UserKey(it->key());
    if (!st_tman::IsTmanStRowKeyLayout(uk.data(), uk.size()) || uk.size() <= best_len) {
      continue;
    }
    best_len = uk.size();
    best_hex.assign(uk.data(), uk.size());
  }
  if (best_len > 0) {
    std::cout << "\n--- longest tman_st key (len=" << best_len << ") ---\n";
    std::cout << "user_key_hex: ";
    HexDump(rocksdb::Slice(best_hex.data(), best_hex.size()), 96);
    std::cout << "\n";
    PrintTmanStKey(rocksdb::Slice(best_hex.data(), best_hex.size()));
  }

  // First E7 or tman_st key for display.
  n = 0;
  for (it->SeekToFirst(); it->Valid() && n < limit; it->Next()) {
    const rocksdb::Slice uk = UserKey(it->key());
    const unsigned char m =
        uk.size() > 0 ? static_cast<unsigned char>(uk.data()[0]) : 0;
    if (m != 0xE7 && !st_tman::IsTmanStRowKeyLayout(uk.data(), uk.size())) {
      continue;
    }
    if (uk.size() < 14) {
      continue;
    }
    std::cout << "\n--- segment KV #" << (n + 1) << " ---\n";
    std::cout << "user_key_len=" << uk.size() << "\n";
    std::cout << "user_key_hex: ";
    HexDump(uk, 80);
    std::cout << "\n";
    if (m == 0xE7) {
      std::cout << "  layout: E7 (Mind.md segment key)\n";
    } else {
      PrintTmanStKey(uk);
    }
    std::cout << "value_len=" << it->value().size() << "\n";
    ++n;
  }

  std::cout << "\nDone. segment_samples=" << n << "\n";
  return 0;
}
