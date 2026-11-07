# Alien Odyssey for Windows.
# Build from an MSYS2 MINGW64 shell:  make -j16
#
# src/gen/<module> is the game's code, recompiled from the six DOS/4GW
# programs on the CD by tools/recomp.py (make gen); src/rt is the PC runtime
# that replaces DOS, the DOS extender, the BIOS, the hardware and the
# launcher (MCP.EXE).
CC    = gcc
B     = build
EXE   = odyssey.exe
PY   ?= python
CD   ?= gamedata/cd

$(shell mkdir -p $(B)/tmp)
export TMP  := $(shell cygpath -m "$(CURDIR)/$(B)/tmp")
export TEMP := $(TMP)

SDL_CFLAGS := $(shell sdl2-config --cflags | sed 's/-Dmain=SDL_main//')
SDL_LIBS   := $(shell sdl2-config --static-libs | sed 's/-lSDL2main//;s/-mwindows//')

CFLAGS_GEN := -std=gnu11 -O2 -g0 -fwrapv -fno-strict-aliasing -w -Isrc/rt
CFLAGS_RT  := -DSDL_MAIN_HANDLED -std=gnu11 -O2 -g -Wall -Wno-unused-function -fwrapv -fno-strict-aliasing -Isrc/rt $(SDL_CFLAGS)

# module tag : program on the CD : Ghidra analysis dir
MODS := intro:INTRO:intro forest:FOREST:forest vil1:VILLAGE1:village1 \
        vil2:VILLAGE2:village2 mine:MINE:mine g3d:3DGAME:3dgame

GEN_SRCS := $(wildcard src/gen/*/*.c)
RT_SRCS  := $(wildcard src/rt/*.c)
# the dev build embeds the game's emblem as icon when the user's CD is in gamedata/
ICON_SRC ?= $(CD)/INTRO/DATA/A.LBM
RES_OBJ := $(if $(wildcard $(ICON_SRC)),$(B)/odyssey_res.o,)
OBJS := $(patsubst src/%.c,$(B)/%.o,$(GEN_SRCS) $(RT_SRCS))

all: $(EXE)

$(EXE): $(OBJS) $(RES_OBJ)
	$(CC) -mwindows -o $@ $^ -static $(SDL_LIBS) -ld3d9 -lwinmm -lm

$(B)/odyssey.ico: $(ICON_SRC) tools/make_icon.py tools/lbm.py
	$(PY) tools/make_icon.py $< $@

$(B)/odyssey_res.o: src/rt/odyssey.rc $(B)/odyssey.ico
	windres -I $(B) $< -o $@

$(B)/gen/%.o: src/gen/%.c src/rt/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_GEN) -I$(dir $<) -c $< -o $@

$(B)/rt/%.o: src/rt/%.c src/rt/rt.h src/rt/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RT) -c $< -o $@

# regenerate the recompiled code (after changing tools/ or tools/native_*.txt)
gen:
	@for m in $(MODS); do \
	  t=$${m%%:*}; r=$${m#*:}; e=$${r%%:*}; g=$${r#*:}; \
	  echo "== $$t ($$e.EXE)"; \
	  $(PY) tools/recomp.py $(CD)/$$e.EXE re/$$g/functions.tsv tools/native_$$t.txt src/gen/$$t $$t || exit 1; \
	done

clean:
	rm -rf $(B) $(EXE)

.PHONY: all gen clean

# ---- release: game exe without the game's icon (setup puts it in from the
# user's CD) and the setup program with that exe inside ----
release: build/release/odyssey.exe build/setup.exe
	cp build/setup.exe "Alien Odyssey PC Setup.exe"

build/release/odyssey.exe: $(OBJS)
	@mkdir -p build/release
	$(CC) -mwindows -o $@ $^ -static $(SDL_LIBS) -ld3d9 -lwinmm -lm
	strip $@

SETUP_SRCS := installer/setup.c installer/install.c installer/cdsrc.c installer/util.c
SETUP_LIBS := -lcomctl32 -lcomdlg32 -lole32 -luuid -lshell32 -ladvapi32

$(B)/setup_res.o: installer/setup.rc installer/setup.manifest build/release/odyssey.exe
	windres -I installer $< -o $@

build/setup.exe: $(SETUP_SRCS) installer/setup.h installer/install.h $(B)/setup_res.o
	$(CC) -O2 -Wall -Wno-format-truncation -mwindows -static -o $@ $(SETUP_SRCS) $(B)/setup_res.o $(SETUP_LIBS)

.PHONY: release
