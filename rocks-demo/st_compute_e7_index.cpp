// Pre-compute E7 key index fields and append to segments_meta.csv
// Uses EXACT same functions as st_bucket_ingest_build.cpp
#include <fstream>
#include <iostream>
#include <cstring>
#include "segment_points_store.hpp"

namespace {

// === Copied from st_bucket_ingest_build.cpp — keep in sync ===

uint64_t Fnv1a64SegmentId(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) { h ^= static_cast<uint64_t>(c); h *= 1099511628211ULL; }
  return h;
}

bool ParseUnsignedDecimal(const std::string& text, uint64_t* out) {
  if (out == nullptr || text.empty()) return false;
  uint64_t value = 0;
  for (char ch : text) { if (ch < '0' || ch > '9') return false; value = value * 10 + static_cast<uint64_t>(ch - '0'); }
  *out = value;
  return true;
}

void ParseSegmentIdForE7(const std::string& segment_id, uint64_t* vehicle_id,
                         uint32_t* segment_seq, uint16_t* chunk_seq) {
  if (!vehicle_id || !segment_seq || !chunk_seq) return;
  *vehicle_id = Fnv1a64SegmentId(segment_id);
  *segment_seq = 0; *chunk_seq = 0;
  const size_t last_colon = segment_id.rfind(':');
  if (last_colon != std::string::npos) {
    const size_t prev_colon = segment_id.rfind(':', last_colon - 1);
    if (prev_colon != std::string::npos) {
      uint64_t parsed_phone = 0;
      if (ParseUnsignedDecimal(segment_id.substr(prev_colon + 1, last_colon - prev_colon - 1), &parsed_phone))
        *vehicle_id = parsed_phone;
    }
  }
  const std::string tail = (last_colon == std::string::npos) ? segment_id : segment_id.substr(last_colon + 1);
  const size_t hash = tail.find('#');
  const std::string main_part = tail.substr(0, hash);
  const std::string chunk_part = (hash == std::string::npos) ? std::string() : tail.substr(hash + 1);
  bool chunk_parsed = false;
  const size_t underscore = main_part.rfind('_');
  if (underscore != std::string::npos) {
    uint64_t parsed_seq = 0, parsed_chunk = 0;
    if (ParseUnsignedDecimal(main_part.substr(0, underscore), &parsed_seq)) *segment_seq = static_cast<uint32_t>(parsed_seq);
    if (ParseUnsignedDecimal(main_part.substr(underscore + 1), &parsed_chunk)) { *chunk_seq = static_cast<uint16_t>(parsed_chunk); chunk_parsed = true; }
  } else if (!main_part.empty()) {
    uint64_t parsed_seq = 0;
    if (ParseUnsignedDecimal(main_part, &parsed_seq)) *segment_seq = static_cast<uint32_t>(parsed_seq);
  }
  if (chunk_part.size() >= 2 && chunk_part[0] == 'p') {
    uint64_t parsed_chunk = 0;
    if (ParseUnsignedDecimal(chunk_part.substr(1), &parsed_chunk)) { *chunk_seq = static_cast<uint16_t>(parsed_chunk); return; }
  }
  if (*chunk_seq == 0 && !chunk_parsed) *chunk_seq = 1;
}

uint64_t MakeZOrderStart(const std::vector<segval::SegmentPoint>& points) {
  if (points.empty()) return 0;
  double cx = 0.0, cy = 0.0;
  for (const auto& p : points) {
    cx += static_cast<double>(p.lon);
    cy += static_cast<double>(p.lat);
  }
  const double n = static_cast<double>(points.size());
  cx /= n; cy /= n;
  const float flon = static_cast<float>(cx);
  const float flat = static_cast<float>(cy);
  uint32_t lon_bits = 0, lat_bits = 0;
  std::memcpy(&lon_bits, &flon, sizeof(lon_bits));
  std::memcpy(&lat_bits, &flat, sizeof(lat_bits));
  uint64_t code = 0;
  for (int i = 31; i >= 0; --i) {
    code = (code << 2) |
           ((static_cast<uint64_t>(lon_bits >> i) & 1U) << 1) |
           (static_cast<uint64_t>(lat_bits >> i) & 1U);
  }
  return code;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 4) {
    std::cerr << "Usage: st_compute_e7_index --meta-csv <meta.csv> --points-csv <points.csv> [--bucket-sec 86400]\n";
    return 1;
  }
  std::string meta_csv, points_csv;
  uint32_t bucket_sec = 86400;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--meta-csv" && i + 1 < argc) meta_csv = argv[++i];
    else if (a == "--points-csv" && i + 1 < argc) points_csv = argv[++i];
    else if (a == "--bucket-sec" && i + 1 < argc) bucket_sec = static_cast<uint32_t>(std::stoul(argv[++i]));
  }

  segval::SegmentPointsStore spill;
  std::cout << "Building spill index for " << points_csv << "...\n";
  if (!spill.BuildFromCsv(points_csv)) { std::cerr << "Failed\n"; return 1; }

  std::ifstream in(meta_csv);
  std::string out_path = meta_csv + ".e7indexed";
  std::ofstream out(out_path);
  if (!in || !out) { std::cerr << "Cannot open files\n"; return 1; }

  std::string line;
  std::getline(in, line);
  out << line << ",e7_bucket_id,e7_zorder,e7_vehicle_id,e7_segment_seq,e7_chunk_seq\n";

  uint64_t cnt = 0;
  while (std::getline(in, line)) {
    size_t c1 = line.find(',');
    if (c1 == std::string::npos) continue;
    std::string sid = line.substr(0, c1);

    size_t pos = c1 + 1;
    for (int f = 0; f < 2 && pos < line.size(); f++) pos = line.find(',', pos) + 1;  // dataset,point_count
    if (pos == 0 || pos > line.size()) continue;
    size_t c4 = line.find(',', pos);
    uint32_t t_start = static_cast<uint32_t>(std::stoul(line.substr(pos, c4 - pos)));

    std::vector<segval::SegmentPoint> points;
    spill.Get(sid, &points);
    uint64_t zorder = MakeZOrderStart(points);
    uint32_t bucket_id = t_start / bucket_sec;
    uint64_t vid = 0; uint32_t seq = 0; uint16_t chk = 0;
    ParseSegmentIdForE7(sid, &vid, &seq, &chk);

    out << line << ',' << bucket_id << ',' << zorder << ',' << vid << ',' << seq << ',' << chk << '\n';
    if (++cnt % 50000 == 0) std::cout << "  " << cnt << " rows...\n";
  }
  std::cout << "Done. " << cnt << " rows -> " << out_path << "\n";
  return 0;
}
