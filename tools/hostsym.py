"""Map host code addresses from odyssey.log ("host 0x... base 0x...") to the
recompiled function they are in.  usage: hostsym.py [odyssey.log]"""
import sys, re, subprocess, bisect, os
log = sys.argv[1] if len(sys.argv) > 1 else 'odyssey.log'
nm = subprocess.run(['/c/msys64/mingw64/bin/nm.exe' if os.path.exists('/c/msys64') else 'C:/msys64/mingw64/bin/nm.exe', 'odyssey.exe'],
                    capture_output=True, text=True).stdout
syms = sorted((int(a, 16), n) for a, t, n in (l.split() for l in nm.splitlines() if len(l.split()) == 3) if t in 'tT')
addrs = [s[0] for s in syms]
for m in re.finditer(r'host (?:0x)?([0-9a-fA-F]+) base (?:0x)?([0-9a-fA-F]+)', open(log).read()):
    a = int(m.group(1), 16) - int(m.group(2), 16) + 0x140000000
    i = bisect.bisect_right(addrs, a) - 1
    print('%x %s+%x' % (a, syms[i][1], a - syms[i][0]))
