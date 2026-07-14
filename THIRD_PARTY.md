# Third-party components

Vendored in `src/vendor/` (license files/headers are preserved in each
directory):

| Component | Version / commit | License | Notes |
|---|---|---|---|
| [Musashi](https://github.com/kstenerud/Musashi) (68000 CPU) | `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd` (v4.60) | MIT | `m68kconf.h` modified: configuration values only (68000-only build, interrupt-ack and instruction-hook callbacks enabled). All modifications marked with `[megadrive_raylib]` comments. |
| [z80](https://github.com/superzazu/z80) by superzazu (Z80 CPU) | `d64fe10a2274e5e40019b1086bf7d8990cbc5f23` | MIT | unmodified |
| [Nuked-OPN2](https://github.com/nukeykt/Nuked-OPN2) (YM2612 FM) | `335747d78cb0abbc3b55b004e62dad9763140115` (v1.0.9) | LGPL-2.1-or-later | unmodified. LGPL source-availability obligations are satisfied by this repository; if you fork this project into a closed-source binary you must still provide Nuked-OPN2 sources and the means to relink it. |

Fetched at build time (not vendored):

| Component | Version | License |
|---|---|---|
| [raylib](https://github.com/raysan5/raylib) | 5.5 (release tarball via CMake FetchContent) | zlib |

This project contains **no** SEGA code, BIOS, TMSS ROM, or game data. It is
not affiliated with or endorsed by SEGA. "Mega Drive" and "Genesis" are used
descriptively to identify the hardware being emulated.
