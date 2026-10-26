#include "segment_value_codec.hpp"
#include "table/st_meta_user_key.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <rocksdb/db.h>
#include <rocksdb/table.h>
#include <string>

void ok(const rocksdb::Status &s) {
  if (!s.ok()) {
    std::cerr << s.ToString() << std::endl;
    std::exit(2);
  }
}
std::string key(unsigned t, unsigned id) {
  std::string k(27, '\0');
  k[0] = char(0xe7);
  unsigned b = t / 3600;
  for (int i = 0; i < 4; ++i)
    k[1 + i] = char(b >> (24 - 8 * i));
  k[24] = char(id);
  return k;
}
std::string value(unsigned start, unsigned end, float lo = 120, float hi = 120,
                  float point = 120) {
  segval::SegmentValueHeader h;
  h.t_start = start;
  h.t_end = end;
  h.x_min = lo;
  h.x_max = hi;
  h.y_min = h.y_max = 30;
  h.point_count = 2;
  return segval::EncodeSegmentValueV2(h,
                                      {{start, point, 30}, {end, point, 30}});
}
void setting(const char *name, const char *value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}
void expect(const char *name, int actual, int expected) {
  std::cout << name << "=" << actual << " expected=" << expected << std::endl;
  if (actual != expected)
    std::exit(3);
}

int scan(rocksdb::DB *db, unsigned start, unsigned end, bool prune,
         bool lower = false, float xlo = 119, float xhi = 121,
         uint64_t* examined = nullptr) {
  rocksdb::ReadOptions ro;
  auto &q = ro.experimental_st_prune_scan;
  q.enable = prune;
  q.block_level_index_entries_examined = examined;
  q.file_level_enable = false;
  q.block_level_enable = true;
  q.key_level_enable = true;
  q.value_level_point_truth_enable = true;
  q.t_min = start;
  q.t_max = end;
  q.x_min = xlo;
  q.x_max = xhi;
  q.y_min = 29;
  q.y_max = 31;
  q.event_time_bucket_width_sec = 3600;
  q.event_time_bucket_lookback = 0;
  q.block_level_index_seek_lower_bound_enable = lower;
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next())
    ++n;
  ok(it->status());
  return n;
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  std::filesystem::create_directories(argv[1]);
  setting("ROCKSDB_EXPERIMENTAL_MANIFEST_SET_WRITE", "0");
  setting("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE", "0");
  setting("ROCKSDB_ST_USER_KEY_SCHEMA", "e7");
  rocksdb::Options o;
  o.create_if_missing = true;
  o.error_if_exists = true;
  o.disable_auto_compactions = true;
  o.compression = rocksdb::kNoCompression;
  rocksdb::BlockBasedTableOptions tb;
  tb.experimental_spatio_temporal_meta_in_index_value = true;
  tb.block_size = 64;
  o.experimental_st_block_mbr_flush_enable = false;
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
  const unsigned t = 1600000000;
  std::unique_ptr<rocksdb::DB> db;
  ok(rocksdb::DB::Open(o, std::string(argv[1]) + "/offset", &db));
  ok(db->Put({}, key(t, 1), value(t, t)));
  ok(db->Put({}, key(t + 3600, 2), value(t + 3600, t + 3600)));
  ok(db->Put({}, key(t + 7200, 3), value(t + 7200, t + 7200)));
  ok(db->Flush(rocksdb::FlushOptions()));
  setting("ST_DISABLE_BACKWARD_TRUNCATION", "0");
  expect("offset_baseline", scan(db.get(), t, t + 1, false), 3);
  expect("offset_prune", scan(db.get(), t, t + 1, true), 1);
  setting("ST_DISABLE_BACKWARD_TRUNCATION", "1");
  expect("offset_disabled", scan(db.get(), t, t + 1, true), 1);
  rocksdb::ReadOptions repeated;
  auto &q = repeated.experimental_st_prune_scan;
  q.enable = true;
  q.t_min = t;
  q.t_max = t + 1;
  q.x_min = 119;
  q.x_max = 121;
  q.y_min = 29;
  q.y_max = 31;
  for (int i = 0; i < 2; ++i) {
    std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(repeated));
    int count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next())
      ++count;
    ok(it->status());
    expect("reuse_read_options", count, 1);
    expect("caller_filter_unchanged", q.manifest_sst_filter == nullptr, 1);
  }
  db.reset();
  ok(rocksdb::DB::Open(o, std::string(argv[1]) + "/late", &db));
  ok(db->Put({}, key(t, 1), value(t, t + 86400 * 100)));
  ok(db->Flush(rocksdb::FlushOptions()));
  expect("late_full_index",
         scan(db.get(), t + 86400 * 100, t + 86400 * 100 + 1, true, false), 1);
  expect("late_lower_bound",
         scan(db.get(), t + 86400 * 100, t + 86400 * 100 + 1, true, true), 1);
  db.reset();
  ok(rocksdb::DB::Open(o, std::string(argv[1]) + "/bounds", &db));
  float bound = 120.27959442138672f, point = 120.27960205078125f;
  ok(db->Put({}, key(t, 1), value(t, t, 120.2f, bound, point)));
  ok(db->Flush(rocksdb::FlushOptions()));
  expect("bounds_baseline", scan(db.get(), t, t, false, false, point, point),
         1);
  expect("bounds_prune", scan(db.get(), t, t, true, false, point, point), 1);
  db.reset();
  ok(rocksdb::DB::Open(o, std::string(argv[1]) + "/prefix", &db));
  for (unsigned i = 0; i < 128; ++i) {
    ok(db->Put({}, key(t + i * 3600, 1),
               value(t + i * 3600, t + i * 3600)));
  }
  ok(db->Flush(rocksdb::FlushOptions()));
  uint64_t linear = 0, binary = 0;
  setting("ST_DISABLE_FORWARD_TRUNCATION", "1");
  expect("prefix_linear", scan(db.get(), t + 120 * 3600,
         t + 120 * 3600, true, false, 119, 121, &linear), 1);
  setting("ST_DISABLE_FORWARD_TRUNCATION", "0");
  expect("prefix_binary", scan(db.get(), t + 120 * 3600,
         t + 120 * 3600, true, false, 119, 121, &binary), 1);
  expect("prefix_checks_reduced", binary < linear, 1);
  unsigned decoded = 0;
  std::string daily = key(3600 * 18000, 1);
  expect("daily_decode_ok",
         rocksdb::TryDecodeSpatioTemporalUserKeyTime(daily, &decoded, 86400),
         1);
  expect("daily_bucket_width", decoded == 18000u * 86400u, 1);
  std::cout << "PASS ST design regressions" << std::endl;
  return 0;
}
