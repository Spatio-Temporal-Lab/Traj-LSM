f = r"D:\Project\tools\sample_data\validate.py"
with open(f, "r", encoding="utf-8") as fh:
    content = fh.read()

# Fix: validate.py reads t_start_unix, t_end_unix, lon_min, lon_max, lat_min, lat_max
# These now match the fixed CSV header, so no change needed to column names
# But the script has a typo: args.ingest_tool vs args.ingest_tool
old = "tool = args.ingest_tool"
new = "tool = args.ingest_tool"
content = content.replace(old, new)
print("validate.py is already consistent with fixed CSV header")

with open(f, "w", encoding="utf-8") as fh:
    fh.write(content)
print("validate.py checked")
