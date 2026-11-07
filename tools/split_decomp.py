"""Split Ghidra's decomp.c export into one file per function (for graphify).
usage: split_decomp.py re/<mod>/ghidra/decomp.c re/<mod>/graph/src"""
import sys, os, re

src, out = sys.argv[1:3]
os.makedirs(out, exist_ok=True)
txt = open(src, encoding='utf-8', errors='replace').read()
parts = re.split(r'^// ==== ([0-9a-fA-F]+) (\S+) ====\n', txt, flags=re.M)
n = 0
for i in range(1, len(parts) - 2, 3):
    addr, name, body = parts[i], parts[i + 1], parts[i + 2]
    fn = os.path.join(out, '%s_%s.c' % (addr.lower(), re.sub(r'[^A-Za-z0-9_]', '_', name)))
    open(fn, 'w', encoding='utf-8').write(body)
    n += 1
print(out, n, 'functions')
