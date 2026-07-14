/* md.h - public interface between the emulator core and the frontend */
#ifndef MD_H
#define MD_H

#include <stdint.h>
#include <stdbool.h>

#define MD_SCREEN_W 320
#define MD_SCREEN_H 224

/* pad button bits for md_set_pad() */
#define MD_BTN_UP    0x0001
#define MD_BTN_DOWN  0x0002
#define MD_BTN_LEFT  0x0004
#define MD_BTN_RIGHT 0x0008
#define MD_BTN_A     0x0010
#define MD_BTN_B     0x0020
#define MD_BTN_C     0x0040
#define MD_BTN_START 0x0080

bool md_load_rom(const char *path);
void md_reset(void);
void md_run_frame(void);
void md_flush_sram(void);   /* persist battery RAM if modified (<rom>.srm) */

/* RGBA8888, MD_SCREEN_W x MD_SCREEN_H. H32 content is centered. */
const uint32_t *md_framebuffer(void);

void md_set_pad(int pad, uint16_t buttons);

/* Pull resampled stereo interleaved s16 audio; returns frames written. */
int md_read_audio(int16_t *dst, int max_frames, int out_rate);

/* debugging helpers */
void md_add_break_pc(uint32_t pc, const char *label);
uint32_t md_get_pc(void);
uint64_t md_instr_count(void);

#endif
