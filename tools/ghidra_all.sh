#!/bin/sh
# Ghidra headless analysis + decompile export of every game program.
# out: re/<mod>/{<mod>_pe.exe,fixup_targets.txt,ghidra/functions.tsv,ghidra/decomp.c}
cd "$(dirname "$0")/.."
GH=/c/Users/Msi/Tools/ghidra_12.1.4_PUBLIC/support/analyzeHeadless.bat
PY=${PY:-python}
for m in ${MODS:-intro forest village1 village2 mine 3dgame}; do
  U=$(echo $m | tr a-z A-Z)
  mkdir -p re/$m/ghidra
  $PY tools/le2pe.py gamedata/cd/$U.EXE re/$m/${m}_pe.exe
  $PY tools/fixtargets.py gamedata/cd/$U.EXE re/$m/fixup_targets.txt
  W=$(cygpath -m "$PWD")
  rm -rf re/$m/ghidra_proj; mkdir -p re/$m/ghidra_proj
  cmd //c "$GH" "$W/re/$m/ghidra_proj" $m -import "$W/re/$m/${m}_pe.exe" \
     -scriptPath "$W/tools/ghidra" -max-cpu 8 \
     -preScript SeedFixups.java "$W/re/$m/fixup_targets.txt" \
     -postScript ExportAll.java "$W/re/$m/ghidra" < /dev/null > re/$m/ghidra_headless.log 2>&1
  echo "$m: $(grep -c . re/$m/ghidra/functions.tsv 2>/dev/null) functions"
done
