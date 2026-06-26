f = r"D:\Project\tools\TIDB_INTEGRATION_GUIDE.md"
with open(f, "r", encoding="utf-8") as fh:
    content = fh.read()

old = """# Generate the patch first (in your RocksDB repo):
#   git diff <base-commit>..HEAD > rocksdb-st-full.patch
#   wc -l rocksdb-st-full.patch  # should be 1000+ lines, NOT 1 line
git apply /path/to/rocksdb-st-full.patch"""

new = """# The patch file (rocksdb-st-full.patch) must be generated from the
# ST-RocksDB fork. If you received a broken patch (1 line "fatal: bad revision"),
# ask the author to regenerate it from the ST branch:
#
#   In the ST-RocksDB repo:
#   git branch -a | grep st       # find the ST branch name
#   git diff main..<st-branch> > rocksdb-st-full.patch
#   wc -l rocksdb-st-full.patch   # must be 1000+ lines
#
# Then apply:
git apply /path/to/rocksdb-st-full.patch"""

content = content.replace(old, new)

with open(f, "w", encoding="utf-8") as fh:
    fh.write(content)
print("Updated patch instructions")
