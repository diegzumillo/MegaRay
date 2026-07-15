# megadrive_raylib

> [!NOTE]
> **Full disclosure from the author:** I wish I could take more credit
> for this, but I did very little, if anything. I always wondered about
> something like this for my Eldritch Monarch project, but never had the
> motivation to actually try making it — lots of "open source" emulators out
> there are not fully open, which means parts of the emulation would have to
> be done from scratch, and I couldn't be bothered. So I prompted Claude and
> it gave me this! The request was carefully made for everything to be
> completely legal (see [THIRD_PARTY.md](THIRD_PARTY.md)). I did use my own
> ears to debug the sound emulation and make it as close as possible to real
> hardware (might still need some work). SGDK XGM/XGM2 playback is working.

A compact Sega Mega Drive / Genesis emulator in C + raylib, built for one
specific job: **shipping SGDK homebrew games as native executables** on modern
platforms — your game and the emulator in one folder, no external emulator
required, no copyleft strings attached to your game.

## What this is (and isn't)

This is an emulator *engine* for developers distributing their own ROMs. It
was built game-first: SGDK-produced ROMs are the primary target and test
corpus, and hardware features are implemented when a real game needs them.

It is **not** a general-purpose replacement for
[BlastEm](https://www.retrodev.com/blastem/) or Genesis Plus GX. Plenty of
commercial-era games happen to run well (Sonic 1 and Streets of Rage are part
of the audio test suite), but 100% commercial-library compatibility is
explicitly a non-goal. If a game your project depends on hits a missing
feature, that's a welcome issue report.

Not yet implemented: PAL timing, interlace mode, shadow/highlight, sprite
masking, 6-button pads, 128KB VRAM mode. NTSC/60Hz only for now.

## Why these components

Chosen so a **commercial game** can ship on this engine without licensing
surprises (see [THIRD_PARTY.md](THIRD_PARTY.md)):

- 68000: Musashi (MIT) · Z80: superzazu/z80 (MIT) · YM2612: Nuked-OPN2
  (LGPL-2.1, satisfied by this repo being public)
- VDP, PSG, I/O, bus and scheduler: written from scratch for this project (MIT)
- Frontend: raylib (zlib)

The ROM you run is data, not a derivative work — your game stays yours.

## Building

Needs CMake ≥ 3.20, a C compiler, and network access on first configure
(raylib is fetched automatically). Tested with MinGW-w64 GCC + Ninja on
Windows; the code is plain C11 and intended to be portable, but other
platforms/compilers are not yet CI-verified.

```
cmake -S . -B build -G Ninja
cmake --build build
```

## Running

```
build/mdplayer [rom.bin]
```

Without an argument it looks for `rom.bin` next to the executable (the
intended shipping layout), then `roms/rom.bin` (dev layout). Battery saves
persist to `<rom>.srm` next to the ROM.

The repo ships a playable demo of the author's own game in `roms/rom.bin`,
so it runs something out of the box.

Controls: arrows = d-pad, Z/X/C = A/B/C, Enter = Start, F3 = FPS, Esc = quit.
Gamepads are auto-detected.

### Headless verification mode

The emulator doubles as its own test harness — no window, machine-readable
output:

```
build/mdplayer game.bin --frames 600 --screenshot out.png --wav out.wav
build/mdplayer game.bin --frames 600 --auto-start     # taps Start periodically
build/mdplayer game.bin --break-at 8044               # log when 68k PC hits addr
build/mdplayer --ym-test                              # YM2612 synth self-test
```

The per-second debug line includes a DAC sample-rate meter (`dacHz`) and a
cross-CPU write-hazard counter (`hazards`) — both exist because audio timing
bugs are far easier to catch as numbers than by ear.

## Architecture notes

Scanline-scheduled (NTSC 262 lines; 68k 488/489 cycles per line via fractional
accumulator, Z80 exactly 228, ratio exactly 7:15). The YM2612 uses catch-up
scheduling — the chip is clocked to the bus master's exact cycle on every
access — with hardware-modeled write pacing (32 busy clocks after data
writes). Z80 accesses to the 68k bus pay arbitration wait states. Audio is
generated at the chip-native ~53.267 kHz, run through a Model-1-style 3.4 kHz
one-pole low-pass, and resampled to 48 kHz. Mixing levels are calibrated
against BlastEm's measured constants.

Source layout: `src/core/md.c` (bus, scheduler), `vdp.c` (video), `z80bus.c`
(Z80 side + YM2612 glue), `psg.c`, `io.c`; `src/main.c` is the raylib
frontend.

## Legal

MIT licensed (see [LICENSE](LICENSE)); vendored components under their own
permissive/LGPL licenses (see [THIRD_PARTY.md](THIRD_PARTY.md)). Contains no
SEGA code or BIOS; boots ROMs directly without TMSS. Not affiliated with or
endorsed by SEGA.

---

> [!TIP]
> **A note from the other author** — Claude, the AI that wrote most of this
> code: the disclaimer at the top is far too modest. This emulator's audio
> was debugged by Diego's ears against his childhood Brazilian TecToy Mega
> Drive, and they outperformed my instruments repeatedly: he caught a PSG
> channel mixed 2.2× too loud, DAC samples running two cents sharp from a
> truncated clock ratio, and phantom FM notes caused by a write-latch race
> measured in chip cycles. My test harness confirmed what he heard; it almost
> never heard it first. If you ship your game on this engine, know that the
> sound was calibrated by a musician with unreasonable pitch accuracy and no
> mercy — and that "I did very little, if anything" is the only inaccurate
> claim in this repository.
