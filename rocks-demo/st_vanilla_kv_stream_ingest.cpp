// Upstream (facebook) RocksDB only: create a new DB with plain BlockBasedTable,
// read framed KV stream from st_fork_kv_stream_dump, Put all keys, Flush + optional Compact.
//
// Usage: st_vanilla_kv_stream_ingest --db <new_empty_dir> --in <dump.kvs> [--compact]

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

#include "st_segment_key.hpp"

namespace {

rocksdb::CompressionType ParseCompression(const std::string& name, bool* ok) {
  if (ok != nullptr) {
    *ok = true;
  }
  if (name == "none") {
    return rocksdb::kNoCompression;
  }
  if (name == "snappy") {
    return rocksdb::kSnappyCompression;
  }
  if (name == "zstd") {
    return rocksdb::kZSTD;
  }
  if (name == "lz4") {
    return rocksdb::kLZ4Compression;
  }
  if (ok != nullptr) {
    *ok = false;
  }
  return rocksdb::kNoCompression;
}

rocksdb::Options VanillaIngestOptions(int write_buffer_mb,
                                      rocksdb::CompressionType compression,
                                      uint64_t block_size) {
  rocksdb::Options o;
  o.create_if_missing = true;
  // Default true in upstream RocksDB 11+ appends UDT bytes before seq+type in
  // internal keys; our dump is plain user keys and window scan strips 8 bytes.
  o.persist_user_defined_timestamps = false;
  if (write_buffer_mb > 0) {
    o.write_buffer_size = static_cast<size_t>(write_buffer_mb) * size_t{1048576};
  }
  o.compression = compression;
  rocksdb::BlockBasedTableOptions tb;
  if (block_size > 0) {
    tb.block_size = static_cast<size_t>(block_size);
  }
  // Intentionally NO experimental_spatio_temporal_meta — SSTs must be vanilla-native.
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
  return o;
}

template <typename OptionsT>
void MaybeApplyExperimentalGlobalOptions(
    OptionsT* opt, bool experimental_global_master_ghost,
    bool experimental_global_verify_ghost) {
  if (opt == nullptr) {
    return;
  }
  if constexpr (requires(OptionsT& o) { o.experimental_global_master_ghost; }) {
    opt->experimental_global_master_ghost = experimental_global_master_ghost;
  }
  if constexpr (requires(OptionsT& o) { o.experimental_global_verify_ghost; }) {
    opt->experimental_global_verify_ghost = experimental_global_verify_ghost;
  }
}

bool ReadU32(std::istream& in, uint32_t* out) {
  unsigned char b[4];
  in.read(reinterpret_cast<char*>(b), 4);
  if (in.gcount() != 4) {
    return false;
  }
  *out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
  return true;
}

constexpr char kMagic[8] = {'W', 'U', 'X', 'I', 'K', 'V', '0', '1'};
constexpr uint32_t kMaxRecord = 64u * 1024u * 1024u;

uint64_t Fnv1a64(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

bool ExtractStringField(const std::string& line, const std::string& key,
                        std::string* out) {
  const std::string needle = "\"" + key + "\"";
  size_t p = line.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = line.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  p = line.find('"', p + 1);
  if (p == std::string::npos) {
    return false;
  }
  const size_t q = line.find('"', p + 1);
  if (q == std::string::npos || q <= p + 1) {
    return false;
  }
  *out = line.substr(p + 1, q - p - 1);
  return true;
}

bool ExtractNumberField(const std::string& line, const std::string& key,
                        double* out) {
  const std::string needle = "\"" + key + "\"";
  size_t p = line.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = line.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  ++p;
  while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) {
    ++p;
  }
  size_t e = p;
  while (e < line.size()) {
    const char ch = line[e];
    if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+' ||
        ch == 'e' || ch == 'E') {
      ++e;
      continue;
    }
    break;
  }
  if (e <= p) {
    return false;
  }
  try {
    *out = std::stod(line.substr(p, e - p));
    return true;
  } catch (...) {
    return false;
  }
}

