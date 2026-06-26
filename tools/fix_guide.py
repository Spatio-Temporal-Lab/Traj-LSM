f = r"D:\Project\tools\TIDB_INTEGRATION_GUIDE.md"
with open(f, "r", encoding="utf-8") as fh:
    content = fh.read()

# Fix 1: offset table - x_max was labeled y_max
old1 = """[20:24) 4B    y_max   (float32)
[24:28) 4B    y_max   (float32)"""
new1 = """[20:24) 4B    x_max   (float32)
[24:28) 4B    y_max   (float32)"""
content = content.replace(old1, new1)

# Fix 2: point_count type + header size
old2 = """[28:30) 2B    point_count (uint16)
[30:32) 2B    reserved
---------------------
Header: 32 bytes total"""
new2 = """[28:32) 4B    point_count (uint32)
---------------------
Header: 32 bytes total"""
content = content.replace(old2, new2)

# Fix 3: Patch generation instructions
old3 = """git apply /path/to/rocksdb-st-full.patch"""
new3 = """# Generate the patch first (in your RocksDB repo):
#   git diff <base-commit>..HEAD > rocksdb-st-full.patch
#   wc -l rocksdb-st-full.patch  # should be 1000+ lines, NOT 1 line
git apply /path/to/rocksdb-st-full.patch"""
content = content.replace(old3, new3)

# Fix 4: Go code comment
old4 = """func EncodeE7Key(bucketID uint32, zorder uint64, tStartOffset uint32,"""
new4 = """// NOTE: Use uint32 for all time arithmetic to match C++ overflow behavior
func EncodeE7Key(bucketID uint32, zorder uint64, tStartOffset uint32,"""
content = content.replace(old4, new4)

# Fix 5: Add note about segments_meta.csv format
old5 = """- `segments_meta.csv`          ← 1000 sample segments metadata"""
new5 = """- `segments_meta.csv`          ← 1000 sample segments metadata
    (columns: segment_id,dataset,point_count,t_start_unix,t_end_unix,lon_min,lon_max,lat_min,lat_max,source_ref)"""
content = content.replace(old5, new5)

# Fix 6: Add note after the offset table explaining V2 vs V3
old6 = """Helper functions in `segment_value_codec.hpp`:"""
new6 = """Note: codec.hpp supports both V2 (28B header with uint32 point_count) and V3 (ghost entries).
The offset table above describes the wire format of V2. See segment_value_codec.hpp for V3 layout.

Helper functions in `segment_value_codec.hpp`:"""
content = content.replace(old6, new6)

with open(f, "w", encoding="utf-8") as fh:
    fh.write(content)
print("All fixes applied to TIDB_INTEGRATION_GUIDE.md")
