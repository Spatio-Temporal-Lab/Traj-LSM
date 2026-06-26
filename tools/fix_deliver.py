f = r"D:\Project\tools\TIDB_INTEGRATION_GUIDE.md"
with open(f, "r", encoding="utf-8") as fh:
    content = fh.read()
content = content.replace("11.2.0", "11.0.4")
with open(f, "w", encoding="utf-8") as fh:
    fh.write(content)
print("Original GUIDE also fixed")
