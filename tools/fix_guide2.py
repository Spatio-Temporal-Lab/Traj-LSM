f = r"D:\Project\tools\TIDB_INTEGRATION_GUIDE.md"
with open(f, "r", encoding="utf-8") as fh:
    content = fh.read()

# The separator is fullwidth dashes, not regular dashes
idx = content.find("[28:30)")
# Find end of the section
end_idx = content.find("Followed by point_count")
block = content[idx:end_idx]

# Replace with corrected block
corrected = """[28:32) 4B    point_count (uint32)
---------------------
Header: 32 bytes total
  (C++ codec.hpp uses 28B SegmentValueHeader with uint32 point_count;
   the 32B here counts 4B magic + 28B header)


Followed by point_count"""

content = content[:idx] + corrected + content[end_idx+len("Followed by point_count"):]

with open(f, "w", encoding="utf-8") as fh:
    fh.write(content)

# Verify
idx2 = content.find("[28:")
print(content[idx2:idx2+300])
