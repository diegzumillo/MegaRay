# megadrive_raylib

A from-scratch Sega Mega Drive emulator in C + raylib, built to ship SGDK games
as native executables on modern platforms.

Scope: it only needs to run games we control the source of (SGDK-produced ROMs),
so obscure hardware edge cases are added on demand, not preemptively.

## Build

Requires CMake, a C compiler (MinGW-w64 tested), and Ninja. raylib is fetched
automatically.

```
cmake -S . -B build -G Ninja
cmake --build build
```

## Run

```
build/mdplayer.exe [rom.bin]          # defaults to roms/rom.bin
```

Controls: arrows = d-pad, Z/X/C = A/B/C, Enter = Start. Gamepads work too.
F3 shows FPS. Esc quits.

### Headless verification mode

```
build/mdplayer.exe rom.bin --frames 600 --screenshot out.png --wav out.wav
build/mdplayer.exe rom.bin --frames 600 --auto-start     # tap Start periodically
build/mdplayer.exe rom.bin --break-at 8044               # log when PC hits addr
build/mdplayer.exe --ym-test                             # YM2612 synth self-test
```

Runs without a window, then prints the 68k state, disassembly at PC, and
framebuffer stats. Useful for regression checks against a reference emulator
(BlastEm recommended).

## Architecture

- `src/core/md.c` — 68k memory map, Musashi glue, scanline scheduler, audio ring
- `src/core/vdp.c` — VDP: ports, DMA, HInt/VInt, scanline renderer (planes A/B,
  window, sprites, priority)
- `src/core/io.c` — version register, 3-button pads
- `src/core/psg.c` — SN76489
- `src/core/z80bus.c` — Z80 RAM/BUSREQ/RESET/banking, YM2612 write queue
- `src/vendor/` — Musashi 68000 (MIT), superzazu z80 (MIT), Nuked-OPN2 (LGPL-2.1)

Timing: NTSC scanline-based (262 lines, 488 68k cycles + 228 Z80 cycles per
line). YM2612 is clocked cycle-accurately via Nuked-OPN2; Z80→YM writes go
through a pacing queue because the chip's write latch needs clocks between
writes. Audio is generated at the chip-native ~53.267 kHz and resampled to
48 kHz.

Deliberately not implemented yet: PAL, interlace, shadow/highlight, sprite
masking, 6-button pads, save states, widescreen.
