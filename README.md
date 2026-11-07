# Alien Odyssey — PC Native

A native Windows version of *Alien Odyssey* (Argonaut Software / Philips Media, 1995).
No DOSBox: the game's own code is converted from x86 machine code to C by a static recompiler
and runs on a small PC runtime that replaces DOS, the DOS/4GW extender, the BIOS, the sound and
video hardware and the game's launcher with Windows services.

**This repository contains no game files and no game code.** You need your own Alien Odyssey CD
(the disc, an ISO / BIN+CUE image, or a copied folder).

## Installing

Run **`Alien Odyssey PC Setup.exe`**, point it at your CD, choose a folder, press Install. It copies
the disc (it is then always "in the drive"), lays out what the original DOS installer put on the
hard disk, and creates the game shortcut and an uninstaller. About 500 MB of disk space is needed.
The game icon is made from the emblem picture on your CD.

Scripted install: `"Alien Odyssey PC Setup.exe" /silent /cd <iso|cue|drive|folder> /dir <folder> [/nodesktop] [/nostartmenu]`

## Playing

- Starts in fullscreen; **Alt+Enter** switches to a window and back (remembered in `odyssey_pc.ini`).
- **Video menu** (menu bar of the window, like our Xbox ports; hidden in fullscreen):
  - *Internal Resolution* 1x-6x (320x200 up to 1920x1200): the 3D game's scenes are really rendered at
    this size (BRender is asked to render the scene a second time into a larger frame, seeded with the
    pre-rendered room and its depth map, and the result is laid over the picture with a mask, so the
    HUD and 2D parts stay as drawn). Default 4x.
  - *Upscale Filter* (nearest / bilinear / bicubic / sharp bilinear), *Anti-aliasing (FXAA)*,
    *Sharpening* (contrast adaptive, off/low/medium/high), *Aspect Ratio* (4:3 / stretch / integer).
  - *View*: window size, fullscreen (Alt+Enter), FPS in the title. All saved in `odyssey_pc.ini`.
  - The videos and 2D screens only carry their original 320x200 pixels; they go through the filter.
- Keyboard as in the original (`GAME.CFG` holds the key layout, as on DOS).
- **Mouse aiming** on the bike and in the mine cart: move the mouse to aim, left button fires (the game's
  own mouse control, `Controller 0` in `GAME.CFG`, switchable in *Input > Mouse aiming*). The pointer is
  captured while the game aims with it. *Input > Mouse sensitivity* / *Invert mouse Y*.
- Speech, sound effects and the FMV soundtracks through a software mixer; music through the
  Windows General MIDI synthesizer (the game's General MIDI arrangements).
- Saved games go to `game\hd\SAVEGAME`.

## How it works

The DOS game is six DOS/4GW programs (`INTRO`, `FOREST`, `VILLAGE1`, `VILLAGE2`, `MINE`, `3DGAME`)
chained by a 16-bit launcher (`MCP.EXE`). All six are recompiled into one `odyssey.exe`; each runs on
a fresh guest address space loaded from the original executable on the CD, and its exit code tells
the (rewritten) launcher what to run next, exactly as `MCP.EXE` did.

| Part | Where |
|---|---|
| LE loader (incl. the DOS/4GW-bound `3DGAME.EXE`) | `tools/le.py`, `src/rt/le.c` |
| Function discovery (recursive descent, fixup code pointers, Watcom switch tables, multi-entry functions for computed jumps into unrolled loops) | `tools/x86an.py` |
| x86 → C recompiler (registers in C locals, flags only where used, x87) | `tools/recomp.py` |
| Function start lists (Ghidra analysis of each program) | `re/<program>/functions.tsv` |
| The launcher (`MCP.EXE` rewritten): program sequence, passwords, save slots | `src/rt/main.c` |
| DOS (`int 21h`) on Windows files, DPMI (`int 31h`), VGA BIOS, keyboard, MSCDEX (incl. sector reads for the FMV streams) | `src/rt/dos.c`, `dpmi.c`, `bios.c`, `cdrom.c` |
| Timer and keyboard interrupts driven from the Windows clock and window events | `src/rt/irq.c` |
| Window, menu, palette, Direct3D 9 presenter (filters, FXAA, CAS sharpening) | `src/rt/video.c` |
| Higher internal resolution of the 3D game (BRender scene rendered again at N x size) | `src/rt/hires3d.c` |
| Miles Sound System (AIL 3) replaced at its API: sample mixer, streaming, XMIDI player on Windows MIDI | `src/rt/native_ail.c`, mapping per program by `tools/gen_ail_natives.py` |
| Native code for routines that modify themselves or jump into their own unrolled loops (BRP video frame decoders, the mine's texture floor) | `src/rt/native.c`, `tools/native_<module>.txt` |
| Installer: ISO 9660 / BIN+CUE reader, setup UI, uninstaller | `installer/` |

## Building

Needs MSYS2 (MINGW64, with `mingw-w64-x86_64-gcc`, `mingw-w64-x86_64-SDL2`, `make`) and Python 3
with `capstone` and `pillow`.

```sh
# 1. put your CD's files in gamedata/cd (the recompiler reads the six programs from there)
# 2. generate the C code and build
./build.sh gen        # or: make gen PY=<python with capstone> ; make -j16
# 3. the setup with the game inside
make release
```

Development: `game/hd` (drive C:) and `game/cd` (drive D:) next to `odyssey.exe` form the dev install;
`./run_test.sh <seconds> [--state <hex>]` runs hidden and muted (`--state 2` starts at the forest,
`84` at the command bunker). Debug switches: `AO_SHOT`, `AO_DUMP`, `AO_WATCH`, `AO_KEYS`, `AO_MIDILOG`,
`AO_WAVOUT` (see `src/rt/irq.c`, `main.c`, `native_ail.c`).

*Alien Odyssey* © Argonaut Software / Philips Media. This is a fan preservation project and is not
affiliated with them.