bool PutFromJsonl(rocksdb::DB* db, const std::string& jsonl_path,
                  const std::string& key_schema, uint64_t* out_written,
                  uint64_t* out_skipped) {
  if (db == nullptr || out_written == nullptr || out_skipped == nullptr) {
    return false;
  }
  std::ifstream in(jsonl_path);
  if (!in) {
    std::cerr << "Cannot open jsonl: " << jsonl_path << std::endl;
    return false;
  }
  rocksdb::WriteBatch batch;
  size_t batch_bytes = 0;
  const size_t kBatchFlush = size_t{4} * 1048576;
  const rocksdb::WriteOptions wopt;
  std::string line;
  uint64_t written = 0;
  uint64_t skipped = 0;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    std::string segment_id;
    double zorder_start_d = 0;
    double t_min_d = 0, t_max_d = 0;
    double x_min = 0, y_min = 0, x_max = 0, y_max = 0;
    if (!ExtractStringField(line, "segment_id", &segment_id) ||
        !ExtractNumberField(line, "t_min", &t_min_d) ||
        !ExtractNumberField(line, "t_max", &t_max_d) ||
        !ExtractNumberField(line, "zorder_start", &zorder_start_d)) {
      ++skipped;
      continue;
    }
    // For legacy E6 key schema, also need MBR fields.
    if (key_schema == "e6_mbr") {
      if (!ExtractNumberField(line, "min_lon", &x_min) ||
          !ExtractNumberField(line, "min_lat", &y_min) ||
          !ExtractNumberField(line, "max_lon", &x_max) ||
          !ExtractNumberField(line, "max_lat", &y_max)) {
        ++skipped;
        continue;
      }
    }
    if (t_min_d < 0 || t_max_d < 0) {
      ++skipped;
      continue;
    }
    const uint32_t t_min = static_cast<uint32_t>(t_min_d);
    const uint32_t t_max = static_cast<uint32_t>(t_max_d);
    const uint64_t id = Fnv1a64(segment_id);
    const uint64_t zorder_start = static_cast<uint64_t>(zorder_start_d);
    std::string key;
    if (key_schema == "e7_tzid") {
      key = StSegmentKeyTzid(t_min, t_max, zorder_start, id);
    } else {
      key = StSegmentKey(t_min, t_max, static_cast<float>(x_min),
                         static_cast<float>(y_min), static_cast<float>(x_max),
                         static_cast<float>(y_max), id);
    }
    // Keep full JSON line as value so later pipelines can parse points if needed.
    batch.Put(key, line);
    batch_bytes += key.size() + line.size() + 32;
    ++written;
    if (batch_bytes >= kBatchFlush) {
      rocksdb::Status st = db->Write(wopt, &batch);
      if (!st.ok()) {
        std::cerr << "Write failed: " << st.ToString() << std::endl;
        return false;
      }
      batch.Clear();
      batch_bytes = 0;
    }
  }
  if (batch.GetDataSize() > 0) {
    rocksdb::Status st = db->Write(wopt, &batch);
    if (!st.ok()) {
      std::cerr << "Final Write failed: " << st.ToString() << std::endl;
      return false;
    }
  }
  *out_written = written;
  *out_skipped = skipped;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_path;
  std::string in_path;
  std::string jsonl_path;
  bool do_compact = false;
  int write_buffer_mb = 256;
  std::string compression_name = "none";
  uint64_t block_size = 4096;
  std::string key_schema = "e6_mbr";
  bool experimental_global_master_ghost = false;
  bool experimental_global_verify_ghost = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--db" && i + 1 < argc) {
      db_path = argv[++i];
    } else if (a == "--in" && i + 1 < argc) {
      in_path = argv[++i];
    } else if (a == "--jsonl" && i + 1 < argc) {
      jsonl_path = argv[++i];
    } else if (a == "--compact") {
      do_compact = true;
    } else if (a == "--write-buffer-mb" && i + 1 < argc) {
      write_buffer_mb = std::atoi(argv[++i]);
    } else if (a == "--compression" && i + 1 < argc) {
      compression_name = argv[++i];
    } else if (a == "--block-size" && i + 1 < argc) {
      block_size = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (a == "--key-schema" && i + 1 < argc) {
      key_schema = argv[++i];
    } else if (a == "--experimental-global-master-ghost") {
      experimental_global_master_ghost = true;
    } else if (a == "--experimental-global-verify-ghost") {
      experimental_global_verify_ghost = true;
    } else if (a == "-h" || a == "--help") {
      std::cout
          << "st_vanilla_kv_stream_ingest --db <new_dir> (--in <dump.kvs> | --jsonl <segments.jsonl>) [--compact]\n"
             "  Builds a DB using **upstream** RocksDB only (plain SSTs).\n"
             "  --in mode expects WUXIKV01 from st_fork_kv_stream_dump.\n"
             "  --jsonl mode expects one segment object per line with fields:\n"
             "    segment_id, t_min, t_max, mbr.{min_lon,min_lat,max_lon,max_lat}\n"
             "  --compression {none|snappy|zstd|lz4} (default: none)\n"
             "  --block-size <bytes> (default: 4096)\n"
             "  --key-schema {e6_mbr|e7_tzid} (default: e6_mbr)\n"
             "  --experimental-global-master-ghost\n"
             "      Enable Global Master/Ghost manifest semantics in CF options.\n"
             "  --experimental-global-verify-ghost\n"
             "      Enable Global ghost consistency fail-fast checks.\n";
      return 0;
    }
  }

  if (db_path.empty() || (in_path.empty() == jsonl_path.empty())) {
    std::cerr << "Required: --db <dir> and exactly one of --in <dump.kvs> or --jsonl <segments.jsonl>\n";
    return 1;
  }

  if (experimental_global_verify_ghost && !experimental_global_master_ghost) {
    std::cerr << "--experimental-global-verify-ghost requires --experimental-global-master-ghost\n";
    return 1;
  }

  bool compression_ok = false;
  const rocksdb::CompressionType compression =
      ParseCompression(compression_name, &compression_ok);
  if (!compression_ok) {
    std::cerr << "Unknown --compression value: " << compression_name
              << " (expected none|snappy|zstd|lz4)\n";
    return 1;
  }
  rocksdb::Options opt =
      VanillaIngestOptions(write_buffer_mb, compression, block_size);
  MaybeApplyExperimentalGlobalOptions(
      &opt, experimental_global_master_ghost, experimental_global_verify_ghost);
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status st = rocksdb::DB::Open(opt, db_path, &db);
  if (!st.ok()) {
    std::cerr << "DB::Open(create): " << st.ToString() << std::endl;
    return 3;
  }

  uint64_t n = 0;
  uint64_t skipped = 0;
  if (!jsonl_path.empty()) {
    if (key_schema != "e6_mbr" && key_schema != "e7_tzid") {
      std::cerr << "Unknown --key-schema: " << key_schema
                << " (expected e6_mbr|e7_tzid)\n";
      return 1;
    }
    if (!PutFromJsonl(db.get(), jsonl_path, key_schema, &n, &skipped)) {
      return 4;
    }
  } else {
    std::ifstream in(in_path, std::ios::binary);
    if (!in) {
      std::cerr << "Cannot open input: " << in_path << std::endl;
      return 2;
    }
    char magic[8];
    in.read(magic, 8);
    if (in.gcount() != 8 || std::memcmp(magic, kMagic, 8) != 0) {
      std::cerr << "Bad magic (expected WUXIKV01)\n";
      return 2;
    }
    const rocksdb::WriteOptions wopt;
    std::string key_buf;
    std::string val_buf;
    rocksdb::WriteBatch batch;
    size_t batch_bytes = 0;
    const size_t kBatchFlush = size_t{4} * 1048576;
    while (true) {
      uint32_t kl = 0, vl = 0;
      if (!ReadU32(in, &kl)) {
        break;
      }
      if (!ReadU32(in, &vl)) {
        std::cerr << "Truncated header after " << n << " records\n";
        return 4;
      }
      if (kl > kMaxRecord || vl > kMaxRecord) {
        std::cerr << "Record too large at " << n << std::endl;
        return 4;
      }
      key_buf.resize(kl);
      val_buf.resize(vl);
      if (kl > 0) {
        in.read(key_buf.data(), static_cast<std::streamsize>(kl));
        if (in.gcount() != static_cast<std::streamsize>(kl)) {
          std::cerr << "Truncated key payload at record " << n << std::endl;
          return 4;
        }
      }
      if (vl > 0) {
        in.read(val_buf.data(), static_cast<std::streamsize>(vl));
        if (in.gcount() != static_cast<std::streamsize>(vl)) {
          std::cerr << "Truncated value payload at record " << n << std::endl;
          return 4;
        }
      }
      batch.Put(key_buf, val_buf);
      batch_bytes += kl + vl + 32;
      ++n;
      if (batch_bytes >= kBatchFlush) {
        st = db->Write(wopt, &batch);
        if (!st.ok()) {
          std::cerr << "Write failed: " << st.ToString() << std::endl;
          return 5;
        }
        batch.Clear();
        batch_bytes = 0;
      }
    }
    if (batch.GetDataSize() > 0) {
      st = db->Write(wopt, &batch);
      if (!st.ok()) {
        std::cerr << "Final Write failed: " << st.ToString() << std::endl;
        return 5;
      }
    }
  }

  st = db->Flush(rocksdb::FlushOptions());
  if (!st.ok()) {
    std::cerr << "Flush: " << st.ToString() << std::endl;
    return 6;
  }

  if (do_compact) {
    rocksdb::CompactRangeOptions cro;
    st = db->CompactRange(cro, nullptr, nullptr);
    if (!st.ok()) {
      std::cerr << "CompactRange: " << st.ToString() << std::endl;
      return 7;
    }
  }

  if (!jsonl_path.empty()) {
    std::cout << "vanilla_jsonl_ingest wrote " << n << " keys to " << db_path
              << " (skipped=" << skipped << ")\n";
  } else {
    std::cout << "vanilla_kv_ingest wrote " << n << " keys to " << db_path << "\n";
  }
  return 0;
}
