#!/bin/sh
# Dev build helper: ./build.sh [gen [module...]] - regenerate (optional) and build.
# Uses Windows Python (has capstone) for generation and MSYS2 MINGW64 for the build.
cd "$(dirname "$0")"
WPY=${WPY:-/c/Users/Msi/AppData/Local/Programs/Python/Python310/python.exe}
if [ "$1" = gen ]; then
  shift
  for m in ${@:-intro:INTRO:intro forest:FOREST:forest vil1:VILLAGE1:village1 vil2:VILLAGE2:village2 mine:MINE:mine g3d:3DGAME:3dgame}; do
    case $m in *:*) ;; intro) m=intro:INTRO:intro;; forest) m=forest:FOREST:forest;; vil1) m=vil1:VILLAGE1:village1;;
      vil2) m=vil2:VILLAGE2:village2;; mine) m=mine:MINE:mine;; g3d) m=g3d:3DGAME:3dgame;; esac
    t=${m%%:*}; r=${m#*:}; e=${r%%:*}; g=${r#*:}
    "$WPY" tools/recomp.py gamedata/cd/$e.EXE re/$g/functions.tsv tools/native_$t.txt src/gen/$t $t | head -1 || exit 1
  done
fi
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
make -j16 2>&1 | grep -E "error|undefined reference" | head -30
ls -la odyssey.exe | awk '{print $5, $6, $7, $8}'
