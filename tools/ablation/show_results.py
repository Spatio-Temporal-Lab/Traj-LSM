import csv, os, glob, subprocess, sys

def read_tsv(path):
    with open(path, 'rb') as f:
        bom = f.read(2)
    enc = 'utf-16-le' if bom == b'\xff\xfe' else 'utf-8'
    with open(path, 'r', encoding=enc) as f:
        return list(csv.DictReader(f, delimiter='\t'))

result_dir = 'D:/Project/Result/ablation'

# Global
print('=== GLOBAL INDEX ABLATION ===')
gf = sorted(glob.glob(os.path.join(result_dir, 'global_*.tsv')))[-1]
rows = read_tsv(gf)
variants = {}
for r in rows:
    v = r['variant']
    if v not in variants: variants[v] = {'fs':0,'fc':0,'br':0,'wall':0,'n':0}
    try:
        variants[v]['fs'] += int(r['files_skipped'])
        variants[v]['fc'] += int(r['files_considered'])
        variants[v]['br'] += int(r['block_reads'])
        variants[v]['wall'] += int(r['wall_us'])
        variants[v]['n'] += 1
    except: pass

hdr = "%-15s %12s %10s %10s" % ("Variant", "SkipRate", "BlockReads", "Wall_ms")
print(hdr)
print("-" * 50)
for v in ['no_file','with_file']:
    if v in variants:
        d = variants[v]
        sr = "%d/%d (%d%%)" % (d['fs'], d['fc'], 100*d['fs']//d['fc']) if d['fc'] else 'N/A'
        print("%-15s %12s %10d %10.1f" % (v, sr, d['br'], d['wall']/1000))

# Local
print('\n=== LOCAL INDEX ABLATION ===')
lf = sorted(glob.glob(os.path.join(result_dir, 'local_*.tsv')))[-1]
rows = read_tsv(lf)
variants = {}
for r in rows:
    v = r['variant']
    if v not in variants: variants[v] = {'br':0,'ie':0,'is':0,'ke':0,'wall':0,'n':0}
    try:
        variants[v]['br'] += int(r['block_reads'])
        variants[v]['ie'] += int(r['index_examined'])
        variants[v]['is'] += int(r['index_skipped'])
        variants[v]['ke'] += int(r['keys_examined'])
        variants[v]['wall'] += int(r['wall_us'])
        variants[v]['n'] += 1
    except: pass

hdr = "%-20s %10s %10s %10s %10s %10s" % ("Variant", "BlockReads", "IdxExam", "IdxSkip", "KeyExam", "Wall_ms")
print(hdr)
print("-" * 73)
for v in ['file_only','file_block','file_block_key']:
    if v in variants:
        d = variants[v]
        print("%-20s %10d %10d %10d %10d %10.1f" % (v, d['br'], d['ie'], d['is'], d['ke'], d['wall']/1000))

# Compaction
print('\n=== COMPACTION ABLATION ===')
exe = 'D:/Project/rocks-demo/build/st_compaction_throughput_bench.exe'
csv_data = []
for s in ['all_leveled','all_tiered_r4','hybrid_l1_r4','hybrid_l12_r4','stfp_r4','stfp_r6','stfp_rinf']:
    print("  Running %s..." % s)
    r = subprocess.run([exe, '--num-keys', '20000', '--repeat', '1', '--%s' % s], 
                       capture_output=True, text=True, timeout=120)
    for line in r.stdout.split('\n'):
        if line.startswith(s + ','):
            parts = line.split(',')
            if len(parts) >= 14:
                csv_data.append({'s':s, 'read':float(parts[13]), 'write':float(parts[2]), 
                                'amp':float(parts[5]), 'l0':parts[6], 'l1':parts[7], 'l2':parts[8], 'l3':parts[9]})

if csv_data:
    print("%-22s %10s %10s %7s %4s %4s %4s %4s" % ("Strategy", "ReadMB/s", "WriteMB/s", "W-Amp", "L0", "L1", "L2", "L3"))
    print("-" * 68)
    for d in csv_data:
        print("%-22s %10.1f %10.1f %7.1fx %4s %4s %4s %4s" % (d['s'], d['read'], d['write'], d['amp'], d['l0'], d['l1'], d['l2'], d['l3']))
else:
    print('No data collected')
