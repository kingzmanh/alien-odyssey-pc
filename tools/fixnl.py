"""Repair C string literals split by a raw newline (shell heredoc mangling of
backslash-n): joins a line with an odd number of quotes to the next one
with an escaped newline.  usage: fixnl.py file.c"""
import sys
BS_N = chr(92) + 'n'
p = sys.argv[1]
L = open(p).read().split('\n')
out, i = [], 0
while i < len(L):
    l = L[i]
    if l.count('"') % 2 == 1 and i + 1 < len(L) and L[i + 1].count('"') % 2 == 1:
        out.append(l + BS_N + L[i + 1])
        i += 2
        continue
    out.append(l)
    i += 1
open(p, 'w').write('\n'.join(out))
