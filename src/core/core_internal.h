/* core_internal.h - shared state between the tightly-coupled core modules */
#ifndef CORE_INTERNAL_H
#define CORE_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "md.h"

/* NTSC timing: master clock 53.693175 MHz, 3420 master cycles per line */
#define MD_LINES_TOTAL      262
#define MD_LINES_ACTIVE     224
#define M68K_CYCLES_LINE    488   /* 3420/7 = 488.57; true per-line budget is
                                     md_line_cycles - this is only for rough
                                     intra-line position estimates */
#define Z80_CYCLES_LINE     228   /* 3420 / 15, exact */
#define FM_SAMPLE_68K_CYC   144   /* one YM2612 sample per 144 68k cycles */

/* ---- md.c ---- */
extern uint8_t   md_rom[0x400000];
extern uint32_t  md_rom_size;
extern uint8_t   md_ram[0x10000];
extern uint32_t  md_fb[MD_SCREEN_W * MD_SCREEN_H];
extern int       md_line;             /* scanline currently executing */
extern int       md_line_68k_cycles;  /* 68k cycles consumed in current line (approx) */
extern uint64_t  md_cycle_base;       /* 68k cycles at the start of this line */
extern int       md_line_cycles;      /* this line's exact 68k budget (488/489) */

/* 68k bus accessors, also used by VDP DMA and the Z80 banked window */
unsigned int m68k_read_memory_8(unsigned int address);
unsigned int m68k_read_memory_16(unsigned int address);
void m68k_write_memory_16(unsigned int address, unsigned int value);

void md_audio_push(int16_t l, int16_t r); /* native-rate (~53.267 kHz) sample */

/* ---- vdp.c ---- */
void     vdp_reset(void);
void     vdp_write_data(uint16_t v);
void     vdp_write_control(uint16_t v);
uint16_t vdp_read_data(void);
uint16_t vdp_read_status(void);
uint16_t vdp_read_hv(void);
void     vdp_line_start(int line);  /* hint counter / vint, called per line */
void     vdp_render_line(int line);
void     vdp_int_ack(int level);

/* ---- io.c ---- */
void    io_reset(void);
uint8_t io_read(uint32_t addr);
void    io_write(uint32_t addr, uint8_t v);
void    io_set_pad(int pad, uint16_t buttons);

/* ---- psg.c ---- */
void    psg_reset(void);
void    psg_write(uint8_t v);
int16_t psg_sample(void);      /* advance one FM-rate sample, return mono */
int16_t psg_sample_last(void); /* same value again without advancing */

/* ---- z80bus.c ---- */
void     z80bus_reset(void);
uint8_t  z80bus_read8_from68k(uint32_t addr);
void     z80bus_write8_from68k(uint32_t addr, uint8_t v);
uint16_t z80bus_busreq_read(void);
void     z80bus_busreq_write(uint16_t v);
void     z80bus_reset_write(uint16_t v);
void     z80bus_run_line(void);      /* one scanline of Z80 execution */
void     z80bus_vint(void);          /* assert Z80 INT at vblank */
void     z80bus_ym_catchup(uint64_t target_68k_cycles); /* clock YM to time */
void     z80bus_debug(char *buf, int n);
int      z80bus_ym_selftest(void);

#endif
