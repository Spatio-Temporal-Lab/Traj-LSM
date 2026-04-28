#include <iomanip>
#include <iostream>
#include <memory>
#include <fstream>
#include <vector>
#include <algorithm>
#include <string>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

namespace {

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          static const char* kHex = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(c >> 4) & 0xF];
          out += kHex[c & 0xF];
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

struct Row {
  std::string file;
  uint64_t entries = 0;
  uint64_t data_blocks = 0;
  uint64_t data_size = 0;
  double avg_block_bytes = 0.0;
  double avg_entries_per_block = 0.0;
};

std::string TsvHeader() {
  return "file\tentries\tdata_blocks\tdata_size\tavg_block_bytes\tavg_entries_per_block\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_path;
  std::string out_tsv;
  std::string out_json;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--db" && i + 1 < argc) {
      db_path = argv[++i];
    } else if (a == "--out-tsv" && i + 1 < argc) {
      out_tsv = argv[++i];
    } else if (a == "--out-json" && i + 1 < argc) {
      out_json = argv[++i];
    } else if (a == "-h" || a == "--help") {
      std::cout
          << "Usage: st_vanilla_table_props --db <path> [--out-tsv <path>] [--out-json <path>]\n"
             "  Prints per-SST table properties (TSV) and a TOTAL row.\n"
             "  If --out-tsv is set, also writes the TSV to that path.\n"
             "  If --out-json is set, writes a machine-readable JSON summary.\n";
      return 0;
    }
  }
  if (db_path.empty()) {
    std::cerr << "Missing --db <path>\n";
    return 1;
  }

  rocksdb::Options opt;
  opt.create_if_missing = false;
  opt.persist_user_defined_timestamps = false;
  rocksdb::BlockBasedTableOptions tb;
  opt.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));

  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status st = rocksdb::DB::OpenForReadOnly(opt, db_path, &db);
  if (!st.ok()) {
    std::cerr << "OpenForReadOnly failed: " << st.ToString() << "\n";
    return 2;
  }

  rocksdb::TablePropertiesCollection props;
  st = db->GetPropertiesOfAllTables(&props);
  if (!st.ok()) {
    std::cerr << "GetPropertiesOfAllTables failed: " << st.ToString() << "\n";
    return 3;
  }

  std::vector<Row> rows;
  rows.reserve(props.size());

  uint64_t total_entries = 0;
  uint64_t total_data_blocks = 0;
  uint64_t total_data_size = 0;

  for (const auto& kv : props) {
    const auto& name = kv.first;
    const auto& p = kv.second;
    const uint64_t e = p->num_entries;
    const uint64_t b = p->num_data_blocks;
    const uint64_t ds = p->data_size;
    const double avg_b = (b == 0) ? 0.0 : static_cast<double>(ds) / static_cast<double>(b);
    const double avg_e = (b == 0) ? 0.0 : static_cast<double>(e) / static_cast<double>(b);
    total_entries += e;
    total_data_blocks += b;
    total_data_size += ds;
    Row r;
    r.file = name;
    r.entries = e;
    r.data_blocks = b;
    r.data_size = ds;
    r.avg_block_bytes = avg_b;
    r.avg_entries_per_block = avg_e;
    rows.push_back(std::move(r));
  }

  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.file < b.file; });

  const double g_avg_b = total_data_blocks == 0
                             ? 0.0
                             : static_cast<double>(total_data_size) /
                                   static_cast<double>(total_data_blocks);
  const double g_avg_e = total_data_blocks == 0
                             ? 0.0
                             : static_cast<double>(total_entries) /
                                   static_cast<double>(total_data_blocks);

  std::ostream* out = &std::cout;
  std::ofstream tsv_f;
  if (!out_tsv.empty()) {
    tsv_f.open(out_tsv, std::ios::binary);
    if (!tsv_f) {
      std::cerr << "Cannot open --out-tsv: " << out_tsv << "\n";
      return 4;
    }
    out = &tsv_f;
  }

  (*out) << TsvHeader();
  (*out) << std::fixed << std::setprecision(2);
  for (const Row& r : rows) {
    (*out) << r.file << "\t" << r.entries << "\t" << r.data_blocks << "\t"
           << r.data_size << "\t" << r.avg_block_bytes << "\t"
           << r.avg_entries_per_block << "\n";
  }
  (*out) << "TOTAL\t" << total_entries << "\t" << total_data_blocks << "\t"
         << total_data_size << "\t" << g_avg_b << "\t" << g_avg_e << "\n";

  if (!out_json.empty()) {
    std::ofstream jf(out_json, std::ios::binary);
    if (!jf) {
      std::cerr << "Cannot open --out-json: " << out_json << "\n";
      return 5;
    }
    jf << "{\n";
    jf << "  \"db\": \"" << JsonEscape(db_path) << "\",\n";
    jf << "  \"files\": [\n";
    for (size_t i = 0; i < rows.size(); ++i) {
      const Row& r = rows[i];
      jf << "    {\n";
      jf << "      \"file\": \"" << JsonEscape(r.file) << "\",\n";
      jf << "      \"entries\": " << r.entries << ",\n";
      jf << "      \"data_blocks\": " << r.data_blocks << ",\n";
      jf << "      \"data_size\": " << r.data_size << ",\n";
      jf << "      \"avg_block_bytes\": " << std::fixed << std::setprecision(2)
         << r.avg_block_bytes << ",\n";
      jf << "      \"avg_entries_per_block\": " << std::fixed << std::setprecision(2)
         << r.avg_entries_per_block << "\n";
      jf << "    }" << (i + 1 == rows.size() ? "" : ",") << "\n";
    }
    jf << "  ],\n";
    jf << "  \"total\": {\n";
    jf << "    \"entries\": " << total_entries << ",\n";
    jf << "    \"data_blocks\": " << total_data_blocks << ",\n";
    jf << "    \"data_size\": " << total_data_size << ",\n";
    jf << "    \"avg_block_bytes\": " << std::fixed << std::setprecision(2) << g_avg_b
       << ",\n";
    jf << "    \"avg_entries_per_block\": " << std::fixed << std::setprecision(2) << g_avg_e
       << "\n";
    jf << "  }\n";
    jf << "}\n";
  }

  return 0;
}

