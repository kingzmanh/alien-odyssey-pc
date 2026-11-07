"""make_kit.py <out dir> - assemble the "convert on this PC" build kit.

The kit lets the setup program turn the user's own ALIENSP2.EXE into the
Windows game without any game code being shipped:
    builder/python     Python + capstone (runs tools/recomp.py)
    builder/tools      le.py, x86an.py, recomp.py, native.txt
    builder/re         functions.tsv (function start addresses)
    builder/src/rt     the PC runtime sources
    builder/mingw64    a minimal GCC: only the headers, libraries and
                       programs this build actually uses (found with gcc -M,
                       ld --trace and the programs' DLL imports)
Run with the MSYS2 MINGW64 toolchain installed at C:\\msys64.
"""
import os, sys, shutil, subprocess, re, glob

MINGW = r'C:\msys64\mingw64'
PYHOME = os.path.dirname(sys.executable)
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.abspath(sys.argv[1])
kit = os.path.join(out, 'builder')
GCC = os.path.join(MINGW, 'bin', 'gcc.exe')
OBJDUMP = os.path.join(MINGW, 'bin', 'objdump.exe')
env = dict(os.environ)
env['PATH'] = os.path.join(MINGW, 'bin') + ';' + env.get('PATH', '')


def cp(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def rel_mingw(p):
    p = os.path.normcase(os.path.abspath(p))
    m = os.path.normcase(MINGW)
    return p[len(m) + 1:] if p.startswith(m + os.sep) else None


if os.path.exists(kit):
    shutil.rmtree(kit)
os.makedirs(kit)

# --- recompiler + runtime sources ---
for f in ('le.py', 'x86an.py', 'recomp.py', 'native.txt'):
    cp(os.path.join(HERE, 'tools', f), os.path.join(kit, 'tools', f))
cp(os.path.join(HERE, 're', 'functions.tsv'), os.path.join(kit, 're', 'functions.tsv'))
for f in glob.glob(os.path.join(HERE, 'src', 'rt', '*.[ch]')):
    cp(f, os.path.join(kit, 'src', 'rt', os.path.basename(f)))

# --- python: interpreter, stdlib (no tests/gui), capstone ---
py = os.path.join(kit, 'python')
for f in os.listdir(PYHOME):
    fl = f.lower()
    if fl in ('python.exe', 'python3.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') or re.match(r'python3\d+\.dll$', fl):
        cp(os.path.join(PYHOME, f), os.path.join(py, f))
shutil.copytree(os.path.join(PYHOME, 'DLLs'), os.path.join(py, 'DLLs'),
                ignore=shutil.ignore_patterns('tk*', 'tcl*', '_tkinter*', '*.ico'))
skip = {'test', 'tests', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'site-packages', 'lib2to3', '__pycache__', 'pydoc_data'}
shutil.copytree(os.path.join(PYHOME, 'Lib'), os.path.join(py, 'Lib'),
                ignore=lambda d, names: [n for n in names if n in skip])
import capstone
shutil.copytree(os.path.dirname(capstone.__file__), os.path.join(py, 'Lib', 'site-packages', 'capstone'),
                ignore=shutil.ignore_patterns('__pycache__', 'include', '*.a'))

# --- gcc: find exactly what a build uses ---
tmp = os.path.join(out, 'kit_tmp')
os.makedirs(tmp, exist_ok=True)
gen = os.path.join(HERE, 'src', 'gen')
rt = os.path.join(HERE, 'src', 'rt')
sdl_inc = os.path.join(MINGW, 'include', 'SDL2')
need = set()
srcs = glob.glob(os.path.join(gen, '*.c')) + glob.glob(os.path.join(rt, '*.c'))
for s in srcs:
    is_rt = os.path.dirname(s) == rt
    r = subprocess.run([GCC, '-M', '-std=gnu11', '-I', rt, '-I', gen, '-I', sdl_inc] + (['-DSDL_MAIN_HANDLED'] if is_rt else []) + [s],
                       capture_output=True, text=True, env=env)
    for tok in r.stdout.replace('\\\n', ' ').split():
        if tok.endswith(':'):
            continue
        rp = rel_mingw(tok)
        if rp:
            need.add(rp)
# link trace: archives and startup objects
objs = []
for s in glob.glob(os.path.join(rt, '*.c'))[:3]:
    o = os.path.join(tmp, os.path.basename(s) + '.o')
    subprocess.run([GCC, '-c', '-DSDL_MAIN_HANDLED', '-std=gnu11', '-I', rt, '-I', gen, '-I', sdl_inc, s, '-o', o], env=env, check=True)
    objs.append(o)
r = subprocess.run([GCC, '-mwindows', '-o', os.path.join(tmp, 't.exe')] + glob.glob(os.path.join(HERE, 'build', 'gen', '*.o')) +
                   glob.glob(os.path.join(HERE, 'build', 'rt', '*.o')) +
                   ['-static', '-lmingw32', os.path.join(MINGW, 'lib', 'libSDL2.a'), '-lm', '-lkernel32', '-luser32', '-lgdi32',
                    '-lwinmm', '-limm32', '-lole32', '-loleaut32', '-lversion', '-luuid', '-ladvapi32', '-lsetupapi', '-lshell32',
                    '-ldinput8', '-Wl,--trace', '-v'], capture_output=True, text=True, env=env)
for line in (r.stdout + r.stderr).splitlines():
    m = re.match(r'^\(?([A-Za-z]:[^()]+?\.(?:a|o))\)?', line.strip())
    if m:
        rp = rel_mingw(m.group(1))
        if rp:
            need.add(rp)
if r.returncode:
    print(r.stderr[-2000:])
    sys.exit('trial link failed')
# gcc's own pieces
ver = os.listdir(os.path.join(MINGW, 'lib', 'gcc', 'x86_64-w64-mingw32'))[0]
for p in ['bin\\gcc.exe', 'bin\\as.exe', 'bin\\ld.exe', 'bin\\ld.bfd.exe']:
    need.add(p)
# ask gcc where its helpers live (MSYS2 keeps cc1 under lib\gcc, others under libexec)
for prog in ('cc1', 'collect2', 'lto-wrapper'):
    r = subprocess.run([GCC, '-print-prog-name=' + prog], capture_output=True, text=True, env=env)
    rp = rel_mingw(r.stdout.strip().replace('/', os.sep))
    if rp and os.path.exists(os.path.join(MINGW, rp)):
        need.add(rp)
r = subprocess.run([GCC, '-print-file-name=liblto_plugin.dll'], capture_output=True, text=True, env=env)
rp = rel_mingw(r.stdout.strip().replace('/', os.sep))
if rp and os.path.exists(os.path.join(MINGW, rp)):
    need.add(rp)
gccdir = os.path.join(MINGW, 'lib', 'gcc', 'x86_64-w64-mingw32', ver)
for f in glob.glob(os.path.join(gccdir, '*.o')):
    need.add(rel_mingw(f))
for sub in ('include', 'include-fixed'):
    for root, dirs, files in os.walk(os.path.join(gccdir, sub)):
        for f in files:
            need.add(rel_mingw(os.path.join(root, f)))
need.add('lib\\libSDL2.a')
# DLLs the programs import (recursively, from mingw64\bin)
todo = [p for p in need if p.lower().endswith(('.exe', '.dll'))]
seen = set()
while todo:
    p = todo.pop()
    if p in seen:
        continue
    seen.add(p)
    r = subprocess.run([OBJDUMP, '-p', os.path.join(MINGW, p)], capture_output=True, text=True, env=env)
    for dll in re.findall(r'DLL Name: (\S+)', r.stdout):
        cand = os.path.join('bin', dll)
        if os.path.exists(os.path.join(MINGW, cand)) and cand not in need:
            need.add(cand)
            todo.append(cand)
total = 0
for p in sorted(need):
    src = os.path.join(MINGW, p)
    if os.path.isfile(src):
        cp(src, os.path.join(kit, 'mingw64', p))
        total += os.path.getsize(src)
shutil.rmtree(tmp, ignore_errors=True)
print('kit: %d toolchain files (%.1f MB) in %s' % (len(need), total / 1e6, kit))
